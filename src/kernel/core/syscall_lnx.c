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
#include "core/tty.h"
#include "core/lnxvfs.h"
#include "fs/fs.h"

/* --- Linux errno values used here --- */
#define L_EPERM   1
#define L_ENOENT  2
#define L_EBADF   9
#define L_ENOMEM  12
#define L_EINVAL  22
#define L_ENOTTY  25
#define L_ENODEV  19
#define L_ENOSYS  38
#define L_ECHILD  10

/* Linux syscall numbers (subset, Phase 0) */
#define LNX_read             0
#define LNX_write            1
#define LNX_open             2
#define LNX_close            3
#define LNX_stat             4
#define LNX_fstat            5
#define LNX_lstat            6
#define LNX_lseek            8
#define LNX_mmap             9
#define LNX_mprotect        10
#define LNX_munmap          11
#define LNX_brk             12
#define LNX_rt_sigaction    13
#define LNX_rt_sigprocmask  14
#define LNX_ioctl           16
#define LNX_writev          20
#define LNX_dup             32
#define LNX_dup2            33
#define LNX_getpid          39
#define LNX_uname           63
#define LNX_getcwd          79
#define LNX_mkdir           83
#define LNX_rmdir           84
#define LNX_unlink          87
#define LNX_readlink        89
#define LNX_getuid         102
#define LNX_getgid         104
#define LNX_geteuid        107
#define LNX_getegid        108
#define LNX_gettid         186
#define LNX_futex          202
#define LNX_set_tid_address 218
#define LNX_getdents64     217
#define LNX_exit_group     231
#define LNX_clock_gettime  228
#define LNX_exit            60
#define LNX_arch_prctl     158
#define LNX_set_robust_list 273
#define LNX_openat         257
#define LNX_newfstatat     262
#define LNX_getrandom      318
#define LNX_statx          332

/* Phase 1.5 process syscalls */
#define LNX_clone          56
#define LNX_fork           57
#define LNX_vfork          58
#define LNX_execve         59
#define LNX_wait4          61
#define LNX_nanosleep      35
#define LNX_access         21
#define LNX_fcntl          72
#define LNX_chdir          81
#define LNX_getppid        110

/* Entry-frame pointer saved by syscall_lnx_entry (isr.asm): points at
 * the first saved register (r15) of the interrupted user context. fork/
 * wait4/execve clone or rewrite this frame. */
extern uint64_t lnx_frame_rsp;

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

/* Console output shared by write/writev. Goes through the TTY line
 * discipline (\n -> \r\n) to the VGA console AND COM1 so headless QEMU
 * runs (serial log) see program output. */
/* fd routing: 0/1/2 are the TTY console unless dup2 redirected them to a
 * file; fds >= 3 always go through the lnxvfs fd layer. */
static int fd_is_file(uint64_t fd) {
    process_t* proc = process_get_current();
    if (proc == NULL || fd >= LNX_MAX_FDS) return 0;
    return proc->fds[fd] != NULL;
}

static uint64_t sc_read(uint64_t fd, uint64_t buf, uint64_t count,
                        uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (fd >= LNX_MAX_FDS) return -L_EBADF;
    if (count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) return -L_EINVAL;
    if (fd_is_file(fd)) return (uint64_t)lnxvfs_read((int)fd, (char*)buf, (size_t)count);
    if (fd != 0) return -L_EBADF;
    return (uint64_t)tty_read((char*)buf, (size_t)count);
}

static uint64_t sc_write(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (fd >= LNX_MAX_FDS) return -L_EBADF;
    if (count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) return -L_EINVAL;
    if (fd_is_file(fd)) return (uint64_t)lnxvfs_write((int)fd, (const char*)buf, (size_t)count);
    if (fd != 1 && fd != 2) return -L_EBADF;
    return (uint64_t)tty_write((const char*)buf, (size_t)count);
}

struct lnx_iovec { uint64_t base; uint64_t len; };

static uint64_t sc_writev(uint64_t fd, uint64_t iov, uint64_t iovcnt,
                          uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (fd >= LNX_MAX_FDS) return -L_EBADF;
    if (iovcnt == 0) return 0;
    if (iovcnt > 1024) return -L_EINVAL;
    if (!process_check_user_range(iov, (size_t)(iovcnt * sizeof(struct lnx_iovec)))) {
        return -L_EINVAL;
    }
    /* Single redirected file fd: delegate (must not split the vector) */
    if (fd_is_file(fd)) {
        const struct lnx_iovec* v0 = (const struct lnx_iovec*)iov;
        if (iovcnt == 1) {
            if (v0[0].len == 0) return 0;
            if (!process_check_user_range(v0[0].base, (size_t)v0[0].len)) return -L_EINVAL;
            return (uint64_t)lnxvfs_write((int)fd, (const char*)v0[0].base, (size_t)v0[0].len);
        }
        return -L_EINVAL;
    }
    if (fd != 1 && fd != 2) return -L_EBADF;
    uint64_t total = 0;
    const struct lnx_iovec* v = (const struct lnx_iovec*)iov;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (v[i].len == 0) continue;
        if (!process_check_user_range(v[i].base, (size_t)v[i].len)) {
            return -L_EINVAL;
        }
        total += (uint64_t)tty_write((const char*)v[i].base, (size_t)v[i].len);
    }
    return total;
}

/* User path strings must live in mapped user memory and be NUL-terminated
 * within it; copies into a bounded kernel buffer. Returns length or -1. */
static int fetch_user_path(uint64_t upath, char* kbuf, size_t kbufsz) {
    if (!process_check_user_range(upath, 1)) return -1;
    size_t n = 0;
    while (n < kbufsz - 1) {
        if (!process_check_user_range(upath + n, 1)) return -1;
        char c = *(const char*)(upath + n);
        kbuf[n] = c;
        if (c == 0) return (int)n;
        n++;
    }
    return -1;   /* unterminated */
}

static uint64_t sc_openat(uint64_t dirfd, uint64_t upath, uint64_t flags,
                          uint64_t mode, uint64_t u5, uint64_t u6) {
    (void)u5; (void)u6;
    (void)dirfd;   /* AT_FDCWD semantics only */
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return (uint64_t)lnxvfs_open(path, (int)flags, (int)mode);
}

static uint64_t sc_open(uint64_t upath, uint64_t flags, uint64_t mode,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return (uint64_t)lnxvfs_open(path, (int)flags, (int)mode);
}

static uint64_t sc_close(uint64_t fd, uint64_t u2, uint64_t u3,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    if (fd >= LNX_MAX_FDS) return -L_EBADF;
    if (fd_is_file(fd)) return (uint64_t)lnxvfs_close((int)fd);
    if (fd <= 2) return 0;   /* console: always "open" */
    return -L_EBADF;
}

static uint64_t sc_lseek(uint64_t fd, uint64_t off, uint64_t whence,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    return (uint64_t)lnxvfs_lseek((int)fd, (int)whence, (long long)off);
}

static uint64_t sc_getdents64(uint64_t fd, uint64_t ubuf, uint64_t count,
                              uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    if (count == 0) return 0;
    if (!process_check_user_range(ubuf, (size_t)count)) return -L_EINVAL;
    return (uint64_t)lnxvfs_getdents64((int)fd, (void*)ubuf, (size_t)count);
}

static uint64_t sc_fstat(uint64_t fd, uint64_t ubuf, uint64_t u3,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u3; (void)u4; (void)u5; (void)u6;
    return (uint64_t)lnxvfs_fstat((int)fd, (void*)ubuf);
}

/* Legacy stat/lstat (4/6): same struct-stat layout as newfstatat's buffer.
 * No symlinks exist yet, so lstat == stat. */
static uint64_t sc_stat(uint64_t upath, uint64_t ubuf, uint64_t u3,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u3; (void)u4; (void)u5; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return (uint64_t)lnxvfs_stat_path(path, (void*)ubuf);
}

#define L_AT_FDCWD            ((uint64_t)-100)
#define L_AT_SYMLINK_NOFOLLOW 0x100
#define L_AT_EMPTY_PATH       0x1000

static uint64_t sc_newfstatat(uint64_t dirfd, uint64_t upath, uint64_t ubuf,
                              uint64_t flags, uint64_t u5, uint64_t u6) {
    (void)dirfd; (void)u5; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (upath == 0) {
        /* statx-style empty path + AT_EMPTY_PATH: not supported here */
        return -L_ENOENT;
    }
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    if (path[0] == 0 && (flags & L_AT_EMPTY_PATH)) return -L_ENOSYS;
    (void)L_AT_SYMLINK_NOFOLLOW;   /* no symlinks exist yet */
    return (uint64_t)lnxvfs_stat_path(path, (void*)ubuf);
}

static uint64_t sc_mkdir(uint64_t upath, uint64_t mode, uint64_t u3,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)mode; (void)u3; (void)u4; (void)u5; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return (uint64_t)lnxvfs_mkdir(path);
}

static uint64_t sc_rmdir(uint64_t upath, uint64_t u2, uint64_t u3,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return (uint64_t)lnxvfs_rmdir(path);
}

static uint64_t sc_unlink(uint64_t upath, uint64_t u2, uint64_t u3,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return (uint64_t)lnxvfs_unlink(path);
}

static uint64_t sc_getcwd(uint64_t ubuf, uint64_t size, uint64_t u3,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u3; (void)u4; (void)u5; (void)u6;
    return (uint64_t)lnxvfs_getcwd((char*)ubuf, (size_t)size);
}

static uint64_t sc_chdir(uint64_t upath, uint64_t u2, uint64_t u3,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return (uint64_t)lnxvfs_chdir(path);
}

static uint64_t sc_statx(uint64_t dirfd, uint64_t upath, uint64_t flags,
                         uint64_t mask, uint64_t ubuf, uint64_t u6) {
    (void)dirfd; (void)flags; (void)mask; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return (uint64_t)lnxvfs_statx_path(path, (void*)ubuf);
}

/* access(2): existence check only */
static uint64_t sc_access(uint64_t upath, uint64_t mode, uint64_t u3,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)mode; (void)u3; (void)u4; (void)u5; (void)u6;
    char path[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, path, sizeof(path)) < 0) return -L_EINVAL;
    return fs_resolve_path(path) ? 0 : (uint64_t)-L_ENOENT;
}

/* nanosleep(2): busy-wait (single CPU, syscalls run with IF=0) */
static uint64_t sc_nanosleep(uint64_t req, uint64_t u2, uint64_t u3,
                             uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    if (!process_check_user_range(req, 16)) return -L_EINVAL;
    const uint64_t* t = (const uint64_t*)req;
    uint64_t us = t[0] * 1000000ULL + t[1] / 1000ULL;
    uint64_t start = pit_uptime_us();
    while (pit_uptime_us() - start < us) { __asm__ volatile("pause"); }
    return 0;
}

/* --- Phase 1.5: fork / wait4 / execve -------------------------------- */

/* fork/vfork/clone all fork here: memory is copied (no COW), which is
 * correct for vfork+exec and for clone(CLONE_VM|CLONE_VFORK) used by
 * musl posix_spawn. */
static uint64_t sc_fork(uint64_t u1, uint64_t u2, uint64_t u3,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    return (uint64_t)process_fork(lnx_frame_rsp);
}

static uint64_t sc_wait4(uint64_t pid, uint64_t status_ptr, uint64_t options,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)options; (void)u4; (void)u5; (void)u6;
    process_t* p = process_get_current();
    if (p == NULL) return -L_ECHILD;

    process_t* child = process_find_child(p, (int)pid);
    if (child == NULL) return -L_ECHILD;

    if (child->state == PROCESS_STATE_ZOMBIE) {
        if (status_ptr) {
            if (!process_check_user_range(status_ptr, 4)) return -L_EINVAL;
            process_write_user_int(p, status_ptr,
                                   (child->exit_status & 0xFF) << 8);
        }
        int ret_pid = (int)child->pid;
        process_reap(child);
        return (uint64_t)ret_pid;
    }

    /* Live child: park this frame and run the child; process_exit()
     * resumes us with the status patched in. Never returns. */
    process_wait_run(child, status_ptr, lnx_frame_rsp);
    return 0;
}

static uint64_t sc_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)uenvp; (void)u4; (void)u5; (void)u6;
    char kpath[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, kpath, sizeof(kpath)) < 0) return -L_EINVAL;

    /* Snapshot argv into kernel memory: the user image is torn down by
     * exec before the new one is mapped. */
    const int MAX_ARGS = 8;
    char (*bufs)[96] = kmalloc((size_t)MAX_ARGS * 96);
    char** ptrs = kmalloc(((size_t)MAX_ARGS + 1) * sizeof(char*));
    if (bufs == NULL || ptrs == NULL) {
        if (bufs) kfree(bufs);
        if (ptrs) kfree(ptrs);
        return -L_ENOMEM;
    }

    int argc = 0;
    if (uargv != 0) {
        for (; argc < MAX_ARGS; argc++) {
            if (!process_check_user_range(uargv + (uint64_t)argc * 8, 8)) break;
            uint64_t p;
            memcpy(&p, (const void*)(uargv + (uint64_t)argc * 8), sizeof(p));
            if (p == 0) break;
            if (fetch_user_path(p, bufs[argc], 96) < 0) break;
            ptrs[argc] = bufs[argc];
        }
    }
    if (argc == 0) {
        strncpy(bufs[0], kpath, 95);
        bufs[0][95] = 0;
        ptrs[0] = bufs[0];
        argc = 1;
    }
    ptrs[argc] = NULL;

    int rc = exec_replace(lnx_frame_rsp, kpath, ptrs, argc);
    kfree(bufs);
    kfree(ptrs);
    if (rc != 0) return -L_ENOENT;
    return 0;   /* frame rewritten: the epilogue enters the new image */
}

static uint64_t sc_getppid(uint64_t u1, uint64_t u2, uint64_t u3,
                           uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    return (proc != NULL && proc->parent_pid > 0) ? (uint64_t)proc->parent_pid : 0;
}

static uint64_t sc_dup2(uint64_t oldfd, uint64_t newfd, uint64_t u3,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u3; (void)u4; (void)u5; (void)u6;
    return (uint64_t)lnxvfs_dup2((int)oldfd, (int)newfd);
}

static uint64_t sc_dup(uint64_t oldfd, uint64_t u2, uint64_t u3,
                       uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    if (proc == NULL || oldfd >= LNX_MAX_FDS) return -L_EBADF;
    if (oldfd >= 3 && proc->fds[oldfd] == NULL) return -L_EBADF;
    /* Allocate the lowest free descriptor above the console range */
    for (int fd = 3; fd < LNX_MAX_FDS; fd++) {
        if (proc->fds[fd] == NULL) {
            return (uint64_t)lnxvfs_dup2((int)oldfd, fd);
        }
    }
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

static uint64_t sc_readlink(uint64_t upath, uint64_t ubuf, uint64_t size,
                            uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)upath; (void)ubuf; (void)size; (void)u4; (void)u5; (void)u6;
    return -L_EINVAL;   /* no symlinks exist yet */
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
    strcpy(u->v[2], "2.12.0");
    strcpy(u->v[3], "#1 SMP Kil0yOS Phase 1");
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
    lnx_table[LNX_openat] = sc_openat;
    lnx_table[LNX_close] = sc_close;
    lnx_table[LNX_fstat] = sc_fstat;
    lnx_table[LNX_stat] = sc_stat;
    lnx_table[LNX_lstat] = sc_stat;
    lnx_table[LNX_newfstatat] = sc_newfstatat;
    lnx_table[LNX_lseek] = sc_lseek;
    lnx_table[LNX_getdents64] = sc_getdents64;
    lnx_table[LNX_mkdir] = sc_mkdir;
    lnx_table[LNX_rmdir] = sc_rmdir;
    lnx_table[LNX_unlink] = sc_unlink;
    lnx_table[LNX_getcwd] = sc_getcwd;
    lnx_table[LNX_dup] = sc_dup;
    lnx_table[LNX_dup2] = sc_dup2;
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
    lnx_table[LNX_readlink] = sc_readlink;
    lnx_table[LNX_set_robust_list] = sc_stub_ok;

    /* Phase 1.5: process syscalls */
    lnx_table[LNX_fork] = sc_fork;
    lnx_table[LNX_vfork] = sc_fork;
    lnx_table[LNX_clone] = sc_fork;
    lnx_table[LNX_wait4] = sc_wait4;
    lnx_table[LNX_execve] = sc_execve;
    lnx_table[LNX_chdir] = sc_chdir;
    lnx_table[LNX_statx] = sc_statx;
    lnx_table[LNX_access] = sc_access;
    lnx_table[LNX_nanosleep] = sc_nanosleep;
    lnx_table[LNX_getppid] = sc_getppid;

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
    uint64_t rv = lnx_table[num](a0, a1, a2, a3, a4, a5);
    return rv;
}
