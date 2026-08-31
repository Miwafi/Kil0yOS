/* Phase 4.1: ustar extractor over the kernel fs overlay. */

#include "pkg/tar.h"
#include "lib/string.h"
#include "fs/fs.h"
#include "drivers/vga.h"
#include "mm/memory.h"

#define TAR_BLOCK 512

/* mkdir -p lives in the fs layer (fs_mkdir_p); tar paths are absolute. */
#define tar_mkdir_p(path) fs_mkdir_p(path)

static long tar_octal(const char* p, int n) {
    long v = 0;
    int seen = 0;
    for (int i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\0' || c == ' ') {
            if (seen) break;
            continue;
        }
        if (c < '0' || c > '7') break;
        v = v * 8 + (c - '0');
        seen = 1;
    }
    return v;
}

/* Strip "./" prefixes and trailing '/' from an archive member name.
 * Returns 0 if the name is unusable (empty / absolute-out-of-scope). */
static int tar_normalize_name(const char* in, char* out, size_t outsz) {
    const char* p = in;
    while (p[0] == '.' && p[1] == '/') p += 2;
    while (*p == '/') p++;
    if (*p == '\0') return 0;
    size_t n = strlen(p);
    while (n > 0 && p[n - 1] == '/') n--;
    if (n == 0 || n >= outsz) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* Shared scanner callback: return 1 to stop early (found), 0 continue. */
typedef int (*tar_iter_cb)(const char* clean_name, char typeflag,
                           const uint8_t* body, size_t size, void* ctx);
static int tar_iterate(const uint8_t* data, size_t len, tar_iter_cb cb, void* ctx);

typedef struct { tar_file_cb cb; void* ctx; int files; int err; } extract_ctx_t;

static int extract_cb(const char* clean, char typeflag,
                      const uint8_t* body, size_t size, void* ctxp) {
    extract_ctx_t* ec = (extract_ctx_t*)ctxp;
    char fp[260];

    if (typeflag == '5' ||
        ((typeflag == 0 || typeflag == '0') && clean[strlen(clean) - 1] == '/')) {
        /* directory entry (or '\0'-typed name ending in '/') */
        char dirp[260];
        strcpy(dirp, "/");
        strcat(dirp, clean);
        /* drop trailing slash for fs_create_dir */
        size_t dl = strlen(dirp);
        if (dl > 1 && dirp[dl - 1] == '/') dirp[dl - 1] = '\0';
        if (tar_mkdir_p(dirp) != 0) {
            klog("[tar] mkdir failed: ");
            klog(dirp);
            klog("\n");
            ec->err = 1;
            return 1;
        }
        return 0;
    }

    if (typeflag == '0' || typeflag == 0) {
        strcpy(fp, "/");
        strcat(fp, clean);
        /* mkdir the parent chain only - the final component is the file
         * itself; fs_mkdir_p would create it as a DIRECTORY */
        char dirp[260];
        strcpy(dirp, fp);
        char* last = strrchr(dirp, '/');
        if (last && last != dirp) {
            *last = '\0';
            if (tar_mkdir_p(dirp) != 0) {
                klog("[tar] parent dirs failed: ");
                klog(dirp);
                klog("\n");
                ec->err = 1;
                return 1;
            }
        }
        fs_entry_t* f = fs_resolve_path(fp);
        if (f == NULL) {
            f = fs_create_file(fp);
        }
        if (f == NULL) {
            char eb[16];
            itoa(fs_get_last_error(), eb, 10, sizeof(eb));
            klog("[tar] create failed (fserr ");
            klog(eb);
            klog("): ");
            klog(fp);
            klog("\n");
            ec->err = 1;
            return 1;
        }
        if (fs_write_file(f, body, size) < 0) {
            char eb[16];
            itoa(fs_get_last_error(), eb, 10, sizeof(eb));
            klog("[tar] write failed (fserr ");
            klog(eb);
            klog(" backend=");
            klog(f->backend == FS_BACKEND_MEM ? "mem" :
                 f->backend == FS_BACKEND_EXT2 ? "ext2" : "fat");
            klog(" size=");
            itoa((int)size, eb, 10, sizeof(eb));
            klog(eb);
            klog("): ");
            klog(fp);
            klog("\n");
            ec->err = 1;
            return 1;
        }
        ec->files++;
        if (ec->cb) ec->cb(fp, (uint32_t)size, ec->ctx);
        return 0;
    }

    /* symlinks ('2'), hardlinks ('1'), char/block/fifo: skipped */
    char tbuf[2] = { typeflag ? typeflag : '?', 0 };
    klog("[tar] skipped entry type '");
    klog(tbuf);
    klog("': ");
    klog(clean);
    klog("\n");
    return 0;
}

int tar_extract(const uint8_t* data, size_t len, tar_file_cb cb, void* ctx) {
    extract_ctx_t ec = { cb, ctx, 0, 0 };
    if (tar_iterate(data, len, extract_cb, &ec) < 0) return -1;
    if (ec.err) return -1;
    return ec.files;
}

/* Iterate tar headers; for each entry invoke the callback. Shared scanner
 * for tar_extract and tar_find_file. cb returns 1 to stop early (found). */


static int tar_iterate(const uint8_t* data, size_t len, tar_iter_cb cb, void* ctx) {
    size_t pos = 0;
    char longname[256];
    int have_longname = 0;

    while (pos + TAR_BLOCK <= len) {
        const uint8_t* hdr = data + pos;
        if (hdr[0] == 0) {
            int all_zero = 1;
            for (int i = 0; i < TAR_BLOCK; i++)
                if (hdr[i]) { all_zero = 0; break; }
            if (all_zero) break;
            pos += TAR_BLOCK;
            continue;
        }

        const char* name = (const char*)hdr;
        long size = tar_octal((const char*)hdr + 124, 12);
        char typeflag = (char)hdr[156];
        const char* prefix = (const char*)hdr + 345;

        if (size < 0) return -1;
        size_t data_len = ((size_t)size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
        if (pos + TAR_BLOCK + data_len > len) return -1;
        const uint8_t* body = data + pos + TAR_BLOCK;
        pos += TAR_BLOCK + data_len;

        char full[256];
        if (typeflag == 'L') {
            size_t n = (size_t)size;
            if (n >= sizeof(longname)) n = sizeof(longname) - 1;
            memcpy(longname, body, n);
            longname[n] = '\0';
            have_longname = 1;
            continue;
        }

        if (have_longname) {
            strcpy(full, longname);
            have_longname = 0;
        } else if (prefix[0]) {
            strncpy(full, prefix, sizeof(full) - 1);
            full[sizeof(full) - 1] = '\0';
            size_t plen = strlen(full);
            if (plen < sizeof(full) - 1) {
                full[plen] = '/';
                strncpy(full + plen + 1, name, sizeof(full) - plen - 2);
                full[sizeof(full) - 1] = '\0';
            }
        } else {
            strncpy(full, name, sizeof(full) - 1);
            full[sizeof(full) - 1] = '\0';
        }

        char clean[256];
        if (!tar_normalize_name(full, clean, sizeof(clean))) continue;

        if (cb(clean, typeflag, body, (size_t)size, ctx) != 0) return 1;
    }
    return 0;
}

typedef struct { const char* want; const uint8_t* body; size_t size; int hit; } find_ctx_t;

static int find_file_cb(const char* clean, char typeflag,
                        const uint8_t* body, size_t size, void* ctxp) {
    find_ctx_t* fc = (find_ctx_t*)ctxp;
    if (typeflag == '0' || typeflag == 0) {
        if (strcmp(clean, fc->want) == 0) {
            fc->body = body;
            fc->size = size;
            fc->hit = 1;
            return 1;
        }
    }
    return 0;
}

int tar_find_file(const uint8_t* data, size_t len, const char* name,
                  const uint8_t** body, size_t* size) {
    const char* want = name;
    if (strncmp(want, "./", 2) == 0) want += 2;
    while (*want == '/') want++;

    find_ctx_t fc = { want, NULL, 0, 0 };
    if (tar_iterate(data, len, find_file_cb, &fc) < 0) return -1;
    if (!fc.hit) return -1;
    *body = fc.body;
    *size = fc.size;
    return 0;
}
