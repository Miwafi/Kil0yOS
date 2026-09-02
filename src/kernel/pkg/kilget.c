/* Phase 4.4/4.5: kilget - repo client + apt-get-equivalent frontend. */

#include "pkg/kilget.h"
#include "pkg/dpkg.h"
#include "pkg/sha256.h"
#include "pkg/inflate.h"
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
/* Real dists indexes (e.g. jammy main) carry ~53000 paragraphs; the
 * record table must hold them ALL or alphabetically-late packages
 * silently vanish - at 4096 entries the table cut off right between
 * libx11-6 (~#4000) and libxcb1, breaking "apt-get install libx11-dev"
 * with "depends on 'libxcb1' which is not in the index". 65536 entries
 * cost ~59 MB of static bss - fine next to a 500 MB heap. */
#define KILGET_MAX_PKGS 65536

#define TERM_OUT(s) term_puts(s)

typedef struct {
    char package[64];
    char version[64];
    char filename[192];
    char sha[65];
    uint32_t size;
    char depends[512];
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
    char     suite[64];   /* dists suite (e.g. "jammy"); empty = flat repo */
    char     comps[8][32];
    int      ncomps;
} repo_src_t;

/* Tokenize one line in place: split on spaces/tabs but keep bracketed
 * apt options ("[arch=amd64 trusted=yes]") as a single token. Returns
 * the token count (at most maxv). */
static int line_tokens(char* line, char** tokv, int maxv) {
    int n = 0;
    char* p = line;
    while (*p && n < maxv) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        tokv[n++] = p;
        if (*p == '[') {
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) { *p = '\0'; p++; }
    }
    return n;
}

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
        if (*p != '#') {
            /* format (flat):   deb http://host[:port][/base] .
             * format (dists):  deb http://host[:port]/base SUITE COMP... */
            char* tokv[16];
            int ntok = line_tokens(p, tokv, 16);
            for (int i = 0; i < ntok; i++) {
                char* url = strstr(tokv[i], "http://");
                if (url == NULL) continue;
                url += 7;
                const char* path_start = strchr(url, '/');
                const char* colon = strchr(url, ':');
                size_t hlen = (colon && (!path_start || colon < path_start))
                              ? (size_t)(colon - url)
                              : (path_start ? (size_t)(path_start - url) : strlen(url));
                if (hlen == 0 || hlen >= sizeof(src->host)) continue;
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

                /* suite + components: tokens after the URL, skipping
                 * bracketed options like [arch=amd64 trusted=yes].
                 * A "." suite means flat-repo style (old format). */
                src->suite[0] = '\0';
                src->ncomps = 0;
                int j = i + 1;
                while (j < ntok && tokv[j][0] == '[') j++;
                if (j < ntok && strcmp(tokv[j], ".") != 0) {
                    size_t slen = strlen(tokv[j]);
                    if (slen >= sizeof(src->suite)) slen = sizeof(src->suite) - 1;
                    memcpy(src->suite, tokv[j], slen);
                    src->suite[slen] = '\0';
                    j++;
                    while (j < ntok && src->ncomps < 8) {
                        if (tokv[j][0] == '[') break;
                        size_t clen = strlen(tokv[j]);
                        if (clen >= sizeof(src->comps[0])) clen = sizeof(src->comps[0]) - 1;
                        memcpy(src->comps[src->ncomps], tokv[j], clen);
                        src->comps[src->ncomps][clen] = '\0';
                        src->ncomps++;
                        j++;
                    }
                }
                rc = 0;
                break;
            }
            if (rc == 0) break;   /* first valid deb line wins (one source) */
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

/* Extract one Packages paragraph (Debian RFC822-ish format) into the
 * record table. Shared by the streaming feeder and index_load. */
static void parse_record(const char* para, size_t plen) {
    if (nrecs >= KILGET_MAX_PKGS) return;
    pkg_rec_t* r = &recs[nrecs];
    memset(r, 0, sizeof(*r));
    index_field(para, plen, "Package", r->package, sizeof(r->package));
    index_field(para, plen, "Version", r->version, sizeof(r->version));
    index_field(para, plen, "Filename", r->filename, sizeof(r->filename));
    index_field(para, plen, "SHA256", r->sha, sizeof(r->sha));
    index_field(para, plen, "Depends", r->depends, sizeof(r->depends));
    char sizebuf[16];
    index_field(para, plen, "Size", sizebuf, sizeof(sizebuf));
    r->size = (uint32_t)strtoul(sizebuf, NULL, 10);
    if (r->package[0] && r->filename[0]) nrecs++;
}

/* Streaming feeder: assembles paragraphs from inflate chunks. Real
 * mirrors ship ~48 MB of plain Packages text (jammy main); buffering
 * that whole output is impossible on this kernel, so records are
 * parsed as they stream past and only the extracted fields are kept. */
typedef struct {
    char*  buf;      /* current paragraph */
    size_t len, cap;
    int    nl_run;   /* consecutive newlines seen (paragraph separator) */
    int    overflow; /* paragraph larger than the cap: skip it */
} index_feed_t;

#define FEED_PARA_MAX (256 * 1024)

static void feed_append(index_feed_t* f, char c) {
    if (f->len + 1 >= f->cap) {
        if (f->cap >= FEED_PARA_MAX) { f->overflow = 1; return; }
        size_t ncap = f->cap ? f->cap * 2 : 8192;
        char* nb = (char*)krealloc(f->buf, ncap);
        if (nb == NULL) { f->overflow = 1; return; }
        f->buf = nb;
        f->cap = ncap;
    }
    f->buf[f->len++] = c;
}

static int index_feed(void* ctx, const uint8_t* d, size_t n) {
    index_feed_t* f = (index_feed_t*)ctx;
    for (size_t i = 0; i < n; i++) {
        char c = (char)d[i];
        if (c == '\n') {
            if (f->nl_run >= 1) {
                f->nl_run++;
                if (f->nl_run == 2) {   /* blank line = record separator */
                    if (!f->overflow && f->len > 0) parse_record(f->buf, f->len);
                    f->len = 0;
                    f->overflow = 0;
                }
                /* runs of 3+ newlines stay in separator state */
            } else {
                f->nl_run = 1;
                feed_append(f, c);   /* line terminator inside a record */
            }
        } else {
            f->nl_run = 0;
            feed_append(f, c);
        }
    }
    return 0;
}

/* Flush the trailing paragraph that has no blank line after it. */
static void index_feed_finish(index_feed_t* f) {
    if (!f->overflow && f->len > 0) parse_record(f->buf, f->len);
    if (f->buf) kfree(f->buf);
    f->buf = NULL;
    f->len = f->cap = 0;
    f->nl_run = 0;
    f->overflow = 0;
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

        parse_record(p, plen);

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
    char deps[512];
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

/* Persist the parsed record table as a compact Packages-format index.
 * The raw dists index is far too large for the FAT ramdisk (jammy main
 * is ~48 MB plain), but we only ever need the extracted fields, and the
 * regenerated text (~0.9 MB) parses back through index_load unchanged. */
static int index_store_compact(void) {
    size_t cap = 768 * (size_t)nrecs + 64;
    char* buf = (char*)kmalloc(cap);
    if (buf == NULL) {
        klog("[kilget] compact index alloc failed\n");
        return -1;
    }
    size_t off = 0;
    char tmp[768];
    for (int i = 0; i < nrecs; i++) {
        pkg_rec_t* r = &recs[i];
        ksprintf(tmp, sizeof(tmp),
                 "Package: %s\nVersion: %s\nFilename: %s\nSize: %u\n",
                 r->package, r->version, r->filename, r->size);
        size_t tlen = strlen(tmp);
        if (r->sha[0]) {
            ksprintf(tmp + tlen, sizeof(tmp) - tlen, "SHA256: %s\n", r->sha);
            tlen += strlen(tmp + tlen);
        }
        if (r->depends[0]) {
            ksprintf(tmp + tlen, sizeof(tmp) - tlen, "Depends: %s\n", r->depends);
            tlen += strlen(tmp + tlen);
        }
        ksprintf(tmp + tlen, sizeof(tmp) - tlen, "\n");
        tlen += strlen(tmp + tlen);
        if (off + tlen > cap) break;
        memcpy(buf + off, tmp, tlen);
        off += tlen;
    }
    int rc = fs_write_all(KILGET_INDEX, (const uint8_t*)buf, off);
    kfree(buf);
    if (rc != 0) klog("[kilget] compact index store failed\n");
    return rc;
}

/* Fetch one component's Packages index for a dists-layout repo and
 * stream-parse it straight into the record table. Tries Packages.gz
 * first (kernel-side streaming gunzip), falls back to plain Packages
 * for servers without compression. Returns 0 on success. */
static int fetch_component_index(const repo_src_t* src, const char* comp) {
    char gzpath[256];
    if (src->base[0])
        ksprintf(gzpath, sizeof(gzpath), "/%s/dists/%s/%s/binary-amd64/Packages.gz",
                 src->base, src->suite, comp);
    else
        ksprintf(gzpath, sizeof(gzpath), "/dists/%s/%s/binary-amd64/Packages.gz",
                 src->suite, comp);

    uint8_t* body = NULL;
    size_t blen = 0;
    int is_gz = 0;
    if (http_download(&g_netif, src->host, src->port, gzpath, &body, &blen) != 0) {
        /* plain fallback: same path minus the .gz suffix */
        char plain[256];
        strncpy(plain, gzpath, sizeof(plain) - 1);
        plain[sizeof(plain) - 1] = '\0';
        size_t plen_ = strlen(plain);
        if (plen_ > 3 && strcmp(plain + plen_ - 3, ".gz") == 0) plain[plen_ - 3] = '\0';
        if (http_download(&g_netif, src->host, src->port, plain, &body, &blen) != 0) {
            TERM_OUT("kilget: component '");
            TERM_OUT(comp);
            TERM_OUT("' has no index (tried .gz and plain), skipped\n");
            klog("[kilget] component index fetch failed: ");
            klog(comp);
            klog("\n");
            return -1;
        }
    } else {
        is_gz = 1;
    }

    index_feed_t f;
    memset(&f, 0, sizeof(f));
    int rc = 0;
    if (is_gz) {
        size_t out_len = 0;
        if (gzip_inflate_cb(body, blen, index_feed, &f, &out_len) != 0) {
            TERM_OUT("kilget: gunzip failed for ");
            TERM_OUT(comp);
            TERM_OUT("\n");
            klog("[kilget] bad gzip index: ");
            klog(comp);
            klog("\n");
            rc = -1;
        }
    } else {
        if (index_feed(&f, body, blen) != 0) rc = -1;
    }
    index_feed_finish(&f);
    kfree(body);
    return rc;
}

int kilget_update(void) {
    repo_src_t src;
    if (parse_sources(&src) != 0) return -1;

    char path[256];
    if (src.suite[0]) {
        /* dists layout: stream-parse dists/<suite>/<comp>/binary-amd64/
         * Packages.gz for every component into the record table, then
         * persist a compact regenerated index */
        nrecs = 0;
        int ok = 0;
        for (int c = 0; c < src.ncomps; c++)
            if (fetch_component_index(&src, src.comps[c]) == 0)
                ok++;
        if (ok == 0 || nrecs == 0) {
            TERM_OUT("kilget: update failed - no component index available\n");
            klog("[kilget] index download failed (dists)\n");
            return -1;
        }
        TERM_OUT("kilget: update from http://");
        TERM_OUT(src.host);
        TERM_OUT(":");
        char pb[8];
        itoa(src.port, pb, 10, sizeof(pb));
        TERM_OUT(pb);
        TERM_OUT(src.base[0] ? src.base : "/");
        TERM_OUT(" ");
        TERM_OUT(src.suite);
        TERM_OUT(" (");
        char cb[8];
        itoa(ok, cb, 10, sizeof(cb));
        TERM_OUT(cb);
        TERM_OUT(" components)\n");

        if (index_store_compact() != 0) {
            TERM_OUT("kilget: failed to store index\n");
            return -1;
        }
        TERM_OUT("kilget: index updated (");
        char nb[12];
        itoa(nrecs, nb, 10, sizeof(nb));
        TERM_OUT(nb);
        TERM_OUT(" packages, ");
        TERM_OUT(cb);
        TERM_OUT(" components)\n");
        klog("[kilget] index updated (");
        klog(nb);
        klog(" packages, dists ");
        klog(src.suite);
        klog(")\n");
        return 0;
    }

    /* flat layout: <base>/Packages */
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
    nrecs = 0;
    index_feed_t f;
    memset(&f, 0, sizeof(f));
    size_t isz = blen;
    if (blen > 2 && body[0] == 0x1F && body[1] == 0x8B) {
        if (gzip_inflate_cb(body, blen, index_feed, &f, &isz) != 0) {
            kfree(body);
            index_feed_finish(&f);
            TERM_OUT("kilget: gunzip failed\n");
            return -1;
        }
    } else {
        index_feed(&f, body, blen);
    }
    index_feed_finish(&f);
    kfree(body);
    if (nrecs == 0 || index_store_compact() != 0) {
        TERM_OUT("kilget: failed to store index\n");
        return -1;
    }
    TERM_OUT("kilget: index updated\n");
    klog("[kilget] index updated (");
    char nb[12];
    itoa(nrecs, nb, 10, sizeof(nb));
    klog(nb);
    klog(" packages)\n");
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

    /* plan_t with KILGET_MAX_PKGS=65536 is ~320 KB - far beyond the
     * 16 KB kernel stack, so it lives in static storage (shell context
     * only). */
    static plan_t plan;
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
