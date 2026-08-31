#ifndef PKG_INFLATE_H
#define PKG_INFLATE_H

#include "lib/types.h"

/* Kernel-side gzip (RFC 1952) / DEFLATE (RFC 1951) decoder (Phase 4.1).
 * Used to unpack data.tar.gz members of .deb packages. The output buffer
 * is caller-provided; the exact uncompressed size can be read from the
 * gzip ISIZE trailer with gzip_payload_size() before inflating. */

/* Inflate a gzip stream (single or concatenated members).
 * Returns 0 on success and sets *out_len; -1 on any error. */
int gzip_inflate(const uint8_t* in, size_t in_len,
                 uint8_t* out, size_t out_cap, size_t* out_len);

/* Uncompressed size from the gzip ISIZE trailer (mod 2^32). The input
 * must end with a complete gzip member. Returns -1 on malformed tail. */
long long gzip_payload_size(const uint8_t* in, size_t in_len);

#endif
