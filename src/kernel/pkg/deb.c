/* Phase 4.1: .deb (ar archive) unpack pipeline. */

#include "pkg/deb.h"
#include "pkg/inflate.h"
#include "pkg/tar.h"
#include "pkg/zstd.h"
#include "lib/string.h"
#include "mm/memory.h"
#include "drivers/vga.h"

typedef struct {
    char name[20];       /* trimmed member name ('/' suffix stripped) */
    size_t size;
    const uint8_t* data;
} ar_member_t;

/* Parse the next ar member header at *pos. Returns 0 on success. */
static int ar_next_member(const uint8_t* buf, size_t len, size_t* pos,
                          ar_member_t* m) {
    static const char magic[8] = "!<arch>\n";

    if (*pos == 0) {
        if (len < 8 || memcmp(buf, magic, 8) != 0) return -1;
        *pos = 8;
    }
    if (*pos + 60 > len) return -1;

    const uint8_t* h = buf + *pos;
    /* ar header: name[16] date[12] uid[6] gid[6] mode[8] size[10] fmag[2];
     * fmag ("`\n") lives at offsets 58-59 */
    if (h[58] != '`' || h[59] != '\n') return -1; /* fmag */

    size_t n = 0;
    while (n < 16 && h[n] != '/' && h[n] != ' ' && h[n] != '\0') n++;
    if (n >= sizeof(m->name)) n = sizeof(m->name) - 1;
    memcpy(m->name, h, n);
    m->name[n] = '\0';

    long sz = 0;
    for (int i = 48; i < 58; i++) {
        if (h[i] < '0' || h[i] > '9') break;
        sz = sz * 10 + (h[i] - '0');
    }
    if (sz < 0) return -1;
    m->size = (size_t)sz;
    m->data = h + 60;
    if (*pos + 60 + m->size > len) return -1;

    size_t advance = 60 + m->size;
    if (advance & 1) advance++;      /* members are 2-byte aligned */
    *pos += advance;
    return 0;
}

/* Inflate a gzip member into an exact-size heap buffer (ISIZE based). */
static int gunzip_to_heap(const uint8_t* gz, size_t gz_len,
                          uint8_t** out, size_t* out_len) {
    long long isz = gzip_payload_size(gz, gz_len);
    if (isz < 0) return -1;
    /* ISIZE is mod 2^32; for sanity cap at 64MB and allow some slack for
     * concatenated members */
    if (isz > 64 * 1024 * 1024) return -1;
    uint8_t* buf = (uint8_t*)kmalloc((size_t)isz + 4096);
    if (buf == NULL) return -1;
    size_t got = 0;
    if (gzip_inflate(gz, gz_len, buf, (size_t)isz + 4096, &got) != 0) {
        kfree(buf);
        return -1;
    }
    if ((long long)got < isz / 2) { /* implausibly short */
        kfree(buf);
        return -1;
    }
    *out = buf;
    *out_len = got;
    return 0;
}

/* Decompress a zstd member into a heap buffer (whole-stream, may
 * reallocate internally; caller kfrees *out). */
static int zstd_to_heap(const uint8_t* z, size_t z_len,
                        uint8_t** out, size_t* out_len) {
    if (zstd_decompress_heap(z, z_len, out, out_len) != 0) return -1;
    return 0;
}

typedef struct { char (*files)[128]; int nfiles; } filelist_ctx_t;

static void filelist_cb(const char* path, uint32_t size, void* ctxp) {
    (void)size;
    filelist_ctx_t* fc = (filelist_ctx_t*)ctxp;
    if (fc->nfiles < DEB_MAX_FILES &&
        strlen(path) < 128) {
        strcpy(fc->files[fc->nfiles], path);
        fc->nfiles++;
    }
}

/* Parse the RFC822-ish control file for the fields dpkg needs. */
static void deb_parse_control(const char* text, size_t len, deb_control_t* c) {
    memset(c, 0, sizeof(*c));

    char* copy = (char*)kmalloc(len + 1);
    if (copy == NULL) return;
    memcpy(copy, text, len);
    copy[len] = '\0';

    for (char* line = strtok(copy, "\n"); line;
         line = strtok(NULL, "\n")) {
        char* colon = strchr(line, ':');
        if (colon == NULL) continue;
        *colon = '\0';
        char* val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (strcmp(line, "Package") == 0) {
            strncpy(c->package, val, sizeof(c->package) - 1);
        } else if (strcmp(line, "Version") == 0) {
            strncpy(c->version, val, sizeof(c->version) - 1);
        } else if (strcmp(line, "Architecture") == 0) {
            strncpy(c->architecture, val, sizeof(c->architecture) - 1);
        } else if (strcmp(line, "Depends") == 0) {
            strncpy(c->depends, val, sizeof(c->depends) - 1);
        }
    }
    kfree(copy);
}

int deb_unpack(const uint8_t* deb, size_t len, deb_result_t* out) {
    memset(out, 0, sizeof(*out));

    size_t pos = 0;
    ar_member_t m;
    int saw_debian_binary = 0;
    int have_control = 0;
    int have_data = 0;

    while (ar_next_member(deb, len, &pos, &m) == 0) {
        if (strcmp(m.name, "debian-binary") == 0) {
            saw_debian_binary = 1;
            continue;
        }
        if (strncmp(m.name, "control.tar", 11) == 0) {
            const char* ext = m.name + 11;
            if (strcmp(ext, ".gz") == 0) {
                uint8_t* tar = NULL;
                size_t tar_len = 0;
                if (gunzip_to_heap(m.data, m.size, &tar, &tar_len) != 0) {
                    klog("[deb] control.tar.gz inflate failed\n");
                    return -1;
                }
                const uint8_t* body;
                size_t blen;
                if (tar_find_file(tar, tar_len, "control", &body, &blen) == 0) {
                    deb_parse_control((const char*)body, blen, &out->control);
                    have_control = 1;
                } else {
                    klog("[deb] control file missing\n");
                }
                kfree(tar);
            } else if (strcmp(ext, ".zst") == 0) {
                uint8_t* tar = NULL;
                size_t tar_len = 0;
                if (zstd_to_heap(m.data, m.size, &tar, &tar_len) != 0) {
                    klog("[deb] control.tar.zst decompress failed\n");
                    return -1;
                }
                const uint8_t* body;
                size_t blen;
                if (tar_find_file(tar, tar_len, "control", &body, &blen) == 0) {
                    deb_parse_control((const char*)body, blen, &out->control);
                    have_control = 1;
                } else {
                    klog("[deb] control file missing\n");
                }
                kfree(tar);
            } else {
                klog("[deb] unsupported control.tar compression: ");
                klog(m.name);
                klog(" (re-pack repo to gzip)\n");
                return -1;
            }
            continue;
        }
        if (strncmp(m.name, "data.tar", 8) == 0) {
            const char* ext = m.name + 8;
            if (strcmp(ext, ".gz") == 0) {
                uint8_t* tar = NULL;
                size_t tar_len = 0;
                if (gunzip_to_heap(m.data, m.size, &tar, &tar_len) != 0) {
                    klog("[deb] data.tar.gz inflate failed\n");
                    return -1;
                }
                filelist_ctx_t fc = { out->installed_files, 0 };
                int n = tar_extract(tar, tar_len, filelist_cb, &fc);
                kfree(tar);
                if (n < 0) {
                    klog("[deb] data.tar extract failed\n");
                    return -1;
                }
                out->nfiles = fc.nfiles;
                have_data = 1;
            } else if (strcmp(ext, ".zst") == 0) {
                uint8_t* tar = NULL;
                size_t tar_len = 0;
                if (zstd_to_heap(m.data, m.size, &tar, &tar_len) != 0) {
                    klog("[deb] data.tar.zst decompress failed\n");
                    return -1;
                }
                filelist_ctx_t fc = { out->installed_files, 0 };
                int n = tar_extract(tar, tar_len, filelist_cb, &fc);
                kfree(tar);
                if (n < 0) {
                    klog("[deb] data.tar extract failed\n");
                    return -1;
                }
                out->nfiles = fc.nfiles;
                have_data = 1;
            } else {
                klog("[deb] unsupported data.tar compression: ");
                klog(m.name);
                klog(" (re-pack repo to gzip)\n");
                return -1;
            }
            continue;
        }
        /* unknown members (e.g. GNU symbol tables) are skipped */
    }

    if (!saw_debian_binary || !have_data) {
        klog("[deb] not a valid .deb archive\n");
        return -1;
    }
    if (!have_control) {
        klog("[deb] control metadata missing\n");
        return -1;
    }
    if (out->control.package[0] == '\0') {
        klog("[deb] control has no Package field\n");
        return -1;
    }
    return 0;
}
