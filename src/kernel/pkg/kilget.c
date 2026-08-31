/* Phase 4.4/4.5: kilget - repo client + apt-get-equivalent frontend. */

#include "pkg/kilget.h"
#include "pkg/dpkg.h"
#include "pkg/sha256.h"
#include "net/http.h"
#include "net/netif.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "fs/fs.h"
#include "mm/memory.h"
#include "shell/terminal.h"
#include "drivers/vga.h"

#define KILGET_INDEX   "/var/lib/kilget/Packages"
#define KILGET_SOURCES "/etc/kilget/sources.list"
#define KILGET_CACHE   "/var/cache/kilget"
#define KILGET_MAX_PKGS 64

#define TERM_OUT(s) term_puts(s)

typedef struct {
    char package[64];
    char version[64];
    char filename[192];
    char sha[65];
    uint32_t size;
    char depends[256];
} pkg_rec_t;

static pkg_rec_t recs[KILGET_MAX_PKGS];
static int       nrecs;

/* ---------- fs helpers (local copies; dpkg.c keeps its own) ---------- */

static int fs_read_all(const char* path, uint8_t** out, size_t* len) {
    fs_entry_t* f = fs_resolve_path(path);
    if (f == NULL || f->type != FS_TYPE_FILE) return -1;
    if (f->size == 0) { *out = NULL; *len = 0; return 0; }
    uint8_t* buf = (uint8_t*)kmalloc(f->size + 1);
    if (buf == NULL) return -1;
    int got = fs_read_file(f, buf, f->size);
    if (got < 0) { kfree(buf); return -1; }
    buf[got] = '\0';
    *out = buf;
    *len = (size_t)got;
    return 0;
}

/* mkdir -p the parent directory of path (path itself is a FILE; running
 * fs_mkdir_p on the full path would create the file as a DIRECTORY). */
static int fs_mkdir_parent(const char* path) {
    char buf[260];
    if (path == NULL || strlen(path) >= sizeof(buf)) return -1;
    strcpy(buf, path);
    char* last = strrchr(buf, '/');
    if (last == NULL || last == buf) return 0;   /* parent is root */
    *last = '\0';
    return fs_mkdir_p(buf);
}

static int fs_write_all(const char* path, const uint8_t* data, size_t len) {
    if (fs_mkdir_parent(path) != 0) return -1;
    fs_entry_t* f = fs_resolve_path(path);
    if (f == NULL) f = fs_create_file(path);
    if (f == NULL) return -1;
    return fs_write_file(f, data, len) >= 0 ? 0 : -1;
}

/* ---------- sources.list ---------- */

typedef struct {
    char     host[64];
    uint16_t port;
    char     base[160];   /* repo base path, no trailing slash */
} repo_src_t;

static int parse_sources(repo_src_t* src) {
    uint8_t* buf;
    size_t len;
    if (fs_read_all(KILGET_SOURCES, &buf, &len) != 0 || buf == NULL) {
        TERM_OUT("kilget: no sources.list - create ");
        TERM_OUT(KILGET_SOURCES);
        TERM_OUT("\n");
        return -1;
    }
    int rc = -1;
    char* p = (char*)buf;
    while (p && *p) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (*p == '#') { p = nl ? nl + 1 : NULL; continue; }
        /* format: deb http://host[:port]/path . */
        char* url = strstr(p, "http://");
        if (url) {
            url += 7;
            const char* path_start = strchr(url, '/');
            const char* colon = strchr(url, ':');
            size_t hlen = (colon && (!path_start || colon < path_start))
                          ? (size_t)(colon - url)
                          : (path_start ? (size_t)(path_start - url) : strlen(url));
            if (hlen == 0 || hlen >= sizeof(src->host)) { p = nl ? nl + 1 : NULL; continue; }
            memcpy(src->host, url, hlen);
            src->host[hlen] = '\0';

            src->port = 80;
            if (colon && (!path_start || colon < path_start)) {
                src->port = (uint16_t)atoi(colon + 1);
                if (src->port == 0) src->port = 80;
            }

            src->base[0] = '\0';
            if (path_start) {
                size_t plen = strlen(path_start);
                while (plen > 0 && (path_start[plen-1] == ' ' ||
                                    path_start[plen-1] == '\r')) plen--;
                /* strip the trailing "dist-style" component ("." or "./") */
                while (plen > 0 && (path_start[plen-1] == '.')) plen--;
                while (plen > 0 && path_start[plen-1] == '/') plen--;
                if (plen >= sizeof(src->base)) plen = sizeof(src->base) - 1;
                memcpy(src->base, path_start, plen);
                src->base[plen] = '\0';
            }
            rc = 0;
            break;
        }
        p = nl ? nl + 1 : NULL;
    }
    kfree(buf);
    if (rc != 0) {
        TERM_OUT("kilget: no deb line in sources.list\n");
    }
    return rc;
}

/* ---------- Packages index ---------- */

static void index_field(const char* para, size_t plen, const char* key,
                        char* out, size_t outsz) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char* p = para;
    const char* end = para + plen;
    while (p < end) {
        /* paragraphs are NUL-terminated within the loaded buffer */
        const char* eol = strchr(p, '\n');
        size_t linelen = (eol && eol < end) ? (size_t)(eol - p)
                                            : (size_t)(end - p);
        if (linelen > klen && strncmp(p, key, klen) == 0 && p[klen] == ':') {
            const char* v = p + klen + 1;
            while (*v == ' ') v++;
            size_t vlen = (size_t)(p + linelen - v);
            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return;
        }
        if (!eol || eol >= end) break;
        p = eol + 1;
    }
}

/* Reload /var/lib/kilget/Packages into the record array. */
static int index_load(void) {
    nrecs = 0;
    uint8_t* buf;
    size_t len;
    if (fs_read_all(KILGET_INDEX, &buf, &len) != 0 || buf == NULL) {
        TERM_OUT("kilget: no package index - run 'kilget update'\n");
        return -1;
    }

    char* p = (char*)buf;
    char* end = (char*)buf + len;
    while (p < end && nrecs < KILGET_MAX_PKGS) {
        char* next = strstr(p, "\n\n");
        size_t plen = next ? (size_t)(next - p) : (size_t)(end - p);
        if (plen == 0) break;

        pkg_rec_t* r = &recs[nrecs];
        memset(r, 0, sizeof(*r));
        index_field(p, plen, "Package", r->package, sizeof(r->package));
        index_field(p, plen, "Version", r->version, sizeof(r->version));
        index_field(p, plen, "Filename", r->filename, sizeof(r->filename));
        index_field(p, plen, "SHA256", r->sha, sizeof(r->sha));
        index_field(p, plen, "Depends", r->depends, sizeof(r->depends));
        char sizebuf[16];
        index_field(p, plen, "Size", sizebuf, sizeof(sizebuf));
        r->size = (uint32_t)strtoul(sizebuf, NULL, 10);

        if (r->package[0] && r->filename[0]) nrecs++;

        p = next ? next + 2 : end;
    }
    kfree(buf);
    if (nrecs == 0) {
        TERM_OUT("kilget: package index is empty\n");
        return -1;
    }
    return 0;
}

static int index_find(const char* package) {
    for (int i = 0; i < nrecs; i++)
        if (strcmp(recs[i].package, package) == 0) return i;
    return -1;
}

/* ---------- install planning (topological order) ---------- */

typedef struct {
    int  order[KILGET_MAX_PKGS];
    int  norder;
    char cycle[KILGET_MAX_PKGS];  /* visit marks: 0 new, 1 visiting, 2 done */
    int  failed;
} plan_t;

/* First alternatives of the Depends list. Parsing happens ENTIRELY here:
 * plan_visit recurses and strtok keeps global state, so a caller-side
 * strtok loop would be corrupted by the nested plan_dep_names call
 * (observed: 'depends on status' - a token scan across buffers). */
static int plan_dep_names(const char* depends, char out[][64], int max) {
    if (!depends || !depends[0]) return 0;
    char deps[256];
    strncpy(deps, depends, sizeof(deps) - 1);
    deps[sizeof(deps) - 1] = '\0';
    int n = 0;
    for (char* item = strtok(deps, ","); item && n < max; item = strtok(NULL, ",")) {
        char* bar = strchr(item, '|');
        if (bar) *bar = '\0';
        char* paren = strchr(item, '(');
        if (paren) *paren = '\0';
        while (*item == ' ' || *item == '\t') item++;
        char* e = item + strlen(item);
        while (e > item && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n')) e--;
        *e = '\0';
        if (*item == '\0') continue;
        strncpy(out[n], item, sizeof(out[n]) - 1);
        out[n][sizeof(out[n]) - 1] = '\0';
        n++;
    }
    return n;
}

static void plan_visit(plan_t* plan, int idx, int depth) {
    if (plan->failed || depth > 16) { plan->failed = 1; return; }
    if (plan->cycle[idx] == 2) return;
    if (plan->cycle[idx] == 1) {
        /* Ignore the back edge, don't fail: real packages contain
         * dependency cycles (libc6 <-> libgcc-s1). Starting from the
         * deepest dependency still yields a workable order, and dpkg
         * re-checks Depends at install time anyway. */
        return;
    }
    plan->cycle[idx] = 1;

    char names[16][64];
    int nn = plan_dep_names(recs[idx].depends, names, 16);
    for (int i = 0; i < nn; i++) {
        const char* tok = names[i];
        if (dpkg_is_installed(tok)) continue;
        int di = index_find(tok);
        if (di < 0) {
            TERM_OUT("kilget: ");
            TERM_OUT(recs[idx].package);
            TERM_OUT(" depends on '");
            TERM_OUT(tok);
            TERM_OUT("' which is not in the index\n");
            plan->failed = 1;
            return;
        }
        plan_visit(plan, di, depth + 1);
        if (plan->failed) return;
    }
    plan->cycle[idx] = 2;
    plan->order[plan->norder++] = idx;
}

/* ---------- download + verify ---------- */

static int fetch_and_install(const repo_src_t* src, int idx) {
    pkg_rec_t* r = &recs[idx];

    /* Filename is relative to the repo base; normalize "./a/b" -> "/a/b" */
    const char* fname = r->filename;
    if (fname[0] == '.' && fname[1] == '/') fname += 2;

    char path[256];
    {
        size_t off = 0;
        const char* seg = fname;
        if (src->base[0])
            ksprintf(path, sizeof(path), "/%s", src->base),
            off = strlen(path);
        while (seg && *seg) {
            const char* slash = strchr(seg, '/');
            size_t slen = slash ? (size_t)(slash - seg) : strlen(seg);
            if (slen > 0 && !(slen == 1 && seg[0] == '.')) {
                if (off + slen + 1 < sizeof(path)) {
                    path[off++] = '/';
                    memcpy(path + off, seg, slen);
                    off += slen;
                }
            }
            seg = slash ? slash + 1 : NULL;
        }
        path[off] = '\0';
    }

    const char* base = fname;
    for (const char* q = fname; *q; q++)
        if (*q == '/') base = q + 1;
    char cache[128];
    ksprintf(cache, sizeof(cache), KILGET_CACHE "/%s", base);

    TERM_OUT("kilget: downloading ");
    TERM_OUT(fname);
    TERM_OUT(" ...\n");

    uint8_t* body;
    size_t blen;
    if (http_download(&g_netif, src->host, src->port, path, &body, &blen) != 0) {
        TERM_OUT("kilget: download failed (");
        TERM_OUT(fname);
        TERM_OUT(")\n");
        klog("[kilget] download failed: ");
        klog(fname);
        klog("\n");
        return -1;
    }

    /* checksum + size verification against the index */
    if (r->sha[0]) {
        uint8_t digest[32];
        char hex[65];
        sha256(body, blen, digest);
        sha256_hex(digest, hex);
        if (strcmp(hex, r->sha) != 0) {
            TERM_OUT("kilget: SHA256 mismatch for ");
            TERM_OUT(fname);
            TERM_OUT("\n");
            klog("[kilget] sha256 mismatch: ");
            klog(fname);
            klog("\n");
            kfree(body);
            return -1;
        }
    }
    if (r->size && blen != r->size) {
        char want[16], got[16];
        utoa(r->size, want, 10, sizeof(want));
        utoa((uint32_t)blen, got, 10, sizeof(got));
        TERM_OUT("kilget: size mismatch for ");
        TERM_OUT(fname);
        TERM_OUT(" (want ");
        TERM_OUT(want);
        TERM_OUT(" got ");
        TERM_OUT(got);
        TERM_OUT(")\n");
        kfree(body);
        return -1;
    }

    if (fs_write_all(cache, body, blen) != 0) {
        TERM_OUT("kilget: cache write failed\n");
        kfree(body);
        return -1;
    }
    kfree(body);

    if (dpkg_install_file(cache) != 0) {
        klog("[kilget] dpkg install failed: ");
        klog(r->package);
        klog("\n");
        return -1;
    }
    /* Drop the cached .deb: the 16 MB RAM disk has no room to keep both
     * the archive and its extracted payload (libc6 alone is ~2.7 + 3.2 MB). */
    fs_delete_entry(cache);
    klog("[kilget] installed ");
    klog(r->package);
    klog("\n");
    return 0;
}

/* ---------- public API ---------- */

int kilget_update(void) {
    repo_src_t src;
    if (parse_sources(&src) != 0) return -1;

    char path[256];
    if (src.base[0])
        ksprintf(path, sizeof(path), "/%s/Packages", src.base);
    else
        ksprintf(path, sizeof(path), "/Packages");

    TERM_OUT("kilget: update from http://");
    TERM_OUT(src.host);
    TERM_OUT(":");
    char pb[8];
    itoa(src.port, pb, 10, sizeof(pb));
    TERM_OUT(pb);
    TERM_OUT(path);
    TERM_OUT("\n");

    uint8_t* body;
    size_t blen;
    if (http_download(&g_netif, src.host, src.port, path, &body, &blen) != 0) {
        TERM_OUT("kilget: update failed\n");
        klog("[kilget] index download failed\n");
        return -1;
    }
    if (fs_write_all(KILGET_INDEX, body, blen) != 0) {
        kfree(body);
        TERM_OUT("kilget: failed to store index\n");
        return -1;
    }
    kfree(body);
    TERM_OUT("kilget: index updated\n");
    klog("[kilget] index updated (");
    char nb[12];
    itoa((int)blen, nb, 10, sizeof(nb));
    klog(nb);
    klog(" bytes)\n");
    return 0;
}

int kilget_install(const char* package) {
    if (index_load() != 0) return -1;
    if (dpkg_is_installed(package)) {
        TERM_OUT("kilget: ");
        TERM_OUT(package);
        TERM_OUT(" is already installed\n");
        return 0;
    }
    int idx = index_find(package);
    if (idx < 0) {
        TERM_OUT("kilget: package '");
        TERM_OUT(package);
        TERM_OUT("' not found in the index\n");
        return -1;
    }

    plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan_visit(&plan, idx, 0);
    if (plan.failed || plan.norder == 0) {
        TERM_OUT("kilget: unable to resolve dependencies\n");
        return -1;
    }

    repo_src_t src;
    if (parse_sources(&src) != 0) return -1;

    /* Transaction mode: the planner may not be able to satisfy every
     * Depends before its member installs (real cycles like
     * libc6 <-> libgcc-s1), so missing deps only warn inside the
     * transaction. The final state after the loop is consistent. */
    dpkg_set_force_deps(1);
    for (int i = 0; i < plan.norder; i++) {
        int ri = plan.order[i];
        if (dpkg_is_installed(recs[ri].package)) continue;
        if (fetch_and_install(&src, ri) != 0) {
            dpkg_set_force_deps(0);
            TERM_OUT("kilget: install aborted at ");
            TERM_OUT(recs[ri].package);
            TERM_OUT("\n");
            return -1;
        }
    }
    dpkg_set_force_deps(0);
    klog("[kilget] install complete: ");
    klog(package);
    klog("\n");
    return 0;
}

void kilget_show(const char* package) {
    if (index_load() != 0) return;
    int idx = index_find(package);
    if (idx < 0) {
        TERM_OUT("kilget: package '");
        TERM_OUT(package);
        TERM_OUT("' not found\n");
        return;
    }
    pkg_rec_t* r = &recs[idx];
    TERM_OUT("Package: ");   TERM_OUT(r->package);   TERM_OUT("\n");
    TERM_OUT("Version: ");   TERM_OUT(r->version);   TERM_OUT("\n");
    TERM_OUT("Filename: ");  TERM_OUT(r->filename);  TERM_OUT("\n");
    TERM_OUT("SHA256: ");    TERM_OUT(r->sha);       TERM_OUT("\n");
    char sb[16];
    utoa(r->size, sb, 10, sizeof(sb));
    TERM_OUT("Size: ");      TERM_OUT(sb);           TERM_OUT("\n");
    TERM_OUT("Depends: ");   TERM_OUT(r->depends);   TERM_OUT("\n");
}

void kilget_list(void) {
    if (index_load() != 0) return;
    for (int i = 0; i < nrecs; i++) {
        TERM_OUT(recs[i].package);
        TERM_OUT("  ");
        TERM_OUT(recs[i].version);
        TERM_OUT(dpkg_is_installed(recs[i].package) ? "  [installed]\n" : "\n");
    }
}
