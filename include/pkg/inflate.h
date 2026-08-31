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

/* Streaming variant: the decompressed stream is delivered to `sink` in
 * chunks (order-preserving, each byte exactly once) instead of one big
 * caller buffer. Internal working set is a fixed 64 KB sliding window.
 * `sink` returns 0 to continue, non-zero to abort (propagated as -1).
 * CRC32 is verified per member as usual. */
typedef int (*gzip_sink_fn)(void* ctx, const uint8_t* data, size_t n);
int gzip_inflate_cb(const uint8_t* in, size_t in_len,
                    gzip_sink_fn sink, void* ctx, size_t* out_len);

/* Uncompressed size from the gzip ISIZE trailer (mod 2^32). The input
 * must end with a complete gzip member. Returns -1 on malformed tail. */
long long gzip_payload_size(const uint8_t* in, size_t in_len);

#endif
