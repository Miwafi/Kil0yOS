/* Phase 0 acceptance test: musl static hello built with
 *   musl-gcc -static -no-pie -O2 -Wl,-Ttext-segment=0x10000000
 * Runs as /bin/hello-lnx on Kil0yOS via the Linux-ABI syscall path. */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    printf("hello from musl static ELF on Kil0yOS!\n");
    printf("argc=%d argv0=%s\n", argc, argc > 0 ? argv[0] : "?");
    if (write(1, "write(2) works\n", 15) != 15) {
        return 1;
    }
    return 42;
}
