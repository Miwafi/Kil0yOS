/* Host-side test harness for the kernel zstd decoder.
 *
 * Compiles src/kernel/pkg/zstd.c against stub headers (fakeinc) that map
 * kmalloc/kfree to malloc/free and neutralize kernel-only headers, then
 * decompresses the file given as argv[1] into argv[2].
 *
 *   gcc -O2 -Wall -Ifakeinc -Iinclude tools/test_zstd_host.c \
 *       src/kernel/pkg/zstd.c -o zdec
 */
#include <stdio.h>
#include <stdlib.h>

int zstd_decompress_heap(const unsigned char* in, unsigned long long inLen,
                         unsigned char** out, unsigned long long* outLen);

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.zst out\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open input"); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc(sz ? (size_t)sz : 1);
    if (!buf || (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz)) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    fclose(f);

    unsigned char* out = NULL;
    unsigned long long outLen = 0;
    if (zstd_decompress_heap(buf, (unsigned long long)sz, &out, &outLen) != 0) {
        fprintf(stderr, "zstd_decompress_heap FAILED (in=%ld)\n", sz);
        return 1;
    }
    FILE* g = fopen(argv[2], "wb");
    if (!g) { perror("open output"); return 2; }
    if (outLen && fwrite(out, 1, (size_t)outLen, g) != outLen) {
        fprintf(stderr, "write failed\n"); return 2;
    }
    fclose(g);
    fprintf(stderr, "ok: %llu bytes\n", outLen);
    return 0;
}
