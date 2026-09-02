#ifndef PKG_ZSTD_H
#define PKG_ZSTD_H

#include "lib/types.h"

/* Minimal self-contained zstd decoder (RFC 8878 subset) for .deb
 * data.tar.zst members. Supports regular + skippable frames, Raw/RLE/
 * Compressed blocks, Raw/RLE/Huffman(1 or 4 streams)/Treeless literals,
 * Predefined/RLE/FSE/Repeat sequence tables, repeat offsets and the
 * optional XXH64 content checksum. No dictionaries, no legacy frames.
 *
 * Decompresses the whole input into a single kmalloc'd buffer (grown
 * geometrically). Returns 0 on success and hands ownership of *out to
 * the caller (kfree it); negative on error. */
int zstd_decompress_heap(const uint8_t* in, size_t inLen,
                         uint8_t** out, size_t* outLen);

#endif
