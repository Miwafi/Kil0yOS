#ifndef PKG_SHA256_H
#define PKG_SHA256_H

#include "lib/types.h"

/* Kernel-side SHA-256 (FIPS 180-4) for Packages-index checksum
 * verification (Phase 4.4). */

void sha256(const uint8_t* data, size_t len, uint8_t digest[32]);

/* hex encode (64 chars + NUL); returns buf */
char* sha256_hex(const uint8_t digest[32], char buf[65]);

#endif
