#include "core/syscall_lnx.h"
#include "core/syscall.h"
#include "core/process.h"
#include "core/uvm.h"
#include "core/elf.h"
#include "mm/memory.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "drivers/io.h"

/* --- Linux errno values used here --- */
#define L_EPERM   1
#define L_ENOENT  2
#define L_EBADF   9
#define L_ENOMEM  12
#define L_EINVAL  22
#define L_ENOTTY  25
#define L_ENODEV  19
#define L_ENOSYS  38

/* Linux syscall numbers (subset, Phase 0) */
#define LNX_read             0
#define LNX_write            1
#define LNX_open             2
#define LNX_close            3
#define LNX_mmap             9
#define LNX_mprotect        10
#define LNX_munmap          11
#define LNX_brk             12
#define LNX_rt_sigaction    13
#define LNX_rt_sigprocmask  14
#define LNX_ioctl           16
#define LNX_writev          20
#define LNX_getpid          39
#define LNX_uname           63
#define LNX_getuid         102
#define LNX_getgid         104
#define LNX_geteuid        107
#define LNX_getegid        108
#define LNX_gettid         186
#define LNX_futex          202
#define LNX_set_tid_address 218
#define LNX_exit_group     231
#define LNX_clock_gettime  228
#define LNX_exit            60
#define LNX_arch_prctl     158
#define LNX_set_robust_list 273
#define LNX_getrandom      318

#define LNX_TABLE_SIZE 512

typedef uint64_t (*lnx_handler_t)(uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t);

static lnx_handler_t lnx_table[LNX_TABLE_SIZE];
static uint32_t lnx_warned[LNX_TABLE_SIZE / 32 + 1];

/* mmap flags (x86-64) */
#define LNX_MAP_FIXED     0x10
#define LNX_MAP_ANONYMOUS 0x20

/* ioctl cmds */
#define LNX_TCGETS    0x5401
#define LNX_TIOCGWINSZ 0x5413

/* arch_prctl codes */
#define LNX_ARCH_SET_FS 0x1002
#define LNX_ARCH_GET_FS 0x1003

static inline void lnx_wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t lnx_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void syscall_lnx_write_fs_base(uint64_t base) {
    lnx_wrmsr(MSR_FS_BASE, base);
}

/* Console output shared by write/writev. User stdout goes to the VGA
 * console AND COM1 so headless QEMU runs (serial log) see program output. */
static void lnx_serial_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
}

static uint64_t lnx_emit(const char* buf, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        vga_putchar(buf[i]);
        if (buf[i] == '\n') lnx_serial_putc('\r');
        lnx_serial_putc(buf[i]);
    }
    return count;
}

static uint64_t sc_read(uint64_t fd, uint64_t buf, uint64_t count,
                        uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (fd != 0) return -L_EBADF;
    if (count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) return -L_EINVAL;
    return sys_read(0, buf, count, 0, 0, 0);
}

static uint64_t sc_write(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (fd != 1 && fd != 2) return -L_EBADF;
    if (count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) return -L_EINVAL;
    return lnx_emit((const char*)buf, count);
}

struct lnx_iovec { uint64_t base; uint64_t len; };

static uint64_t sc_writev(uint64_t fd, uint64_t iov, uint64_t iovcnt,
                          uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (fd != 1 && fd != 2) return -L_EBADF;
    if (iovcnt == 0) return 0;
    if (iovcnt > 1024) return -L_EINVAL;
    if (!process_check_user_range(iov, (size_t)(iovcnt * sizeof(struct lnx_iovec)))) {
        return -L_EINVAL;
    }
    uint64_t total = 0;
    const struct lnx_iovec* v = (const struct lnx_iovec*)iov;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (v[i].len == 0) continue;
        if (!process_check_user_range(v[i].base, (size_t)v[i].len)) {
            return -L_EINVAL;
        }
        total += lnx_emit((const char*)v[i].base, v[i].len);
    }
    return total;
}

static uint64_t sc_open(uint64_t u1, uint64_t u2, uint64_t u3,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    return -L_ENOENT;   /* no per-process fd table until a later phase */
}

static uint64_t sc_close(uint64_t u1, uint64_t u2, uint64_t u3,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    return -L_EBADF;
}

/* Phase 0.2: anonymous mmap. File-backed mappings are rejected. */
static uint64_t sc_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t off) {
    (void)fd; (void)off;
    process_t* proc = process_get_current();
    if (proc == NULL) return -L_ENOMEM;
    if (len == 0) return -L_EINVAL;
    if (len > UVM_MMAP_LIMIT - UVM_MMAP_BASE) return -L_ENOMEM;
    if (prot & ~7ULL) return -L_EINVAL;
    if (!(flags & LNX_MAP_ANONYMOUS)) return -L_ENODEV;

    uint32_t p = 0;
    if (prot & 1) p |= UVM_PROT_READ;
    if (prot & 2) p |= UVM_PROT_WRITE;
    if (prot & 4) p |= UVM_PROT_EXEC;

    uint64_t va = uvm_mmap_anon(proc, addr, (size_t)len, p,
                                (flags & LNX_MAP_FIXED) != 0);
    return va ? va : (uint64_t)-L_ENOMEM;
}

static uint64_t sc_munmap(uint64_t addr, uint64_t len,
                          uint64_t u3, uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    if (proc == NULL) return -L_EINVAL;
    if (len == 0) return -L_EINVAL;
    if (addr & 0xFFF) return -L_EINVAL;
    uvm_unmap_range(proc, addr, addr + len);
    return 0;
}

/* Phase 0.3: change page protections of a mapped range */
static uint64_t sc_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                            uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    if (proc == NULL) return -L_EINVAL;
    if (len == 0) return 0;
    if (addr & 0xFFF) return -L_EINVAL;
    if (prot & ~7ULL) return -L_EINVAL;

    uint32_t p = 0;
    if (prot & 1) p |= UVM_PROT_READ;
    if (prot & 2) p |= UVM_PROT_WRITE;
    if (prot & 4) p |= UVM_PROT_EXEC;

    if (uvm_change_prot(proc, addr, addr + len, p) != 0) return -L_ENOMEM;
    return 0;
}

/* Phase 0.4: brk. brk(0) and failed attempts return the current break,
 * matching what musl's malloc expects. */
static uint64_t sc_brk(uint64_t addr,
                       uint64_t u2, uint64_t u3, uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    if (proc == NULL) return -L_ENOMEM;
    if (proc->brk_start == 0) return -L_ENOMEM;   /* not an ELF process */

    if (addr == 0 || addr < proc->brk_start || addr > UVM_BRK_LIMIT) {
        return proc->brk_cur;
    }

    if (addr > proc->brk_cur) {
        if (uvm_map_range(proc, proc->brk_cur, addr, UVM_PROT_READ | UVM_PROT_WRITE) != 0) {
            return proc->brk_cur;
        }
    } else if (addr < proc->brk_cur) {
        uvm_unmap_range(proc, addr, proc->brk_cur);
    }
    proc->brk_cur = addr;
    return proc->brk_cur;
}

static uint64_t sc_getpid(uint64_t u1, uint64_t u2, uint64_t u3,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    return proc ? proc->pid : 0;
}

static uint64_t sc_gettid(uint64_t u1, uint64_t u2, uint64_t u3,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    return proc ? proc->pid : 0;
}

/* Single-threaded, no signals: succeed without doing anything */
static uint64_t sc_stub_ok(uint64_t u1, uint64_t u2, uint64_t u3,
                           uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    return 0;
}

static uint64_t sc_getuid_like(uint64_t u1, uint64_t u2, uint64_t u3,
                               uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    return 0;   /* root */
}

static uint64_t sc_futex(uint64_t u1, uint64_t u2, uint64_t u3,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    return -L_ENOSYS;   /* no threads yet (Phase 3.2) */
}

static uint64_t sc_set_tid_address(uint64_t u1, uint64_t u2, uint64_t u3,
                                   uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    return proc ? proc->pid : 0;
}

static uint64_t sc_exit(uint64_t code, uint64_t u2, uint64_t u3,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    process_exit((int)code);
    return 0;   /* never reached */
}

static uint64_t sc_clock_gettime(uint64_t clk, uint64_t ts,
                                 uint64_t u3, uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)clk; (void)u3; (void)u4; (void)u5; (void)u6;
    if (!process_check_user_range(ts, 16)) return -L_EINVAL;
    uint64_t us = pit_uptime_us();
    uint64_t sec = us / 1000000ULL;
    uint64_t nsec = (us % 1000000ULL) * 1000ULL;
    uint64_t* t = (uint64_t*)ts;
    t[0] = sec;
    t[1] = nsec;
    return 0;
}

static uint64_t sc_getrandom(uint64_t buf, uint64_t len, uint64_t flags,
                             uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)flags; (void)u4; (void)u5; (void)u6;
    if (len == 0) return 0;
    if (!process_check_user_range(buf, (size_t)len)) return -L_EINVAL;
    static uint64_t gr_seed = 0;
    if (gr_seed == 0) gr_seed = pit_uptime_us() | 1;
    uint8_t* dst = (uint8_t*)buf;
    for (uint64_t i = 0; i < len; i++) {
        if (i % 8 == 0) {
            gr_seed ^= gr_seed << 13;
            gr_seed ^= gr_seed >> 7;
            gr_seed ^= gr_seed << 17;
        }
        dst[i] = (uint8_t)(gr_seed >> ((i % 8) * 8));   /* soft random */
    }
    return len;
}

static uint64_t sc_arch_prctl(uint64_t code, uint64_t addr,
                              uint64_t u3, uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    if (proc == NULL) return -L_EINVAL;

    switch (code) {
    case LNX_ARCH_SET_FS:
        syscall_lnx_write_fs_base(addr);
        proc->fs_base = addr;
        return 0;
    case LNX_ARCH_GET_FS:
        if (!process_check_user_range(addr, 8)) return -L_EINVAL;
        *(uint64_t*)addr = proc->fs_base;
        return 0;
    default:
        return -L_EINVAL;
    }
}

static uint64_t sc_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    if (fd != 0 && fd != 1 && fd != 2) return -L_EBADF;

    if (cmd == LNX_TIOCGWINSZ) {
        if (!process_check_user_range(arg, 8)) return -L_EINVAL;
        uint16_t* ws = (uint16_t*)arg;
        ws[0] = 24; ws[1] = 80; ws[2] = 0; ws[3] = 0;   /* rows, cols */
        return 0;
    }
    if (cmd == LNX_TCGETS) {
        /* Zeroed termios => isatty() true => musl line-buffers stdout */
        if (!process_check_user_range(arg, 60)) return -L_EINVAL;
        memset((void*)arg, 0, 60);
        return 0;
    }
    return -L_ENOTTY;
}

struct lnx_utsname { char v[6][65]; };

static uint64_t sc_uname(uint64_t buf,
                         uint64_t u2, uint64_t u3, uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    if (!process_check_user_range(buf, sizeof(struct lnx_utsname))) return -L_EINVAL;
    struct lnx_utsname* u = (struct lnx_utsname*)buf;
    memset(u, 0, sizeof(*u));
    strcpy(u->v[0], "Kil0yOS");
    strcpy(u->v[1], "kil0yos");
    strcpy(u->v[2], "2.10.0");
    strcpy(u->v[3], "#1 SMP Kil0yOS Phase 0");
    strcpy(u->v[4], "x86_64");
    strcpy(u->v[5], "");
    return 0;
}

void syscall_lnx_init(void) {
    memset(lnx_table, 0, sizeof(lnx_table));
    memset(lnx_warned, 0, sizeof(lnx_warned));

    lnx_table[LNX_read] = sc_read;
    lnx_table[LNX_write] = sc_write;
    lnx_table[LNX_writev] = sc_writev;
    lnx_table[LNX_open] = sc_open;
    lnx_table[LNX_close] = sc_close;
    lnx_table[LNX_mmap] = sc_mmap;
    lnx_table[LNX_mprotect] = sc_mprotect;
    lnx_table[LNX_munmap] = sc_munmap;
    lnx_table[LNX_brk] = sc_brk;
    lnx_table[LNX_rt_sigaction] = sc_stub_ok;
    lnx_table[LNX_rt_sigprocmask] = sc_stub_ok;
    lnx_table[LNX_ioctl] = sc_ioctl;
    lnx_table[LNX_getpid] = sc_getpid;
    lnx_table[LNX_uname] = sc_uname;
    lnx_table[LNX_getuid] = sc_getuid_like;
    lnx_table[LNX_getgid] = sc_getuid_like;
    lnx_table[LNX_geteuid] = sc_getuid_like;
    lnx_table[LNX_getegid] = sc_getuid_like;
    lnx_table[LNX_gettid] = sc_gettid;
    lnx_table[LNX_futex] = sc_futex;
    lnx_table[LNX_set_tid_address] = sc_set_tid_address;
    lnx_table[LNX_exit] = sc_exit;
    lnx_table[LNX_exit_group] = sc_exit;
    lnx_table[LNX_clock_gettime] = sc_clock_gettime;
    lnx_table[LNX_arch_prctl] = sc_arch_prctl;
    lnx_table[LNX_getrandom] = sc_getrandom;
    lnx_table[LNX_set_robust_list] = sc_stub_ok;

    /* Enable the `syscall` instruction (EFER.SCE) */
    uint64_t efer = lnx_rdmsr(0xC0000080);
    lnx_wrmsr(0xC0000080, efer | 1);

    /* STAR: kernel CS 0x08 (SS loaded as 0x10 = CS+8) on entry,
     * user CS 0x18 (SS 0x20) for the sysret fields (we return via iretq). */
    lnx_wrmsr(0xC0000081, ((uint64_t)0x18 << 48) | ((uint64_t)0x08 << 32));

    /* LSTAR: entry point in isr.asm */
    lnx_wrmsr(0xC0000082, (uint64_t)syscall_lnx_entry);

    /* FMASK: clear IF/DF/NT (and TF) while in the kernel */
    lnx_wrmsr(0xC0000084, 0x4700);

    klog("[sc] Linux ABI syscall entry enabled\n");
}

uint64_t syscall_lnx_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
    /* TEMPORARY Phase 0 debug trace: every Linux syscall to serial */
    {
        char tb[12];
        klog("[sc? ");
        itoa(num, tb, 10, 10);
        klog(tb);
        klog("]\n");
    }
    if (num >= LNX_TABLE_SIZE || lnx_table[num] == NULL) {
        if (!(lnx_warned[num / 32] & (1u << (num % 32)))) {
            lnx_warned[num / 32] |= 1u << (num % 32);
            klog("[sc] ENOSYS: ");
            char b[12];
            itoa(num, b, 10, 10);
            klog(b);
            klog("\n");
        }
        return -L_ENOSYS;
    }
    return lnx_table[num](a0, a1, a2, a3, a4, a5);
}
