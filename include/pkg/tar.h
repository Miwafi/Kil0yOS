#ifndef PKG_TAR_H
#define PKG_TAR_H

#include "lib/types.h"

/* ustar (POSIX tar) extractor into the kernel fs (Phase 4.1).
 * Regular files are written through fs_write_file (MEM/overlay backend);
 * directories are created (mkdir -p semantics); symlinks/hardlinks and
 * special files are skipped - the kernel fs has no link/special support.
 * GNU long names (typeflag 'L') and ustar prefix fields are handled. */

typedef void (*tar_file_cb)(const char* path, uint32_t size, void* ctx);

/* Extract a tar stream under "/" (paths inside the archive are used as-is,
 * with "./" stripped). Returns the number of regular files extracted, or
 * -1 on error (malformed header / fs failure). cb (optional) is invoked
 * per extracted regular file. */
int tar_extract(const uint8_t* data, size_t len, tar_file_cb cb, void* ctx);

/* Locate a regular file member by path (e.g. "control", "./control").
 * Returns 0 and sets *body and *size on hit; -1 if not found or malformed. */
int tar_find_file(const uint8_t* data, size_t len, const char* name,
                  const uint8_t** body, size_t* size);

#endif
