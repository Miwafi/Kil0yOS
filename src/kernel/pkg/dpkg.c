/* Phase 4.2: minimal dpkg frontend.
 *
 * Database layout (same shape as real dpkg, subset of fields):
 *   /var/lib/dpkg/status          - package paragraphs (RFC822-ish)
 *   /var/lib/dpkg/info/<pkg>.list - absolute paths owned by <pkg>
 *
 * Everything lives in the fs overlay (MEM backend in ext2 mode), so the
 * database and installed files share the persistence semantics of every
 * other runtime write in this kernel. */

#include "pkg/dpkg.h"
#include "pkg/deb.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "fs/fs.h"
#include "mm/memory.h"
#include "shell/terminal.h"
#include "drivers/vga.h"

#define DPKG_DIR     "/var/lib/dpkg"
#define DPKG_INFO    DPKG_DIR "/info"
#define DPKG_STATUS  DPKG_DIR "/status"

#define TERM_OUT(s) term_puts(s)

/* Transaction mode flag (dpkg_set_force_deps): missing Depends only
 * warns while set. */
static int force_deps = 0;

int dpkg_set_force_deps(int enable) {
    int prev = force_deps;
    force_deps = enable;
    return prev;
}

/* ---------- small fs helpers ---------- */

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
    /* remove existing so creation hits a free slot (overwrite keeps the
     * same node, but removing first also resets ext2 overlay cleanly) */
    fs_entry_t* f = fs_resolve_path(path);
    if (f == NULL) {
        f = fs_create_file(path);
    }
    if (f == NULL) return -1;
    return fs_write_file(f, data, len) >= 0 ? 0 : -1;
}

/* ---------- status database ---------- */

typedef struct {
    char*  buf;      /* whole status file (owned) */
    size_t len;
} status_db_t;

static int status_load(status_db_t* db) {
    db->buf = NULL;
    db->len = 0;
    uint8_t* raw = NULL;
    size_t rawlen = 0;
    if (fs_read_all(DPKG_STATUS, &raw, &rawlen) != 0 || raw == NULL) {
        if (raw) kfree(raw);
        return 0;
    }
    db->buf = (char*)raw;
    db->len = rawlen;
    return 0;
}

static void status_free(status_db_t* db) {
    if (db->buf) kfree(db->buf);
    db->buf = NULL;
    db->len = 0;
}

/* Iterate paragraphs: returns start of paragraph i or NULL. */
static char* status_paragraph(status_db_t* db, int i) {
    if (db->buf == NULL) return NULL;
    char* p = db->buf;
    int idx = 0;
    while (p && *p) {
        if (idx == i) return p;
        char* next = strstr(p, "\n\n");
        if (next == NULL) break;
        p = next + 2;
        idx++;
    }
    return NULL;
}

/* Field value within a paragraph ("Package", "Status", ...). */
static int para_field(const char* para, const char* field,
                      char* out, size_t outsz) {
    size_t flen = strlen(field);
    const char* p = para;
    while (p && *p) {
        const char* eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > flen && strncmp(p, field, flen) == 0 &&
            p[flen] == ':') {
            const char* v = p + flen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vlen = (size_t)(p + linelen - v);
            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (eol == NULL) break;
        p = eol + 1;
    }
    return 0;
}

/* Copy out every paragraph EXCEPT the one for `package` into a fresh
 * heap buffer. Returns NULL when the db is empty. */
static char* status_without(const char* package) {
    status_db_t db;
    status_load(&db);
    if (db.buf == NULL) { status_free(&db); return NULL; }

    char* out = (char*)kmalloc(db.len + 2);
    if (out == NULL) { status_free(&db); return NULL; }
    out[0] = '\0';
    size_t olen = 0;

    char* p = db.buf;
    while (p && *p) {
        char* next = strstr(p, "\n\n");
        size_t plen = next ? (size_t)(next - p) + 2 : strlen(p);
        char name[64];
        if (!(para_field(p, "Package", name, sizeof(name)) &&
              strcmp(name, package) == 0)) {
            memcpy(out + olen, p, plen);
            olen += plen;
            out[olen] = '\0';
        }
        p = next ? next + 2 : NULL;
    }
    status_free(&db);
    return out;
}

int dpkg_is_installed(const char* package) {
    status_db_t db;
    status_load(&db);
    int installed = 0;
    char* p = status_paragraph(&db, 0);
    for (int i = 0; p; i++) {
        char name[64], st[64];
        if (para_field(p, "Package", name, sizeof(name)) &&
            strcmp(name, package) == 0) {
            if (para_field(p, "Status", st, sizeof(st)) &&
                strstr(st, "installed") != NULL) {
                installed = 1;
            }
            break;
        }
        p = status_paragraph(&db, i + 1);
    }
    status_free(&db);
    return installed;
}

/* ---------- dependency checking ---------- */

/* Check every comma-separated Depends entry; each accepts the first
 * alternative ("a | b | c"). Returns 0 if all satisfied, -1 otherwise
 * (first missing dependency copied to missing). */
static int dpkg_check_deps(const char* depends, char* missing, size_t msz) {
    if (depends == NULL || depends[0] == '\0') return 0;

    char deps[256];
    strncpy(deps, depends, sizeof(deps) - 1);
    deps[sizeof(deps) - 1] = '\0';

    for (char* item = strtok(deps, ","); item; item = strtok(NULL, ",")) {
        /* first alternative only */
        char* bar = strchr(item, '|');
        if (bar) *bar = '\0';
        /* strip version constraint "(...)" */
        char* paren = strchr(item, '(');
        if (paren) *paren = '\0';
        /* trim */
        while (*item == ' ' || *item == '\t') item++;
        char* end = item + strlen(item);
        while (end > item && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) end--;
        *end = '\0';
        if (*item == '\0') continue;
        if (!dpkg_is_installed(item)) {
            if (missing && msz) {
                strncpy(missing, item, msz - 1);
                missing[msz - 1] = '\0';
            }
            return -1;
        }
    }
    return 0;
}

/* ---------- info/<pkg>.list ---------- */

static int info_list_path(const char* package, char* out, size_t outsz) {
    ksprintf(out, outsz, DPKG_INFO "/%s.list", package);
    return strlen(out) < outsz ? 0 : -1;
}

static int info_write_list(const char* package, deb_result_t* deb) {
    char path[96];
    if (info_list_path(package, path, sizeof(path)) != 0) return -1;

    size_t cap = 1;
    for (int i = 0; i < deb->nfiles; i++) cap += strlen(deb->installed_files[i]) + 1;
    char* buf = (char*)kmalloc(cap + 1);
    if (buf == NULL) return -1;
    size_t off = 0;
    for (int i = 0; i < deb->nfiles; i++) {
        strcpy(buf + off, deb->installed_files[i]);
        off += strlen(deb->installed_files[i]);
        buf[off++] = '\n';
    }
    buf[off] = '\0';
    int rc = fs_write_all(path, (const uint8_t*)buf, off);
    kfree(buf);
    return rc;
}

static int info_read_list(const char* package, char** out, size_t* len) {
    char path[96];
    if (info_list_path(package, path, sizeof(path)) != 0) return -1;
    uint8_t* raw = NULL;
    size_t rawlen = 0;
    int rc = fs_read_all(path, &raw, &rawlen);
    *out = (char*)raw;
    *len = rawlen;
    return rc;
}

static void info_delete_list(const char* package) {
    char path[96];
    if (info_list_path(package, path, sizeof(path)) != 0) return;
    fs_delete_entry(path);
}

/* 1 if file is mentioned in any other package's .list (shared file) */
static int file_owned_by_other(const char* package, const char* file) {
    fs_entry_t* dir = fs_resolve_path(DPKG_INFO);
    if (dir == NULL || dir->type != FS_TYPE_DIRECTORY) return 0;
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        fs_entry_t* c = dir->children[i];
        if (c == NULL || c->type != FS_TYPE_FILE) continue;
        /* name is "<pkg>.list" */
        size_t nl = strlen(c->name);
        if (nl < 6 || strcmp(c->name + nl - 5, ".list") != 0) continue;
        char other[64];
        strncpy(other, c->name, sizeof(other) - 1);
        other[sizeof(other) - 1] = '\0';
        other[nl - 5] = '\0';
        if (strcmp(other, package) == 0) continue;

        char* buf;
        size_t blen;
        if (info_read_list(other, &buf, &blen) != 0) continue;
        int owned = 0;
        if (buf) {
            size_t flen = strlen(file);
            const char* p = buf;
            while (p && *p) {
                if (strncmp(p, file, flen) == 0 &&
                    (p[flen] == '\n' || p[flen] == '\0')) { owned = 1; break; }
                p = strchr(p, '\n');
                if (p) p++;
            }
            kfree(buf);
        }
        if (owned) return 1;
    }
    return 0;
}

/* ---------- public API ---------- */

int dpkg_install_file(const char* deb_path) {
    /* read the .deb */
    uint8_t* deb_buf;
    size_t deb_len;
    if (fs_read_all(deb_path, &deb_buf, &deb_len) != 0) {
        TERM_OUT("dpkg: cannot read ");
        TERM_OUT(deb_path);
        TERM_OUT("\n");
        klog("[dpkg] cannot read ");
        klog(deb_path);
        klog("\n");
        return -1;
    }
    if (deb_buf == NULL || deb_len < 64) {
        if (deb_buf) kfree(deb_buf);
        TERM_OUT("dpkg: ");
        TERM_OUT(deb_path);
        TERM_OUT(" is not a .deb archive\n");
        klog("[dpkg] too small: ");
        klog(deb_path);
        klog("\n");
        return -1;
    }

    /* deb_result_t is ~33KB (256 file slots) - heap, not the 16KB stack */
    deb_result_t* deb = (deb_result_t*)kmalloc(sizeof(deb_result_t));
    if (deb == NULL) {
        kfree(deb_buf);
        TERM_OUT("dpkg: out of memory\n");
        return -1;
    }
    uint32_t magic0 = 0, magic1 = 0;
    if (deb_buf && deb_len >= 8) {
        memcpy(&magic0, deb_buf, 4);
        memcpy(&magic1, deb_buf + 4, 4);
    }
    int rc = deb_unpack(deb_buf, deb_len, deb);
    kfree(deb_buf);
    if (rc != 0) {
        kfree(deb);
        TERM_OUT("dpkg: unpack failed for ");
        TERM_OUT(deb_path);
        TERM_OUT("\n");
        klog("[dpkg] deb_unpack failed: len=");
        klog_hex("", (uint32_t)deb_len);
        klog(" magic=");
        klog_hex("", magic0);
        klog_hex(" ", magic1);
        klog("\n");
        return -1;
    }

    const char* pkg = deb->control.package;

    /* dependency pre-check (order is the caller's job). In transaction
     * mode a missing dep only warns: real-world dependency cycles make a
     * strictly-satisfying order impossible. */
    char missing[64];
    if (dpkg_check_deps(deb->control.depends, missing, sizeof(missing)) != 0) {
        TERM_OUT("dpkg: dependency problems - ");
        TERM_OUT(missing);
        TERM_OUT(" is not installed\n");
        klog("[dpkg] dep missing for ");
        klog(pkg);
        klog(": ");
        klog(missing);
        klog("\n");
        if (!force_deps) {
            kfree(deb);
            return -1;
        }
        klog("[dpkg] force-deps: continuing (transaction mode)\n");
    }

    /* record the file list; deb_unpack already extracted the payload
     * into the fs overlay */
    if (info_write_list(pkg, deb) != 0) {
        TERM_OUT("dpkg: failed to record file list for ");
        TERM_OUT(pkg);
        TERM_OUT("\n");
        klog("[dpkg] info list write failed: ");
        klog(pkg);
        klog("\n");
        kfree(deb);
        return -1;
    }

    /* rewrite status: drop old paragraph, append the new one */
    char* without = status_without(pkg);
    size_t need = (without ? strlen(without) : 0) + 320;
    char* status = (char*)kmalloc(need);
    if (status == NULL) {
        if (without) kfree(without);
        kfree(deb);
        return -1;
    }
    if (without) {
        strcpy(status, without);
        kfree(without);
    } else {
        status[0] = '\0';
    }
    size_t off = strlen(status);
    ksprintf(status + off, need - off,
             "Package: %s\n"
             "Status: install ok installed\n"
             "Version: %s\n"
             "Architecture: %s\n"
             "Depends: %s\n\n",
             pkg, deb->control.version, deb->control.architecture,
             deb->control.depends);
    int wrc = fs_write_all(DPKG_STATUS, (const uint8_t*)status, strlen(status));
    kfree(status);
    if (wrc != 0) {
        TERM_OUT("dpkg: failed to update status database\n");
        klog("[dpkg] status write failed\n");
        kfree(deb);
        return -1;
    }

    char num[16];
    itoa(deb->nfiles, num, 10, sizeof(num));
    TERM_OUT("dpkg: installed ");
    TERM_OUT(pkg);
    TERM_OUT(" ");
    TERM_OUT(deb->control.version);
    TERM_OUT(" (");
    TERM_OUT(num);
    TERM_OUT(" files)\n");
    klog("[dpkg] installed ");
    klog(pkg);
    klog(" ");
    klog(deb->control.version);
    klog("\n");
    kfree(deb);
    return 0;
}

int dpkg_remove(const char* package) {
    if (!dpkg_is_installed(package)) {
        TERM_OUT("dpkg: package '");
        TERM_OUT(package);
        TERM_OUT("' is not installed\n");
        return -1;
    }

    char* list;
    size_t llen;
    int removed = 0;
    if (info_read_list(package, &list, &llen) == 0 && list) {
        char* p = list;
        while (p && *p) {
            char* nl = strchr(p, '\n');
            size_t flen = nl ? (size_t)(nl - p) : strlen(p);
            char file[260];
            if (flen < sizeof(file)) {
                memcpy(file, p, flen);
                file[flen] = '\0';
                if (!file_owned_by_other(package, file)) {
                    if (fs_delete_entry(file) == 0) removed++;
                }
            }
            p = nl ? nl + 1 : NULL;
        }
        kfree(list);
    }
    info_delete_list(package);

    /* drop the status paragraph */
    char* without = status_without(package);
    if (without) {
        fs_write_all(DPKG_STATUS, (const uint8_t*)without, strlen(without));
        kfree(without);
    } else {
        fs_delete_entry(DPKG_STATUS);
    }

    char num[16];
    itoa(removed, num, 10, sizeof(num));
    TERM_OUT("dpkg: removed ");
    TERM_OUT(package);
    TERM_OUT(" (");
    TERM_OUT(num);
    TERM_OUT(" files)\n");
    klog("[dpkg] removed ");
    klog(package);
    klog("\n");
    return 0;
}

void dpkg_list(void) {
    status_db_t db;
    status_load(&db);
    char* p = status_paragraph(&db, 0);
    int n = 0;
    for (int i = 0; p; i++) {
        char name[64], ver[64], st[64];
        if (para_field(p, "Package", name, sizeof(name))) {
            para_field(p, "Version", ver, sizeof(ver));
            para_field(p, "Status", st, sizeof(st));
            int is_inst = strstr(st, "installed") != NULL;
            TERM_OUT(is_inst ? "ii  " : "iU  ");
            TERM_OUT(name);
            TERM_OUT("  ");
            TERM_OUT(ver);
            TERM_OUT("\n");
            n++;
        }
        p = status_paragraph(&db, i + 1);
    }
    status_free(&db);
    if (n == 0) TERM_OUT("dpkg: no packages installed\n");
}

int dpkg_show_files(const char* package) {
    char* list;
    size_t llen;
    if (info_read_list(package, &list, &llen) != 0 || list == NULL) {
        TERM_OUT("dpkg: package '");
        TERM_OUT(package);
        TERM_OUT("' not found\n");
        return -1;
    }
    for (char* p = list; p && *p; ) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        TERM_OUT(p);
        TERM_OUT("\n");
        p = nl ? nl + 1 : NULL;
    }
    kfree(list);
    return 0;
}
