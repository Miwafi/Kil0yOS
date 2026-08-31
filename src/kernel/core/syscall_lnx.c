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
#include "net/netif.h"
#include "net/tcp.h"
#include "net/udp.h"

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
#define L_EAGAIN  11
#define L_EIO     5
#define L_ENOTSOCK 88
#define L_EMSGSIZE 90
#define L_ECONNREFUSED 111
#define L_EADDRINUSE 98

/* socket fds live above the file-fd range (sc_close needs this early) */
#define SOCK_FD_BASE   64
#define MSG_KBUF_SZ    4096   /* kernel staging buffer for msg coalescing */
#define SOCK_FD_COUNT  64

/* Socket types shared by the syscall layer (needed by sc_read/sc_poll
 * which run before the Phase 3.3 section below). */
#define LNX_AF_INET     2
#define LNX_SOCK_STREAM 1
#define LNX_SOCK_DGRAM  2
#define LNX_MSG_PEEK    2

struct lnx_sockaddr_in {
    uint16_t family;
    uint16_t port;    /* network byte order */
    uint32_t addr;    /* network byte order */
    uint8_t  zero[8];
};

enum sock_kind { SOCK_NONE = 0, SOCK_TCP, SOCK_UDP };

static void* sock_fd_get(uint64_t fd, enum sock_kind* kind);

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
#define LNX_readv           19
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
#define LNX_pread64        17
#define LNX_rseq          334
#define LNX_prlimit64     302
#define LNX_fcntl          72
#define LNX_chdir          81
#define LNX_getppid        110

/* Legacy interval timers: musl alarm()/ualarm() are built on setitimer;
 * busybox wget calls alarm() for its transfer timeout. A "succeed but
 * never fire" stub is enough - nothing depends on real SIGALRM. */
#define LNX_getitimer      36
#define LNX_setitimer      38

/* Phase 3.3 socket syscalls */
#define LNX_poll            7
#define LNX_socket         41
#define LNX_connect        42
#define LNX_accept         43
#define LNX_sendto         44
#define LNX_recvfrom       45
#define LNX_shutdown       48
#define LNX_getsockname    51
#define LNX_getpeername    52
#define LNX_setsockopt     54
#define LNX_getsockopt     55
#define LNX_sendmsg        46
#define LNX_recvmsg        47
#define LNX_bind           49

/* Entry-frame pointer saved by syscall_lnx_entry (isr.asm): points at
 * the first saved register (r15) of the interrupted user context. fork/
 * wait4/execve clone or rewrite this frame. */
extern uint64_t lnx_frame_rsp;

/* socket fd close (defined in the Phase 3.3 socket section below) */
static void sock_fd_close(uint64_t fd);

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
    if (count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) return -L_EINVAL;

    /* Socket fds: musl recv()/busybox code paths often fall back to plain
     * read(2) on the socket (e.g. busybox nslookup reads the DNS reply
     * with read(fd) after poll()). */
    if (fd >= SOCK_FD_BASE) {
        enum sock_kind kind;
        void* s = sock_fd_get(fd, &kind);
        if (s == NULL) return -L_EBADF;
        if (kind == SOCK_TCP) {
            int r = tcp_recv((tcp_socket_t*)s, (uint8_t*)buf,
                             (uint16_t)(count > 0xFFFF ? 0xFFFF : count), 15000);
            return r < 0 ? (uint64_t)-L_EIO : (uint64_t)r;
        }
        uint8_t kbuf[2048];
        uint32_t src_ip = 0;
        uint16_t src_port = 0;
        uint16_t want = (uint16_t)(count > sizeof(kbuf) ? sizeof(kbuf) : count);
        int r = udp_recvfrom((udp_socket_t*)s, kbuf, want, &src_ip, &src_port, 15000);
        if (r < 0) return (uint64_t)-L_EAGAIN;
        memcpy((void*)buf, kbuf, (size_t)r);
        return (uint64_t)r;
    }

    if (fd >= LNX_MAX_FDS) return -L_EBADF;
    if (fd_is_file(fd)) return (uint64_t)lnxvfs_read((int)fd, (char*)buf, (size_t)count);
    if (fd != 0) return -L_EBADF;
    return (uint64_t)tty_read((char*)buf, (size_t)count);
}

static uint64_t sc_write(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) return -L_EINVAL;

    /* write(2) on a connected socket (busybox nslookup sends DNS queries
     * with plain write after connect()). */
    if (fd >= SOCK_FD_BASE) {
        enum sock_kind kind;
        void* s = sock_fd_get(fd, &kind);
        if (s == NULL) return -L_EBADF;
        if (kind == SOCK_TCP) {
            int r = tcp_send((tcp_socket_t*)s, (const uint8_t*)buf,
                             (uint16_t)(count > 0xFFFF ? 0xFFFF : count));
            return r < 0 ? (uint64_t)-L_EIO : (uint64_t)r;
        }
        udp_socket_t* us = (udp_socket_t*)s;
        if (us->remote_ip == 0) return (uint64_t)-L_EINVAL;   /* not connected */
        int r = udp_sendto(us, us->remote_ip, us->remote_port,
                           (const uint8_t*)buf, (uint16_t)count);
        return r < 0 ? (uint64_t)-L_EIO : (uint64_t)count;
    }

    if (fd >= LNX_MAX_FDS) return -L_EBADF;
    if (fd_is_file(fd)) return (uint64_t)lnxvfs_write((int)fd, (const char*)buf, (size_t)count);
    if (fd != 1 && fd != 2) return -L_EBADF;
    return (uint64_t)tty_write((const char*)buf, (size_t)count);
}

struct lnx_iovec { uint64_t base; uint64_t len; };

static uint64_t sc_writev(uint64_t fd, uint64_t iov, uint64_t iovcnt,
                          uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (iovcnt == 0) return 0;
    if (iovcnt > 1024) return -L_EINVAL;
    if (!process_check_user_range(iov, (size_t)(iovcnt * sizeof(struct lnx_iovec)))) {
        return -L_EINVAL;
    }
    /* writev(2) on a socket: musl stdio flushes FILE* output through
     * writev (busybox wget sends its HTTP GET request this way), so the
     * vectors must be coalesced and handed to the TCP/UDP layer. NOTE:
     * socket fds (>= SOCK_FD_BASE) live above the file-fd table, so this
     * branch MUST come before the LNX_MAX_FDS bound check below. */
    if (fd >= SOCK_FD_BASE) {
        enum sock_kind kind;
        void* s = sock_fd_get(fd, &kind);
        if (s == NULL) return -L_EBADF;
        const struct lnx_iovec* v0 = (const struct lnx_iovec*)iov;
        uint8_t kbuf[MSG_KBUF_SZ];
        size_t total = 0;
        for (uint64_t i = 0; i < iovcnt; i++) {
            if (v0[i].len == 0) continue;
            size_t n = v0[i].len;
            if (n > sizeof(kbuf) - total) n = sizeof(kbuf) - total;
            if (!process_check_user_range(v0[i].base, v0[i].len)) return -L_EINVAL;
            memcpy(kbuf + total, (const void*)v0[i].base, n);
            total += n;
        }
        if (total == 0) return 0;
        if (kind == SOCK_TCP) {
            int r = tcp_send((tcp_socket_t*)s, kbuf, (uint16_t)total);
            return r < 0 ? (uint64_t)-L_EIO : (uint64_t)total;
        }
        udp_socket_t* us = (udp_socket_t*)s;
        if (us->remote_ip == 0) return (uint64_t)-L_EINVAL;   /* not connected */
        int r = udp_sendto(us, us->remote_ip, us->remote_port,
                           kbuf, (uint16_t)total);
        (void)r;
        return (uint64_t)total;
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
    if (fd >= LNX_MAX_FDS) return -L_EBADF;
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

/* readv(2): musl stdio pulls FILE* input through readv (two vectors:
 * the caller's buffer plus the internal one), so busybox wget reads the
 * HTTP response - and getaddrinfo reads /etc/hosts - via this syscall.
 * A short read is valid POSIX; sockets fill just the first non-empty
 * buffer, files loop until EOF. */
static uint64_t sc_readv(uint64_t fd, uint64_t uiov, uint64_t iovcnt,
                         uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (iovcnt == 0 || iovcnt > 16) return -L_EINVAL;
    if (!process_check_user_range(uiov, (size_t)(iovcnt * sizeof(struct lnx_iovec))))
        return -L_EINVAL;
    struct lnx_iovec iv[16];
    memcpy(iv, (const void*)uiov, (size_t)(iovcnt * sizeof(struct lnx_iovec)));

    if (fd >= SOCK_FD_BASE) {
        enum sock_kind kind;
        void* s = sock_fd_get(fd, &kind);
        if (s == NULL) return -L_EBADF;
        if (kind == SOCK_TCP) {
            for (uint64_t i = 0; i < iovcnt; i++) {
                if (iv[i].len == 0) continue;
                if (!process_check_user_range(iv[i].base, (size_t)iv[i].len))
                    return -L_EINVAL;
                int r = tcp_recv((tcp_socket_t*)s, (uint8_t*)iv[i].base,
                                 (uint16_t)(iv[i].len > 0xFFFF ? 0xFFFF : iv[i].len),
                                 15000);
                return r < 0 ? (uint64_t)-L_EIO : (uint64_t)r;
            }
            return 0;
        }
        for (uint64_t i = 0; i < iovcnt; i++) {
            if (iv[i].len == 0) continue;
            if (!process_check_user_range(iv[i].base, (size_t)iv[i].len))
                return -L_EINVAL;
            uint8_t kbuf[2048];
            uint32_t src_ip = 0;
            uint16_t src_port = 0;
            uint16_t want = (uint16_t)(iv[i].len > sizeof(kbuf) ? sizeof(kbuf) : iv[i].len);
            int r = udp_recvfrom((udp_socket_t*)s, kbuf, want,
                                 &src_ip, &src_port, 15000);
            if (r < 0) return (uint64_t)-L_EAGAIN;
            memcpy((void*)iv[i].base, kbuf, (size_t)r);
            return (uint64_t)r;
        }
        return 0;
    }

    if (fd >= LNX_MAX_FDS) return -L_EBADF;
    uint64_t total = 0;
    if (fd_is_file(fd)) {
        for (uint64_t i = 0; i < iovcnt; i++) {
            if (iv[i].len == 0) continue;
            if (!process_check_user_range(iv[i].base, (size_t)iv[i].len))
                return -L_EINVAL;
            long r = lnxvfs_read((int)fd, (char*)iv[i].base, (size_t)iv[i].len);
            if (r < 0) return total > 0 ? total : (uint64_t)r;
            total += (uint64_t)r;
            if ((size_t)r < iv[i].len) break;   /* EOF */
        }
        return total;
    }
    if (fd != 0) return -L_EBADF;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iv[i].len == 0) continue;
        if (!process_check_user_range(iv[i].base, (size_t)iv[i].len))
            return -L_EINVAL;
        long r = tty_read((char*)iv[i].base, (size_t)iv[i].len);
        if (r < 0) return total > 0 ? total : (uint64_t)r;
        total += (uint64_t)r;
        if (total > 0) break;   /* one read per call is fine for a tty */
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
    if (fd >= SOCK_FD_BASE) { sock_fd_close(fd); return 0; }
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

/* Phase 0.2: anonymous mmap. Phase 3.0: file-backed MAP_PRIVATE (the
 * musl loader maps libraries this way). File content is eagerly copied
 * into fresh anonymous pages at map time - the VFS keeps whole-file
 * caches, so there is no demand paging to defer to. */
#define LNX_MAP_PRIVATE 0x02

static uint64_t sc_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t off) {
    process_t* proc = process_get_current();
    if (proc == NULL) return -L_ENOMEM;
    if (len == 0) return -L_EINVAL;
    if (len > UVM_MMAP_LIMIT - UVM_MMAP_BASE) return -L_ENOMEM;
    if (prot & ~7ULL) return -L_EINVAL;
    if ((flags & LNX_MAP_ANONYMOUS) == 0 && (off & 0xFFF)) return -L_EINVAL;

    uint32_t p = 0;
    if (prot & 1) p |= UVM_PROT_READ;
    if (prot & 2) p |= UVM_PROT_WRITE;
    if (prot & 4) p |= UVM_PROT_EXEC;

    int is_file = !(flags & LNX_MAP_ANONYMOUS);
    if (is_file) {
        if (fd >= LNX_MAX_FDS || !fd_is_file(fd)) {
            klog("[mmap] file: bad fd\n");
            return -L_EBADF;
        }
    }

    uint64_t va = uvm_mmap_anon(proc, addr, (size_t)len, p,
                                (flags & LNX_MAP_FIXED) != 0);
    if (va == 0) {
        klog("[mmap] file: uvm_mmap_anon failed\n");
        return -L_ENOMEM;
    }

    if (is_file) {
        long long fsize = lnxvfs_filesize((int)fd);
        if (fsize < 0) {
            klog("[mmap] file: bad fsize\n");
            return -L_EBADF;
        }
        /* Copy [off, off+len) clamped to EOF; pages past EOF stay zero
         * (fresh anonymous pages), matching Linux file-map semantics. */
        uint64_t done = 0;
        uint8_t chunk[1024];   /* modest: 16 KB kernel stack */
        while (done < len) {
            uint64_t pos = off + done;
            size_t n = sizeof(chunk);
            if (n > len - done) n = (size_t)(len - done);
            if (pos >= (uint64_t)fsize) break;
            if ((uint64_t)fsize - pos < n) n = (size_t)((uint64_t)fsize - pos);
            int r = lnxvfs_pread((int)fd, chunk, n, (long long)pos);
            if (r <= 0) return (uint64_t)-L_EIO;
            uvm_write_user_va(proc, va + done, chunk, (size_t)r);
            done += (uint64_t)r;
        }
    }
    return va;
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

/* setitimer(which, new, old) / getitimer(which, cur): the timer never
 * fires, but callers (musl alarm()) read back the old-value struct, so
 * zero it instead of leaving uninitialized user stack. */
static uint64_t sc_itimer(uint64_t which, uint64_t unew, uint64_t uold,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)which; (void)u4; (void)u5; (void)u6;
    if (unew != 0 && !process_check_user_range(unew, 32)) return -L_EINVAL;
    if (uold != 0) {
        if (!process_check_user_range(uold, 32)) return -L_EINVAL;
        memset((void*)uold, 0, 32);   /* struct itimerval = 2 x timeval */
    }
    return 0;
}

static uint64_t sc_getuid_like(uint64_t u1, uint64_t u2, uint64_t u3,
                               uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    return 0;   /* root */
}

/* futex(2): Phase 3.2 minimal. Programs are single-threaded, so only
 * the uncontended paths are exercised: WAIT would only be called when
 * another thread should wake us (impossible here), WAKE wakes nothing. */
#define LNX_FUTEX_WAIT 0
#define LNX_FUTEX_WAKE 1

static uint64_t sc_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    switch (op & 0x7F) {
    case LNX_FUTEX_WAIT:
        if (!process_check_user_range(uaddr, 4)) return -L_EINVAL;
        /* Linux returns EAGAIN when the value raced; otherwise block.
         * We cannot be raced (no threads), so pretending we were woken
         * is equivalent to the uncontended fast path. */
        if (*(volatile uint32_t*)uaddr != (uint32_t)val) return -L_EAGAIN;
        return 0;
    case LNX_FUTEX_WAKE:
        return 0;
    default:
        return 0;
    }
}

/* pread64(2): glibc's loader reads program headers with it. */
static uint64_t sc_pread64(uint64_t fd, uint64_t ubuf, uint64_t count,
                           uint64_t off, uint64_t u5, uint64_t u6) {
    (void)u5; (void)u6;
    if (fd >= LNX_MAX_FDS || !fd_is_file(fd)) return -L_EBADF;
    if (count > 0 && !process_check_user_range(ubuf, count)) return -L_EINVAL;
    uint64_t done = 0;
    uint8_t kbuf[512];
    while (done < count) {
        size_t n = sizeof(kbuf);
        if (n > count - done) n = (size_t)(count - done);
        int r = lnxvfs_pread((int)fd, kbuf, n, (long long)(off + done));
        if (r < 0) return done ? done : (uint64_t)r;   /* r is -errno */
        if (r == 0) break;
        memcpy((void*)(ubuf + done), kbuf, (size_t)r);
        done += (uint64_t)r;
    }
    return done;
}

/* rseq(2): glibc 2.35+ registers per-thread restartable sequences at
 * startup. Succeeding avoids its ENOSYS retry dance. */
static uint64_t sc_rseq(uint64_t u1, uint64_t u2, uint64_t u3,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u1; (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    return 0;
}

/* prlimit64(2): glibc startup queries RLIMIT_STACK (thread sizing). */
struct lnx_rlimit64 { uint64_t cur, max; };
#define LNX_RLIMIT_STACK   3
#define LNX_RLIMIT_NOFILE  7

static uint64_t sc_prlimit64(uint64_t pid, uint64_t res, uint64_t unew,
                             uint64_t uold, uint64_t u5, uint64_t u6) {
    (void)pid; (void)u5; (void)u6;
    if (unew != 0) return -L_EPERM;
    if (uold == 0) return 0;
    if (!process_check_user_range(uold, sizeof(struct lnx_rlimit64))) return -L_EINVAL;
    struct lnx_rlimit64* r = (struct lnx_rlimit64*)uold;
    switch (res) {
    case LNX_RLIMIT_STACK:
        r->cur = 8ULL * 1024 * 1024;
        r->max = ~0ULL;
        return 0;
    case LNX_RLIMIT_NOFILE:
        r->cur = LNX_MAX_FDS;
        r->max = LNX_MAX_FDS;
        return 0;
    default:
        r->cur = ~0ULL;
        r->max = ~0ULL;
        return 0;
    }
}

/* --- Phase 3.3: sockets --------------------------------------------------
 * Socket fds live in their own high range (SOCK_FD_BASE..) so they never
 * collide with the lnxvfs file-fd table (LNX_MAX_FDS = 16 entries). The
 * fd layer routes: fd < 16 -> file/tty, fd >= 64 -> socket table. */

static struct {
    enum sock_kind kind;
    void* sock;
} sock_fds[SOCK_FD_COUNT];

static int sock_fd_alloc(enum sock_kind kind, void* sock) {
    for (int i = 0; i < SOCK_FD_COUNT; i++) {
        if (sock_fds[i].kind == SOCK_NONE) {
            sock_fds[i].kind = kind;
            sock_fds[i].sock = sock;
            return SOCK_FD_BASE + i;
        }
    }
    return -1;
}

static void* sock_fd_get(uint64_t fd, enum sock_kind* kind) {
    if (fd < SOCK_FD_BASE || fd >= SOCK_FD_BASE + SOCK_FD_COUNT) return NULL;
    if (sock_fds[fd - SOCK_FD_BASE].kind == SOCK_NONE) return NULL;
    if (kind) *kind = sock_fds[fd - SOCK_FD_BASE].kind;
    return sock_fds[fd - SOCK_FD_BASE].sock;
}

static void sock_fd_free(uint64_t fd) {
    if (fd >= SOCK_FD_BASE && fd < SOCK_FD_BASE + SOCK_FD_COUNT) {
        sock_fds[fd - SOCK_FD_BASE].kind = SOCK_NONE;
        sock_fds[fd - SOCK_FD_BASE].sock = NULL;
    }
}

/* Close a socket fd: TCP does the active-close handshake (best effort,
 * bounded), UDP just frees. Always releases the fd slot. */
static void sock_fd_close(uint64_t fd) {
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return;
    if (kind == SOCK_TCP) tcp_close((tcp_socket_t*)s);
    else                  udp_socket_close((udp_socket_t*)s);
    sock_fd_free(fd);
}

static uint64_t sc_socket(uint64_t domain, uint64_t type, uint64_t proto,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    /* musl passes SOCK_CLOEXEC(0x80000)/SOCK_NONBLOCK(0x800) inside type */
    uint64_t t = type & 0xFF;
    if (domain != LNX_AF_INET) return -L_EINVAL;
    if (proto != 0 && !(domain == LNX_AF_INET && t == LNX_SOCK_STREAM && proto == 6) &&
        !(domain == LNX_AF_INET && t == LNX_SOCK_DGRAM && proto == 17)) {
        return -L_EINVAL;
    }
    if (t == LNX_SOCK_STREAM) {
        tcp_socket_t* s = tcp_socket_create();
        if (s == NULL) return -L_ENOMEM;
        int fd = sock_fd_alloc(SOCK_TCP, s);
        if (fd < 0) { tcp_socket_close(s); return -L_ENOMEM; }
        return (uint64_t)fd;
    }
    if (t == LNX_SOCK_DGRAM) {
        udp_socket_t* s = udp_socket_create();
        if (s == NULL) return -L_ENOMEM;
        int fd = sock_fd_alloc(SOCK_UDP, s);
        if (fd < 0) { udp_socket_close(s); return -L_ENOMEM; }
        return (uint64_t)fd;
    }
    return -L_EINVAL;
}

static uint64_t sc_connect(uint64_t fd, uint64_t uaddr, uint64_t addrlen,
                           uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_ENOTSOCK;
    if (addrlen < sizeof(struct lnx_sockaddr_in)) return -L_EINVAL;
    if (!process_check_user_range(uaddr, sizeof(struct lnx_sockaddr_in))) return -L_EINVAL;
    const struct lnx_sockaddr_in* a = (const struct lnx_sockaddr_in*)uaddr;
    if (a->family != LNX_AF_INET) return -L_EINVAL;
    uint32_t ip = net_ntohl(a->addr);
    uint16_t port = net_ntohs(a->port);
    if (ip == 0 || port == 0) return -L_EINVAL;

    if (kind == SOCK_TCP) {
        if (tcp_connect((tcp_socket_t*)s, ip, port) != 0) return -L_ECONNREFUSED;
        return 0;
    }
    /* UDP connect: just record the peer. Do NOT bind here - binding with
     * port 0 would make udp_receive drop every reply (demux matches
     * local_port). udp_sendto assigns the ephemeral port on first send. */
    udp_socket_t* us = (udp_socket_t*)s;
    us->remote_ip = ip;
    us->remote_port = port;
    return 0;
}

/* fcntl: only the flag commands musl/busybox actually issue on our
 * sockets/files. The fds themselves are always blocking internally;
 * callers that want O_NONBLOCK get it accepted but poll() still works. */
static uint64_t sc_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                         uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)fd; (void)arg; (void)u4; (void)u5; (void)u6;
    switch (cmd) {
    case 1:   /* F_GETFD */
    case 3:   /* F_GETFL */
        return 2;                     /* O_RDWR */
    case 0:   /* F_DUPFD */
        return fd;
    case 2:   /* F_SETFD */
    case 4:   /* F_SETFL */
        return 0;
    default:
        return (uint64_t)-L_EINVAL;
    }
}

static uint64_t sc_bind(uint64_t fd, uint64_t uaddr, uint64_t addrlen,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_ENOTSOCK;
    if (addrlen < sizeof(struct lnx_sockaddr_in)) return -L_EINVAL;
    if (!process_check_user_range(uaddr, sizeof(struct lnx_sockaddr_in))) return -L_EINVAL;
    const struct lnx_sockaddr_in* a = (const struct lnx_sockaddr_in*)uaddr;
    if (a->family != LNX_AF_INET) return -L_EINVAL;
    uint16_t port = net_ntohs(a->port);

    if (kind == SOCK_TCP) return 0;   /* client sockets: bind is a no-op */
    /* UDP: port 0 means "kernel picks an ephemeral port". Defer to the
     * first udp_sendto (which assigns one) - binding port 0 here would
     * make udp_receive demux drop every reply. */
    udp_socket_t* us = (udp_socket_t*)s;
    if (port == 0) { us->bound = 1; return 0; }
    return udp_bind(us, port) == 0 ? 0 : (uint64_t)-L_EADDRINUSE;
}

static uint64_t sock_sendto(uint64_t fd, uint64_t buf, uint64_t len,
                            uint64_t flags, uint64_t uaddr, uint64_t addrlen) {
    (void)flags;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_EBADF;
    if (len == 0) return 0;
    if (len > NET_MTU && kind == SOCK_UDP) return -L_EMSGSIZE;
    if (!process_check_user_range(buf, (size_t)len)) return -L_EINVAL;

    uint32_t dst_ip = 0;
    uint16_t dst_port = 0;
    int have_dst = 0;
    if (uaddr != 0) {
        if (addrlen < sizeof(struct lnx_sockaddr_in)) return -L_EINVAL;
        if (!process_check_user_range(uaddr, sizeof(struct lnx_sockaddr_in))) return -L_EINVAL;
        const struct lnx_sockaddr_in* a = (const struct lnx_sockaddr_in*)uaddr;
        dst_ip = net_ntohl(a->addr);
        dst_port = net_ntohs(a->port);
        have_dst = 1;
    }

    if (kind == SOCK_TCP) {
        if (have_dst) return -L_EINVAL;
        int r = tcp_send((tcp_socket_t*)s, (const uint8_t*)buf, (uint16_t)len);
        return r < 0 ? (uint64_t)-L_EIO : (uint64_t)r;
    }
    udp_socket_t* us = (udp_socket_t*)s;
    if (!have_dst) {
        dst_ip = us->remote_ip;
        dst_port = us->remote_port;
        if (dst_ip == 0) return -L_EINVAL;
    }
    int r = udp_sendto(us, dst_ip, dst_port, (const uint8_t*)buf, (uint16_t)len);
    return r < 0 ? (uint64_t)-L_EIO : (uint64_t)len;
}

static uint64_t sc_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                          uint64_t uaddr, uint64_t addrlen) {
    return sock_sendto(fd, buf, len, flags, uaddr, addrlen);
}

static uint64_t sc_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                            uint64_t uaddr, uint64_t uaddrlen) {
    (void)uaddrlen;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_EBADF;
    if (len == 0) return 0;
    if (!process_check_user_range(buf, (size_t)len)) return -L_EINVAL;

    if (kind == SOCK_TCP) {
        int r = tcp_recv((tcp_socket_t*)s, (uint8_t*)buf,
                         (uint16_t)(len > 0xFFFF ? 0xFFFF : len), 15000);
        if (r < 0) return -L_EIO;
        return (uint64_t)r;
    }
    /* UDP */
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    uint8_t kbuf[2048];
    uint16_t want = (uint16_t)(len > sizeof(kbuf) ? sizeof(kbuf) : len);
    int r = udp_recvfrom((udp_socket_t*)s, kbuf, want, &src_ip, &src_port, 15000);
    if (r < 0) return -L_EAGAIN;
    if (flags & LNX_MSG_PEEK) {
        /* single-buffer rx: peek = copy without consuming is not supported;
         * report data length but leave the buffer drained by the copy-out */
    }
    memcpy((void*)buf, kbuf, (size_t)r);
    if (uaddr != 0) {
        if (!process_check_user_range(uaddr, sizeof(struct lnx_sockaddr_in))) return -L_EINVAL;
        struct lnx_sockaddr_in* a = (struct lnx_sockaddr_in*)uaddr;
        a->family = LNX_AF_INET;
        a->port = net_htons(src_port);
        a->addr = net_htonl(src_ip);
        memset(a->zero, 0, sizeof(a->zero));
    }
    return (uint64_t)r;
}

/* --- sendmsg/recvmsg: musl's DNS resolver (res_msend) talks to the
 * nameservers exclusively through sendto/sendmsg/recvmsg. Only the
 * linear, no-cmsg subset is supported - exactly what the resolver and
 * busybox use. Buffers are small (DNS queries/replies); the kernel-side
 * staging buffer stays well under the 16 KB kernel stack. */
struct lnx_msghdr {
    void*    msg_name;       /* optional sockaddr_in (in/out) */
    uint32_t msg_namelen;
    struct lnx_iovec* msg_iov;
    uint64_t msg_iovlen;
    void*    msg_control;
    uint64_t msg_controllen;
    int      msg_flags;
};

static uint64_t sc_sendmsg(uint64_t fd, uint64_t umsg, uint64_t flags,
                           uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)flags; (void)u4; (void)u5; (void)u6;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_ENOTSOCK;
    if (!process_check_user_range(umsg, sizeof(struct lnx_msghdr))) return -L_EINVAL;
    const struct lnx_msghdr* mh = (const struct lnx_msghdr*)umsg;
    if (mh->msg_iovlen == 0 || mh->msg_iovlen > 1024) return -L_EMSGSIZE;
    if (!process_check_user_range((uint64_t)mh->msg_iov,
                                  mh->msg_iovlen * sizeof(struct lnx_iovec)))
        return -L_EINVAL;

    uint8_t kbuf[MSG_KBUF_SZ];
    size_t total = 0;
    const struct lnx_iovec* iv = mh->msg_iov;
    for (uint64_t i = 0; i < mh->msg_iovlen; i++) {
        if (iv[i].len == 0) continue;
        size_t n = iv[i].len;
        if (n > MSG_KBUF_SZ - total) n = MSG_KBUF_SZ - total;
        if (!process_check_user_range((uint64_t)iv[i].base, n)) return -L_EINVAL;
        memcpy(kbuf + total, iv[i].base, n);
        total += n;
    }
    if (total == 0) return 0;
    if (total > 0xFFFF) return -L_EMSGSIZE;

    uint32_t dst_ip = 0;
    uint16_t dst_port = 0;
    int have_dst = 0;
    if (mh->msg_name != NULL && mh->msg_namelen >= sizeof(struct lnx_sockaddr_in)) {
        if (!process_check_user_range((uint64_t)mh->msg_name,
                                      sizeof(struct lnx_sockaddr_in)))
            return -L_EINVAL;
        const struct lnx_sockaddr_in* a = (const struct lnx_sockaddr_in*)mh->msg_name;
        if (a->family == LNX_AF_INET) {
            dst_ip = net_ntohl(a->addr);
            dst_port = net_ntohs(a->port);
            have_dst = 1;
        }
    }

    if (kind == SOCK_TCP) {
        if (have_dst) return -L_EINVAL;
        int r = tcp_send((tcp_socket_t*)s, kbuf, (uint16_t)total);
        return r < 0 ? (uint64_t)-L_EIO : (uint64_t)r;
    }
    udp_socket_t* us = (udp_socket_t*)s;
    if (!have_dst) {
        dst_ip = us->remote_ip;
        dst_port = us->remote_port;
    }
    if (dst_ip == 0) return -L_EINVAL;
    int r = udp_sendto(us, dst_ip, dst_port, kbuf, (uint16_t)total);
    return r < 0 ? (uint64_t)-L_EIO : (uint64_t)total;
}

static uint64_t sc_recvmsg(uint64_t fd, uint64_t umsg, uint64_t flags,
                           uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)flags; (void)u4; (void)u5; (void)u6;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_ENOTSOCK;
    if (!process_check_user_range(umsg, sizeof(struct lnx_msghdr))) return -L_EINVAL;
    struct lnx_msghdr* mh = (struct lnx_msghdr*)umsg;
    if (mh->msg_iov == NULL || mh->msg_iovlen == 0 || mh->msg_iovlen > 1024)
        return -L_EINVAL;
    if (!process_check_user_range((uint64_t)mh->msg_iov,
                                  mh->msg_iovlen * sizeof(struct lnx_iovec)))
        return -L_EINVAL;

    uint8_t kbuf[MSG_KBUF_SZ];
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    long r;
    if (kind == SOCK_TCP) {
        r = tcp_recv((tcp_socket_t*)s, kbuf, MSG_KBUF_SZ, 15000);
        if (r < 0) return -L_EIO;
    } else {
        int rr = udp_recvfrom((udp_socket_t*)s, kbuf, MSG_KBUF_SZ,
                              &src_ip, &src_port, 15000);
        if (rr < 0) return (uint64_t)-L_EAGAIN;
        r = rr;
    }

    /* scatter into the user iovecs */
    size_t left = (size_t)r;
    size_t copied = 0;
    const struct lnx_iovec* iv = mh->msg_iov;
    for (uint64_t i = 0; i < mh->msg_iovlen && left > 0; i++) {
        size_t n = iv[i].len;
        if (n > left) n = left;
        if (n != 0) {
            if (!process_check_user_range((uint64_t)iv[i].base, n)) return -L_EINVAL;
            memcpy(iv[i].base, kbuf + copied, n);
        }
        copied += n;
        left -= n;
    }

    if (mh->msg_name != NULL && mh->msg_namelen >= sizeof(struct lnx_sockaddr_in)) {
        struct lnx_sockaddr_in* sa = (struct lnx_sockaddr_in*)mh->msg_name;
        if (!process_check_user_range((uint64_t)sa, sizeof(*sa))) return -L_EINVAL;
        sa->family = LNX_AF_INET;
        sa->port = net_htons(src_port);
        sa->addr = net_htonl(src_ip);
        memset(sa->zero, 0, sizeof(sa->zero));
        mh->msg_namelen = sizeof(struct lnx_sockaddr_in);
    }
    mh->msg_flags = 0;
    if (mh->msg_control != NULL) mh->msg_controllen = 0;
    return (uint64_t)copied;
}

/* poll(2): supports sockets + files/console. pollfd layout matches Linux
 * x86-64: { int fd; short events; short revents; }. */
#define LNX_POLLIN   0x001
#define LNX_POLLOUT  0x004
#define LNX_POLLERR  0x008
#define LNX_POLLNVAL 0x020

struct lnx_pollfd { int32_t fd; int16_t events; int16_t revents; };

static uint64_t sc_poll(uint64_t ufds, uint64_t nfds, uint64_t timeout_ms,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    if (nfds == 0) return 0;
    if (nfds > 256) return -L_EINVAL;
    if (!process_check_user_range(ufds, nfds * sizeof(struct lnx_pollfd))) return -L_EINVAL;
    struct lnx_pollfd* fds = (struct lnx_pollfd*)ufds;

    uint32_t waited = 0;
    while (1) {
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            int16_t rev = 0;
            int32_t fd = fds[i].fd;
            if (fd >= SOCK_FD_BASE) {
                enum sock_kind kind;
                void* s = sock_fd_get((uint64_t)fd, &kind);
                if (s == NULL) { rev = LNX_POLLNVAL; }
                else if (kind == SOCK_TCP) {
                    if (tcp_rx_ready((tcp_socket_t*)s)) rev |= LNX_POLLIN;
                    rev |= LNX_POLLOUT;
                } else {
                    udp_socket_t* us = (udp_socket_t*)s;
                    if (us->rx_count != 0) rev |= LNX_POLLIN;
                    rev |= LNX_POLLOUT;
                }
            } else if (fd >= 0 && fd < (int32_t)LNX_MAX_FDS && fd_is_file((uint64_t)fd)) {
                rev = LNX_POLLIN | LNX_POLLOUT;
            } else if (fd >= 0 && fd <= 2) {
                rev = LNX_POLLIN | LNX_POLLOUT;
            } else {
                rev = LNX_POLLNVAL;
            }
            fds[i].revents = (int16_t)(rev & (fds[i].events | LNX_POLLERR | LNX_POLLNVAL));
            if (fds[i].revents) ready++;
        }
        if (ready != 0) return (uint64_t)ready;
        if (timeout_ms == 0) return 0;
        if (timeout_ms != 0xFFFFFFFF && waited >= timeout_ms) return 0;
        netif_poll();
        pit_delay_ms(2);
        waited += 2;
    }
}

/* getsockopt: report success / zeroed values (enough for SO_ERROR etc.) */
static uint64_t sc_getsockopt_stub(uint64_t fd, uint64_t level, uint64_t optname,
                                   uint64_t optval, uint64_t optlen, uint64_t u6) {
    (void)fd; (void)level; (void)optname; (void)u6;
    if (optval != 0 && optlen >= 4) {
        if (!process_check_user_range(optval, 4)) return -L_EINVAL;
        *(uint32_t*)optval = 0;
    }
    return 0;
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
    strcpy(u->v[2], "2.14.0");
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
    lnx_table[LNX_readv] = sc_readv;
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
    lnx_table[LNX_getitimer] = sc_itimer;
    lnx_table[LNX_setitimer] = sc_itimer;
    lnx_table[LNX_pread64] = sc_pread64;
    lnx_table[LNX_rseq] = sc_rseq;
    lnx_table[LNX_prlimit64] = sc_prlimit64;

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

    /* Phase 3.3: sockets + poll */
    lnx_table[LNX_socket]     = sc_socket;
    lnx_table[LNX_bind]       = sc_bind;
    lnx_table[LNX_fcntl]      = sc_fcntl;
    lnx_table[LNX_connect]    = sc_connect;
    lnx_table[LNX_sendto]     = sc_sendto;
    lnx_table[LNX_recvfrom]   = sc_recvfrom;
    lnx_table[LNX_sendmsg]    = sc_sendmsg;
    lnx_table[LNX_recvmsg]    = sc_recvmsg;
    lnx_table[LNX_poll]       = sc_poll;
    lnx_table[LNX_shutdown]   = sc_close;          /* close semantics suffice */
    lnx_table[LNX_setsockopt] = sc_stub_ok;        /* common no-op options */
    lnx_table[LNX_getsockopt] = sc_getsockopt_stub;

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
