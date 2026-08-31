#ifndef PKG_DEB_H
#define PKG_DEB_H

#include "lib/types.h"

/* .deb unpack pipeline (Phase 4.1): ar archive -> control.tar/data.tar
 * members -> gzip inflate -> tar. xz/zst members are rejected with a
 * clear error; the roadmap mitigates this by re-packing the mirror to
 * gzip (tools/build_repo.sh). */

#define DEB_MAX_FILES 256

typedef struct {
    char package[64];
    char version[64];
    char architecture[32];
    char depends[256];     /* raw Depends line, "" if none */
} deb_control_t;

typedef struct {
    deb_control_t control;
    char installed_files[DEB_MAX_FILES][128]; /* absolute paths, "/" prefixed */
    int  nfiles;
} deb_result_t;

/* Unpack a .deb image: extract data.tar.* into the fs and parse the
 * control metadata (not written to the fs). Returns 0 on success and
 * fills *out; negative on error (klog has details). */
int deb_unpack(const uint8_t* deb, size_t len, deb_result_t* out);

#endif
