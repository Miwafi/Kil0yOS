/* Phase 0 plumbing test: freestanding static ELF that only uses the two
 * most basic Linux syscalls (write, exit_group) via inline asm.
 * Built with: gcc -nostdlib -static -no-pie -Wl,-Ttext-segment=0x10000000 */
#include <stdint.h>

static uint64_t sys1(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

static const char msg[] = "MINI OK\n";

void _start(void) {
    sys1(1, 1, (uint64_t)msg, sizeof(msg) - 1);   /* write(1, msg, n) */
    sys1(231, 7, 0, 0);                            /* exit_group(7) */
    for (;;) ;
}
