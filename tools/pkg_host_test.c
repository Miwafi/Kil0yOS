/* Host-side unit test for the kernel gzip/sha256 modules (Phase 4.1/4.4).
 * Run in WSL: gcc -I include tools/pkg_host_test.c src/kernel/pkg/inflate.c \
 *   src/kernel/pkg/sha256.c src/kernel/lib/string.c src/kernel/lib/stdlib.c \
 *   -o /tmp/pkgtest && /tmp/pkgtest <file.gz>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "pkg/inflate.h"
#include "pkg/sha256.h"

int main(int argc, char** argv) {
    /* SHA-256 FIPS vectors */
    {
        uint8_t d[32];
        char hex[65];
        sha256((const uint8_t*)"abc", 3, d);
        sha256_hex(d, hex);
        assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223"
                            "b00361a396177a9cb410ff61f20015ad") == 0);
        sha256((const uint8_t*)"", 0, d);
        sha256_hex(d, hex);
        assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb924"
                            "27ae41e4649b934ca495991b7852b855") == 0);
        /* 56-byte message: classic padding boundary case */
        sha256((const uint8_t*)"abcdbcdecdefdefgefghfghighijhi"
                               "jkijkljklmklmnlmnomnopnopq", 56, d);
        sha256_hex(d, hex);
        assert(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039"
                            "a33ce45964ff2167f6ecedd419db06c1") == 0);
        printf("sha256 vectors OK\n");
    }

    /* inflate: one multi-member gzip file, original kept alongside */
    if (argc >= 3) {
        FILE* fgz = fopen(argv[1], "rb");
        FILE* forg = fopen(argv[2], "rb");
        if (!fgz || !forg) { perror("open"); return 1; }
        fseek(fgz, 0, SEEK_END); long gzlen = ftell(fgz); fseek(fgz, 0, SEEK_SET);
        uint8_t* gz = malloc(gzlen);
        fread(gz, 1, gzlen, fgz);
        fclose(fgz);

        fseek(forg, 0, SEEK_END); long orglen = ftell(forg); fseek(forg, 0, SEEK_SET);
        uint8_t* org = malloc(orglen);
        fread(org, 1, orglen, forg);
        fclose(forg);

        long long isz = gzip_payload_size(gz, gzlen);
        printf("payload ISIZE=%lld (orig=%ld)\n", isz, orglen);

        uint8_t* out = malloc(orglen + 65536);
        size_t outlen = 0;
        if (gzip_inflate(gz, gzlen, out, orglen + 65536, &outlen) != 0) {
            fprintf(stderr, "gzip_inflate FAILED\n");
            return 1;
        }
        printf("inflated %zu bytes\n", outlen);
        if ((long)outlen != orglen || memcmp(out, org, orglen) != 0) {
            fprintf(stderr, "MISMATCH\n");
            return 1;
        }
        printf("inflate round-trip OK\n");
        /* ISIZE sanity (mod 2^32) - only for single-member files; for
         * concatenated members gzip_payload_size reports the LAST
         * member's ISIZE, which is less than the total by design */
        if (argc < 4 || strcmp(argv[3], "multi") != 0)
            assert(isz >= 0 && (unsigned long long)isz % (1ULL<<32) ==
                   (unsigned long long)orglen % (1ULL<<32));
    }
    printf("ALL PKG HOST TESTS PASSED\n");
    return 0;
}
