/* Phase 0 acceptance probe: brk / mmap / munmap / mprotect.
 * Freestanding static ELF (like mini.c) - direct Linux syscalls only.
 * Built with: gcc -nostdlib -static -no-pie -Wl,-Ttext-segment=0x10000000
 *
 * Checks the ROADMAP Phase 0 acceptance criteria:
 *   0.4 brk      - grow by 1 MiB, roundtrip data at both ends, shrink back
 *   0.2 mmap     - 64 KiB anonymous RW mapping, roundtrip data, munmap
 *   0.3 mprotect - page to PROT_READ and back without error
 */
#include <stdint.h>

#define SYS_write       1
#define SYS_mmap        9
#define SYS_mprotect   10
#define SYS_munmap     11
#define SYS_brk        12
#define SYS_exit_group 231

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static uint64_t sys3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

static uint64_t sys6(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = d;
    register uint64_t r8  __asm__("r8")  = e;
    register uint64_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/* Linux syscall error convention: -errno in [-4095, -1] */
static int failed(uint64_t r) {
    return (int64_t)r <= (int64_t)-1 && (int64_t)r >= (int64_t)-4095;
}

static void put(const char* s) {
    uint64_t n = 0;
    while (s[n]) n++;
    sys3(SYS_write, 1, (uint64_t)s, n);
}

static void die(const char* s) {
    put(s);
    sys3(SYS_exit_group, 1, 0, 0);
    for (;;) ;
}

void _start(void) {
    /* --- 0.4 brk: grow 1 MiB, touch first and last byte, shrink back --- */
    uint64_t cur  = sys3(SYS_brk, 0, 0, 0);
    uint64_t want = cur + 0x100000;
    uint64_t got  = sys3(SYS_brk, want, 0, 0);
    if (got != want) die("MM FAIL brk grow\n");

    volatile uint8_t* hp = (volatile uint8_t*)cur;
    hp[0]       = 0x5A;
    hp[0xFFFFF] = 0xA5;
    if (hp[0] != 0x5A || hp[0xFFFFF] != 0xA5) die("MM FAIL brk roundtrip\n");
    if (sys3(SYS_brk, cur, 0, 0) != cur) die("MM FAIL brk shrink\n");

    /* --- 0.2 mmap: 64 KiB anonymous RW + munmap --- */
    uint64_t p = sys6(SYS_mmap, 0, 0x10000, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
    if (failed(p)) die("MM FAIL mmap\n");

    volatile uint8_t* mp = (volatile uint8_t*)p;
    mp[0]      = 1;
    mp[0x8000] = 2;
    mp[0xFFFF] = 3;
    if (mp[0] != 1 || mp[0x8000] != 2 || mp[0xFFFF] != 3) {
        die("MM FAIL mmap roundtrip\n");
    }

    /* --- 0.3 mprotect: page to PROT_READ and back to RW --- */
    if (sys3(SYS_mprotect, p, 0x1000, PROT_READ) != 0) {
        die("MM FAIL mprotect RO\n");
    }
    if (sys3(SYS_mprotect, p, 0x1000, PROT_READ | PROT_WRITE) != 0) {
        die("MM FAIL mprotect RW\n");
    }
    mp[0] = 9;
    if (mp[0] != 9) die("MM FAIL mprotect roundtrip\n");

    if (sys3(SYS_munmap, p, 0x10000, 0) != 0) die("MM FAIL munmap\n");

    put("MM OK\n");
    sys3(SYS_exit_group, 0, 0, 0);
    for (;;) ;
}
