#include "core/syscall_lnx.h"
#include "core/syscall.h"
#include "core/process.h"
#include "core/uvm.h"
#include "core/elf.h"
#include "mm/memory.h"
#include "timer/pit.h"
#include "sched/wait.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/video/vga.h"
#include "drivers/input/keyboard.h"
#include "drivers/io.h"
#include "core/tty.h"
#include "core/lnxvfs.h"
#include "fs/fs.h"
#include "net/netif.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "net/unix_sock.h"

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
#define L_EPIPE     32
#define L_EISCONN  106
#define L_ENOTCONN 107
#define L_EOPNOTSUPP 95
#define L_ECONNRESET 104

/* socket fd plumbing now rides the unified per-process fd table (P1.2):
 * sockets are struct file entries (FILE_T_SOCK_TCP/UDP), no high range */
#define MSG_KBUF_SZ    4096   /* kernel staging buffer for msg coalescing */

/* Socket types shared by the syscall layer (needed by sc_read/sc_poll
 * which run before the Phase 3.3 socket section below). */
#define LNX_AF_INET     2
#define LNX_AF_UNIX     1
#define LNX_SOCK_STREAM 1
#define LNX_SOCK_DGRAM  2
#define LNX_SOCK_SEQPACKET 5
#define LNX_MSG_PEEK    2
#define LNX_MSG_DONTWAIT 0x40
#define LNX_SOL_SOCKET  1
#define LNX_SO_PEERCRED 17
#define LNX_SCM_RIGHTS  1

struct lnx_sockaddr_in {
    uint16_t family;
    uint16_t port;    /* network byte order */
    uint32_t addr;    /* network byte order */
    uint8_t  zero[8];
};

enum sock_kind { SOCK_NONE = 0, SOCK_TCP, SOCK_UDP, SOCK_UNIX };

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
#define LNX_listen         50
#define LNX_socketpair     53
#define LNX_accept4       288

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
 * runs (serial log) see program output. fd routing is kind-based since
 * P1.2: sockets/ttys/files are all struct file entries. */

static uint64_t sc_read(uint64_t fd, uint64_t buf, uint64_t count,
                        uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) return -L_EINVAL;

    struct file* f = fd_get_file((int)fd);
    if (f == NULL) return -L_EBADF;

    /* Socket fds: musl recv()/busybox code paths often fall back to plain
     * read(2) on the socket (e.g. busybox nslookup reads the DNS reply
     * with read(fd) after poll()). */
    if (f->kind == FILE_T_SOCK_TCP || f->kind == FILE_T_SOCK_UDP) {
        enum sock_kind kind;
        void* s = sock_fd_get(fd, &kind);
        if (s == NULL) return -L_EBADF;
        if (kind == SOCK_TCP) {
            tcp_socket_t* ts = (tcp_socket_t*)s;
            uint16_t want = (uint16_t)(count > 0xFFFF ? 0xFFFF : count);
            for (;;) {
                int r = tcp_recv(ts, (uint8_t*)buf, want, 0);
                if (r >= 0) return (uint64_t)r;
                if (ts->state == TCP_CLOSED &&
                    ts->rx_head == ts->rx_tail) return (uint64_t)-L_EIO;
                /* no data yet: park; the wake resumes user mode so this
                 * read(2) re-executes and re-checks (P1.1) */
                wait_block(lnx_frame_rsp, &g_net_wq, WAIT_RESTART, 0, 0);
            }
        }
        udp_socket_t* us = (udp_socket_t*)s;
        uint8_t kbuf[2048];
        uint32_t src_ip = 0;
        uint16_t src_port = 0;
        uint16_t want = (uint16_t)(count > sizeof(kbuf) ? sizeof(kbuf) : count);
        for (;;) {
            int r = udp_recvfrom(us, kbuf, want, &src_ip, &src_port, 0);
            if (r >= 0) {
                memcpy((void*)buf, kbuf, (size_t)r);
                return (uint64_t)r;
            }
            wait_block(lnx_frame_rsp, &g_net_wq, WAIT_RESTART, 0, 0);
        }
    }

    /* AF_UNIX: X11/D-Bus clients read with plain read(2) as well. */
    if (f->kind == FILE_T_SOCK_UNIX) {
        unix_sock_t* us = (unix_sock_t*)f->priv;
        for (;;) {
            int r = unix_recv(us, (void*)buf, (int)(count > 0x7FFFFFFF ? 0x7FFFFFFF : count));
            if (r >= 0) return (uint64_t)r;
            wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
        }
    }

    if (f->kind == FILE_T_VFS)
        return (uint64_t)lnxvfs_read((int)fd, (char*)buf, (size_t)count);
    return (uint64_t)f->ops->read(f, (char*)buf, (size_t)count);
}

static uint64_t sc_write(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t u3, uint64_t u4, uint64_t u5) {
    (void)u3; (void)u4; (void)u5;
    if (count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) return -L_EINVAL;

    struct file* f = fd_get_file((int)fd);
    if (f == NULL) return -L_EBADF;

    /* write(2) on a connected socket (busybox nslookup sends DNS queries
     * with plain write after connect()). */
    if (f->kind == FILE_T_SOCK_TCP || f->kind == FILE_T_SOCK_UDP) {
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
        /* Clamp before truncating, exactly like the TCP branch above:
         * a bare (uint16_t)count wraps for count > 0xFFFF (0x10000 -> 0),
         * sending the wrong length and reporting a bogus byte count. */
        uint16_t len = (uint16_t)(count > 0xFFFF ? 0xFFFF : count);
        int r = udp_sendto(us, us->remote_ip, us->remote_port,
                           (const uint8_t*)buf, len);
        return r < 0 ? (uint64_t)-L_EIO : (uint64_t)len;
    }

    /* AF_UNIX stream write */
    if (f->kind == FILE_T_SOCK_UNIX) {
        unix_sock_t* us = (unix_sock_t*)f->priv;
        for (;;) {
            int r = unix_send(us, (const void*)buf, (int)(count > 0x7FFFFFFF ? 0x7FFFFFFF : count));
            if (r >= 0) return (uint64_t)r;
            if (r != -L_EAGAIN) return (uint64_t)r;     /* EPIPE / ENOTCONN */
            wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
        }
    }

    if (f->kind == FILE_T_VFS)
        return (uint64_t)lnxvfs_write((int)fd, (const char*)buf, (size_t)count);
    return (uint64_t)f->ops->write(f, (const char*)buf, (size_t)count);
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
     * vectors must be coalesced and handed to the TCP/UDP layer. */
    struct file* uf = fd_get_file((int)fd);
    if (uf == NULL) return -L_EBADF;
    if (uf->kind == FILE_T_SOCK_TCP || uf->kind == FILE_T_SOCK_UDP) {
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
        /* Clamp before truncating (see sc_write): total is already bounded
         * by MSG_KBUF_SZ, but keep the guard explicit and consistent. */
        uint16_t len = (uint16_t)(total > 0xFFFF ? 0xFFFF : total);
        int r = udp_sendto(us, us->remote_ip, us->remote_port, kbuf, len);
        return r < 0 ? (uint64_t)-L_EIO : (uint64_t)len;
    }
    /* AF_UNIX writev: coalesce and push (X11 requests are small writevs) */
    if (uf->kind == FILE_T_SOCK_UNIX) {
        unix_sock_t* us = (unix_sock_t*)uf->priv;
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
        for (;;) {
            int r = unix_send(us, kbuf, (int)total);
            if (r >= 0) return (uint64_t)total;
            if (r != -L_EAGAIN) return (uint64_t)r;
            wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
        }
    }
    /* Single redirected file fd: delegate (must not split the vector) */
    if (uf->kind == FILE_T_VFS) {
        const struct lnx_iovec* v0 = (const struct lnx_iovec*)iov;
        if (iovcnt == 1) {
            if (v0[0].len == 0) return 0;
            if (!process_check_user_range(v0[0].base, (size_t)v0[0].len)) return -L_EINVAL;
            return (uint64_t)lnxvfs_write((int)fd, (const char*)v0[0].base, (size_t)v0[0].len);
        }
        return -L_EINVAL;
    }
    uint64_t total = 0;
    const struct lnx_iovec* v = (const struct lnx_iovec*)iov;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (v[i].len == 0) continue;
        if (!process_check_user_range(v[i].base, (size_t)v[i].len)) {
            return -L_EINVAL;
        }
        total += (uint64_t)uf->ops->write(uf, (const char*)v[i].base, (size_t)v[i].len);
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

    struct file* uf = fd_get_file((int)fd);
    if (uf == NULL) return -L_EBADF;
    if (uf->kind == FILE_T_SOCK_TCP || uf->kind == FILE_T_SOCK_UDP) {
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

    /* AF_UNIX readv: fill the first non-empty vector (stream semantics) */
    if (uf->kind == FILE_T_SOCK_UNIX) {
        unix_sock_t* us = (unix_sock_t*)uf->priv;
        for (uint64_t i = 0; i < iovcnt; i++) {
            if (iv[i].len == 0) continue;
            if (!process_check_user_range(iv[i].base, (size_t)iv[i].len))
                return -L_EINVAL;
            for (;;) {
                int r = unix_recv(us, (void*)iv[i].base, (int)iv[i].len);
                if (r >= 0) return (uint64_t)r;
                wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
            }
        }
        return 0;
    }

    uint64_t total = 0;
    if (uf->kind == FILE_T_VFS) {
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
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iv[i].len == 0) continue;
        if (!process_check_user_range(iv[i].base, (size_t)iv[i].len))
            return -L_EINVAL;
        long r = uf->ops->read(uf, (char*)iv[i].base, (size_t)iv[i].len);
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
    /* Unified layer: files, sockets and console ttys all close here
     * (ttys are real refcounted objects now, mirroring close(0..2)). */
    return (uint64_t)fd_close((int)fd);
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
    /* pathname AF_UNIX socket nodes live in the kernel bind table (P1.4) */
    if (unix_unlink_path(path) == 0) return 0;
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

/* nanosleep(2): park the caller; a one-shot timer completes the parked
 * frame with 0 at the deadline (P1.1 blocking syscalls). */
static uint64_t sc_nanosleep(uint64_t req, uint64_t u2, uint64_t u3,
                             uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    if (!process_check_user_range(req, 16)) return -L_EINVAL;
    const uint64_t* t = (const uint64_t*)req;
    uint64_t us = t[0] * 1000000ULL + t[1] / 1000ULL;
    if (us != 0) {
        wait_block(lnx_frame_rsp, &g_sleep_wq, WAIT_COMPLETE, 0, us);
        /* only reachable via the timer-pool spin fallback: re-check */
        uint64_t start = pit_uptime_us();
        while (pit_uptime_us() - start < us) { __asm__ volatile("pause"); }
    }
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

/* clone(2) (P1.3): Linux x86-64 ABI is
 *   clone(flags, stack, parent_tid, child_tid, tls)
 * i.e. tls rides R8 and child_tid rides R10. musl pthread_create and
 * posix_spawn need the shared-groups subset. */
static uint64_t sc_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid,
                         uint64_t child_tid, uint64_t tls,
                         uint64_t u6) {
    (void)u6;
    return (uint64_t)process_clone(lnx_frame_rsp, flags, stack,
                                   parent_tid, child_tid, tls);
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
    (void)u4; (void)u5; (void)u6;
    char kpath[MAX_PATH_LENGTH];
    if (fetch_user_path(upath, kpath, sizeof(kpath)) < 0) return -L_EINVAL;

    /* Snapshot argv AND envp into kernel memory: the user image is torn
     * down by exec before the new one is mapped. */
    const int MAX_ARGS = 32;
    char (*bufs)[96]  = kmalloc((size_t)MAX_ARGS * 96);
    char (*ebufs)[96] = kmalloc((size_t)MAX_ARGS * 96);
    char** ptrs  = kmalloc(((size_t)MAX_ARGS + 1) * sizeof(char*));
    char** eptrs = kmalloc(((size_t)MAX_ARGS + 1) * sizeof(char*));
    if (bufs == NULL || ebufs == NULL || ptrs == NULL || eptrs == NULL) {
        if (bufs) kfree(bufs);
        if (ebufs) kfree(ebufs);
        if (ptrs) kfree(ptrs);
        if (eptrs) kfree(eptrs);
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

    int envc = 0;
    if (uenvp != 0) {
        for (; envc < MAX_ARGS; envc++) {
            if (!process_check_user_range(uenvp + (uint64_t)envc * 8, 8)) break;
            uint64_t p;
            memcpy(&p, (const void*)(uenvp + (uint64_t)envc * 8), sizeof(p));
            if (p == 0) break;
            if (fetch_user_path(p, ebufs[envc], 96) < 0) break;
            eptrs[envc] = ebufs[envc];
        }
    }
    eptrs[envc] = NULL;

    int rc = exec_replace(lnx_frame_rsp, kpath, ptrs, argc, eptrs, envc);
    kfree(bufs);
    kfree(ebufs);
    kfree(ptrs);
    kfree(eptrs);
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
    return (uint64_t)fd_dup((int)oldfd);
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
        struct file* uf = fd_get_file((int)fd);
        if (uf == NULL || uf->kind != FILE_T_VFS) {
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
    if (proc == NULL || proc->mm == NULL) return -L_ENOMEM;
    mm_t* mm = proc->mm;
    if (mm->brk_start == 0) return -L_ENOMEM;   /* not an ELF process */

    if (addr == 0 || addr < mm->brk_start || addr > UVM_BRK_LIMIT) {
        return mm->brk_cur;
    }

    if (addr > mm->brk_cur) {
        if (uvm_map_range(proc, mm->brk_cur, addr, UVM_PROT_READ | UVM_PROT_WRITE) != 0) {
            return mm->brk_cur;
        }
    } else if (addr < mm->brk_cur) {
        uvm_unmap_range(proc, addr, mm->brk_cur);
    }
    mm->brk_cur = addr;
    return mm->brk_cur;
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
    return proc ? proc->tid : 0;
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

/* --- futex (P1.3 rewrite) --------------------------------------------------
 * Hashed waiter registry; each waiter parks on its OWN private wait queue
 * so a timeout timer can only ever wake its owner (no cross-waiter
 * pollution when WAKE and a deadline race). Wake flavor is always
 * WAIT_COMPLETE: futex returns 0 when woken, -ETIMEDOUT on deadline. */
#define LNX_FUTEX_PRIVATE 128
#define LNX_FUTEX_WAIT    0
#define LNX_FUTEX_WAKE    1
#define L_EFAULT  14
#define L_ETIMEDOUT 110

typedef struct futex_waiter {
    uint64_t uaddr;
    process_t* proc;
    wait_queue_t wq;             /* private; holds exactly proc */
    struct futex_waiter* bnext;
} futex_waiter_t;

#define FUT_HASH 16
static futex_waiter_t* futex_tab[FUT_HASH];

static inline uint64_t futex_hash(uint64_t uaddr) {
    return ((uaddr >> 2) ^ (uaddr >> 17)) & (FUT_HASH - 1);
}

/* WAIT: returns 0, -EAGAIN, -EINVAL or -ETIMEDOUT (via parked frame). */
static uint64_t futex_wait(uint64_t uaddr, uint32_t val, uint64_t timeout_us) {
    if (!process_check_user_range(uaddr, 4)) return -L_EINVAL;
    if (*(volatile uint32_t*)uaddr != val) return -L_EAGAIN;

    process_t* p = process_get_current();
    futex_waiter_t* w = (futex_waiter_t*)kmalloc(sizeof(futex_waiter_t));
    if (p == NULL || w == NULL) return -L_ENOMEM;
    w->uaddr = uaddr;
    w->proc = p;
    w->wq.head = w->wq.tail = NULL;
    w->bnext = NULL;

    uint64_t h = futex_hash(uaddr);

    __asm__ volatile("cli");

    if (timeout_us != 0 &&
        wait_timer_arm(&w->wq, timeout_us, WAIT_COMPLETE,
                       (uint64_t)(int64_t)-L_ETIMEDOUT) != 0) {
        /* Timer pool exhausted: bounded spin, report timeout */
        __asm__ volatile("sti");
        kfree(w);
        uint64_t deadline = pit_uptime_us() + timeout_us;
        while (pit_uptime_us() < deadline) __asm__ volatile("pause");
        return (uint64_t)(int64_t)-L_ETIMEDOUT;
    }

    process_park_entry_frame(p, lnx_frame_rsp, 0, 0);  /* wake => RAX 0 */
    w->bnext = futex_tab[h];
    futex_tab[h] = w;
    p->wait_next = NULL;
    w->wq.head = w->wq.tail = p;
    p->state = PROCESS_STATE_BLOCKED;
    fpu_save(p);

    wait_park_now(p);                    /* never returns */
}

/* WAKE up to n waiters on uaddr; returns the number woken. Also reaps
 * stale (timed-out) entries for this address. Exported: used by the
 * thread-exit path to signal CLONE_CHILD_CLEARTID joiners. */
int futex_wake_user(uint64_t uaddr, int n) {
    uint64_t h = futex_hash(uaddr);
    futex_waiter_t** pp = &futex_tab[h];
    int woken = 0;
    while (*pp != NULL) {
        futex_waiter_t* w = *pp;
        if (w->uaddr != uaddr) {
            pp = &w->bnext;
            continue;
        }
        if (w->proc->state == PROCESS_STATE_BLOCKED) {
            if (n >= 0 && woken >= n) {
                pp = &w->bnext;
                continue;
            }
            wait_wake_status(&w->wq, 0);
            *pp = w->bnext;
            kfree(w);
            woken++;
        } else {
            /* Timed out or already gone: lazy reaper */
            *pp = w->bnext;
            kfree(w);
        }
    }
    return woken;
}

static uint64_t sc_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                         uint64_t utime, uint64_t u5, uint64_t u6) {
    (void)u5; (void)u6;
    switch (op & 0x7F) {
    case LNX_FUTEX_WAIT: {
        uint64_t timeout_us = 0;
        if (utime != 0) {
            /* user-space struct timespec { i64 sec; i64 nsec } */
            if (!process_check_user_range(utime, 16)) return -L_EFAULT;
            int64_t sec, nsec;
            memcpy(&sec,  (const void*)utime, 8);
            memcpy(&nsec, (const void*)(utime + 8), 8);
            if (sec < 0) return (uint64_t)(int64_t)-L_EINVAL;
            timeout_us = (uint64_t)sec * 1000000ULL +
                         (uint64_t)nsec / 1000ULL;
            if (sec != 0 && timeout_us == 0) timeout_us = 1;
        }
        return futex_wait(uaddr, (uint32_t)val, timeout_us);
    }
    case LNX_FUTEX_WAKE:
        if (!process_check_user_range(uaddr, 4)) return -L_EFAULT;
        return (uint64_t)futex_wake_user(uaddr, (int)val);
    default:
        /* FD, REQUEUE, BITSET, ... : nothing depends on them yet */
        return 0;
    }
}

/* pread64(2): glibc's loader reads program headers with it. */
static uint64_t sc_pread64(uint64_t fd, uint64_t ubuf, uint64_t count,
                           uint64_t off, uint64_t u5, uint64_t u6) {
    (void)u5; (void)u6;
    struct file* uf = fd_get_file((int)fd);
    if (uf == NULL || uf->kind != FILE_T_VFS) return -L_EBADF;
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
 * Sockets are struct file entries (FILE_T_SOCK_TCP/UDP) in the unified
 * per-process fd table (P1.2) - no private fd range anymore. sock_fd_get
 * resolves any fd to its backend object so the syscall bodies below stay
 * unchanged. */

static void* sock_fd_get(uint64_t fd, enum sock_kind* kind) {
    struct file* f = fd_get_file((int)fd);
    if (f == NULL) return NULL;
    if (f->kind == FILE_T_SOCK_TCP) {
        if (kind) *kind = SOCK_TCP;
        return f->priv;
    }
    if (f->kind == FILE_T_SOCK_UDP) {
        if (kind) *kind = SOCK_UDP;
        return f->priv;
    }
    if (f->kind == FILE_T_SOCK_UNIX) {
        if (kind) *kind = SOCK_UNIX;
        return f->priv;
    }
    return NULL;
}

/* Backend close/poll ops for socket file objects. */
static int sock_tcp_f_close(struct file* f) {
    tcp_close((tcp_socket_t*)f->priv);
    return 0;
}

static int sock_udp_f_close(struct file* f) {
    udp_socket_close((udp_socket_t*)f->priv);
    return 0;
}

static int sock_tcp_f_poll(struct file* f) {
    tcp_socket_t* s = (tcp_socket_t*)f->priv;
    int rev = 0;
    if (s->rx_head != s->rx_tail) rev |= POLL_IN;
    if (s->state == TCP_ESTABLISHED) rev |= POLL_OUT;
    if (s->state == TCP_CLOSED) rev |= POLL_ERR;
    return rev;
}

static int sock_udp_f_poll(struct file* f) {
    udp_socket_t* s = (udp_socket_t*)f->priv;
    int rev = POLL_OUT;
    if (s->rx_count != 0) rev |= POLL_IN;
    return rev;
}

static const struct file_ops sock_tcp_ops = {
    .close = sock_tcp_f_close,
    .poll = sock_tcp_f_poll,
};

static const struct file_ops sock_udp_ops = {
    .close = sock_udp_f_close,
    .poll = sock_udp_f_poll,
};

/* --- P1.4: AF_UNIX ------------------------------------------------------- */

static int sock_unix_f_close(struct file* f) {
    unix_sock_close((unix_sock_t*)f->priv);
    return 0;
}

static int sock_unix_f_poll(struct file* f) {
    return unix_poll_bits((unix_sock_t*)f->priv);
}

static const struct file_ops sock_unix_ops = {
    .close = sock_unix_f_close,
    .poll = sock_unix_f_poll,
};

/* Parse a user sockaddr_un. Returns 0 on success with the raw address in
 * kpath and *abstract set; negative errno otherwise. Abstract addresses
 * (leading NUL) are the next (addrlen-2) bytes, NULs included. */
static int unix_fetch_addr(uint64_t uaddr, uint64_t addrlen,
                           char* kpath, int* abstract) {
    *abstract = 0;
    if (addrlen < 3 || addrlen > sizeof(struct lnx_sockaddr_un)) return -L_EINVAL;
    if (!process_check_user_range(uaddr, addrlen)) return -L_EINVAL;
    const struct lnx_sockaddr_un* a = (const struct lnx_sockaddr_un*)uaddr;
    if (a->family != LNX_AF_UNIX) return -L_EINVAL;
    uint64_t plen = addrlen - 2;
    if (a->path[0] == '\0') {
        if (plen == 0 || plen > UNIX_PATH_MAX) return -L_EINVAL;
        memcpy(kpath, a->path, plen);
        *abstract = 1;
        return 0;
    }
    /* pathname: NUL-terminated within the provided length */
    if (plen >= UNIX_PATH_MAX) plen = UNIX_PATH_MAX - 1;
    uint64_t n = 0;
    while (n < plen && a->path[n] != '\0') n++;
    if (n == 0 || n >= plen + 1 || a->path[n] != '\0') return -L_EINVAL;
    memcpy(kpath, a->path, n);
    kpath[n] = '\0';
    return 0;
}

/* sockaddr_un output helper: fills the user address and updates *ulen. */
static int unix_put_addr(unix_sock_t* s, uint64_t uaddr, uint64_t ulen_p) {
    if (uaddr == 0 || ulen_p == 0) return 0;
    if (!process_check_user_range(uaddr, sizeof(struct lnx_sockaddr_un)) ||
        !process_check_user_range(ulen_p, 4))
        return -L_EINVAL;
    char pth[UNIX_PATH_MAX];
    int n = unix_getsockname(s, pth);
    struct lnx_sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.family = LNX_AF_UNIX;
    int len = 2;
    if (n != 0) {
        int alen = n < 0 ? -n : n;
        memcpy(sa.path, pth, (size_t)alen);
        len = 2 + alen;                     /* abstract keeps raw bytes */
    }
    memcpy((void*)uaddr, &sa, sizeof(sa));
    *(uint32_t*)ulen_p = (uint32_t)len;
    return 0;
}

static uint64_t sc_listen(uint64_t fd, uint64_t backlog, uint64_t u3,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u3; (void)u4; (void)u5; (void)u6;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_ENOTSOCK;
    if (kind != SOCK_UNIX) return 0;        /* INET listeners: pretend ok */
    return (uint64_t)unix_listen((unix_sock_t*)s, (int)backlog);
}

static uint64_t sc_accept4(uint64_t fd, uint64_t uaddr, uint64_t ulen_p,
                           uint64_t flags, uint64_t u5, uint64_t u6) {
    (void)flags;   /* SOCK_CLOEXEC/NONBLOCK: sockets stay blocking */
    (void)u5; (void)u6;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_ENOTSOCK;
    if (kind != SOCK_UNIX) return -L_EOPNOTSUPP;

    for (;;) {
        unix_sock_t* c = unix_accept((unix_sock_t*)s);
        if (c != NULL) {
            struct file* f = file_new(FILE_T_SOCK_UNIX, &sock_unix_ops, c);
            if (f == NULL) { unix_sock_close(c); return -L_ENOMEM; }
            int nfd = fd_install(f);
            if (nfd < 0) { file_close(f); return -L_ENOMEM; }
            if (unix_put_addr(c, uaddr, ulen_p) != 0) return -L_EINVAL;
            return (uint64_t)nfd;
        }
        if (unix_sock_state((unix_sock_t*)s) != US_LISTENING) return -L_EINVAL;
        wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
    }
}

static uint64_t sc_socketpair(uint64_t domain, uint64_t type, uint64_t proto,
                              uint64_t sv, uint64_t u5, uint64_t u6) {
    (void)u5; (void)u6;
    if (domain != LNX_AF_UNIX) return -L_EOPNOTSUPP;
    uint64_t t = type & 0xFF;               /* strip SOCK_CLOEXEC/NONBLOCK */
    if (t != LNX_SOCK_STREAM && t != LNX_SOCK_DGRAM && t != LNX_SOCK_SEQPACKET)
        return -L_EINVAL;
    (void)proto;
    if (!process_check_user_range(sv, 2 * sizeof(int))) return -L_EINVAL;

    unix_sock_t* a = unix_socket_create((int)t);
    if (a == NULL) return -L_ENOMEM;
    unix_sock_t* b = unix_socket_create((int)t);
    if (b == NULL) { unix_sock_close(a); return -L_ENOMEM; }
    if (unix_socketpair(a, b) != 0) {
        unix_sock_close(a); unix_sock_close(b);
        return -L_EINVAL;
    }
    struct file* fa = file_new(FILE_T_SOCK_UNIX, &sock_unix_ops, a);
    if (fa == NULL) { unix_sock_close(a); unix_sock_close(b); return -L_ENOMEM; }
    struct file* fb = file_new(FILE_T_SOCK_UNIX, &sock_unix_ops, b);
    if (fb == NULL) { file_close(fa); unix_sock_close(b); return -L_ENOMEM; }
    int fda = fd_install(fa);
    if (fda < 0) { file_close(fa); file_close(fb); return -L_ENOMEM; }
    int fdb = fd_install(fb);
    if (fdb < 0) { fd_close(fda); file_close(fb); return -L_ENOMEM; }
    int* usv = (int*)sv;
    usv[0] = fda;
    usv[1] = fdb;
    return 0;
}

static uint64_t sc_socket(uint64_t domain, uint64_t type, uint64_t proto,
                          uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    /* musl passes SOCK_CLOEXEC(0x80000)/SOCK_NONBLOCK(0x800) inside type */
    uint64_t t = type & 0xFF;
    if (domain == LNX_AF_UNIX) {
        if (t != LNX_SOCK_STREAM && t != LNX_SOCK_DGRAM && t != LNX_SOCK_SEQPACKET)
            return -L_EINVAL;
        unix_sock_t* s = unix_socket_create((int)t);
        if (s == NULL) return -L_ENOMEM;
        struct file* f = file_new(FILE_T_SOCK_UNIX, &sock_unix_ops, s);
        if (f == NULL) { unix_sock_close(s); return -L_ENOMEM; }
        int fd = fd_install(f);
        if (fd < 0) { file_close(f); return -L_ENOMEM; }
        return (uint64_t)fd;
    }
    if (domain != LNX_AF_INET) return -L_EINVAL;
    if (proto != 0 && !(domain == LNX_AF_INET && t == LNX_SOCK_STREAM && proto == 6) &&
        !(domain == LNX_AF_INET && t == LNX_SOCK_DGRAM && proto == 17)) {
        return -L_EINVAL;
    }
    if (t == LNX_SOCK_STREAM) {
        tcp_socket_t* s = tcp_socket_create();
        if (s == NULL) return -L_ENOMEM;
        struct file* f = file_new(FILE_T_SOCK_TCP, &sock_tcp_ops, s);
        if (f == NULL) { tcp_socket_close(s); return -L_ENOMEM; }
        int fd = fd_install(f);
        if (fd < 0) { file_close(f); return -L_ENOMEM; }
        return (uint64_t)fd;
    }
    if (t == LNX_SOCK_DGRAM) {
        udp_socket_t* s = udp_socket_create();
        if (s == NULL) return -L_ENOMEM;
        struct file* f = file_new(FILE_T_SOCK_UDP, &sock_udp_ops, s);
        if (f == NULL) { udp_socket_close(s); return -L_ENOMEM; }
        int fd = fd_install(f);
        if (fd < 0) { file_close(f); return -L_ENOMEM; }
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
    if (addrlen < 2 || !process_check_user_range(uaddr, 2)) return -L_EINVAL;
    if (*(const uint16_t*)uaddr == LNX_AF_UNIX) {
        if (kind != SOCK_UNIX) return -L_EINVAL;
        char kpath[UNIX_PATH_MAX];
        int abstract = 0;
        if (unix_fetch_addr(uaddr, addrlen, kpath, &abstract) != 0) return -L_EINVAL;
        int n = abstract ? -(int)(addrlen - 2) : (int)strlen(kpath);
        for (;;) {
            int r = unix_connect((unix_sock_t*)s, kpath, n);
            if (r == 0) return 0;
            if (r != -L_EAGAIN) return (uint64_t)r;
            wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
        }
    }
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
    if (addrlen < 2 || !process_check_user_range(uaddr, 2)) return -L_EINVAL;
    if (*(const uint16_t*)uaddr == LNX_AF_UNIX) {
        if (kind != SOCK_UNIX) return -L_EINVAL;
        char kpath[UNIX_PATH_MAX];
        int abstract = 0;
        if (unix_fetch_addr(uaddr, addrlen, kpath, &abstract) != 0) return -L_EINVAL;
        int n = abstract ? -(int)(addrlen - 2) : (int)strlen(kpath);
        return (uint64_t)unix_bind((unix_sock_t*)s, kpath, n);
    }
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

    /* AF_UNIX */
    if (kind == SOCK_UNIX) {
        unix_sock_t* us = (unix_sock_t*)s;
        char kpath[UNIX_PATH_MAX];
        int abstract = 0;
        int have_dst = 0;
        if (uaddr != 0) {
            if (addrlen < 2 || !process_check_user_range(uaddr, 2)) return -L_EINVAL;
            if (*(const uint16_t*)uaddr != LNX_AF_UNIX) return -L_EINVAL;
            if (unix_fetch_addr(uaddr, addrlen, kpath, &abstract) != 0) return -L_EINVAL;
            have_dst = 1;
        }
        int n = abstract ? -(int)(addrlen - 2) : (int)strlen(kpath);
        for (;;) {
            int r;
            if (unix_sock_type(us) == 1) {
                if (have_dst) return -L_EINVAL;
                r = unix_send(us, (const void*)buf, (int)len);
            } else {
                r = unix_sendto_dgram(us, (const void*)buf, (int)len,
                                      have_dst ? kpath : NULL, have_dst ? n : 0);
            }
            if (r >= 0) return (uint64_t)r;
            if (r != -L_EAGAIN) return (uint64_t)r;
            wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
        }
    }

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

    /* AF_UNIX: peer address from the socket (stream) or zeroed (dgram) */
    if (kind == SOCK_UNIX) {
        unix_sock_t* us = (unix_sock_t*)s;
        for (;;) {
            int r = (unix_sock_type(us) == 1)
                        ? unix_recv(us, (void*)buf, (int)len)
                        : unix_recvmsg(us, &(struct unix_iovec){ (void*)buf, (size_t)len },
                                       1, NULL, &(int){0});
            if (r >= 0) {
                if (uaddr != 0 && unix_sock_type(us) == 1)
                    unix_put_addr(us, uaddr, uaddrlen);
                return (uint64_t)r;
            }
            if (r != -L_EAGAIN) return (uint64_t)r;
            wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
        }
    }

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

/* cmsghdr layout (x86-64): size_t len; int level; int type; payload */
struct lnx_cmsghdr { uint64_t len; int32_t level; int32_t type; };
#define LNX_CMSG_HDRSZ 16

static uint64_t sc_sendmsg(uint64_t fd, uint64_t umsg, uint64_t flags,
                           uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)flags; (void)u4; (void)u5; (void)u6;
    enum sock_kind kind;
    void* s = sock_fd_get(fd, &kind);
    if (s == NULL) return -L_ENOTSOCK;

    /* AF_UNIX: iovecs straight from user memory + SCM_RIGHTS cmsg */
    if (kind == SOCK_UNIX) {
        if (!process_check_user_range(umsg, sizeof(struct lnx_msghdr))) return -L_EINVAL;
        const struct lnx_msghdr* mh = (const struct lnx_msghdr*)umsg;
        if (mh->msg_iovlen == 0 || mh->msg_iovlen > 64) return -L_EMSGSIZE;
        if (!process_check_user_range((uint64_t)mh->msg_iov,
                                      mh->msg_iovlen * sizeof(struct lnx_iovec)))
            return -L_EINVAL;
        struct unix_iovec kiv[64];
        for (uint64_t i = 0; i < mh->msg_iovlen; i++) {
            kiv[i].base = (void*)(uintptr_t)mh->msg_iov[i].base;
            kiv[i].len  = (size_t)mh->msg_iov[i].len;
            if (kiv[i].len != 0 &&
                !process_check_user_range((uint64_t)kiv[i].base, kiv[i].len))
                return -L_EINVAL;
        }
        /* SCM_RIGHTS: gather the referenced files (ref for the transfer) */
        struct file* fds[UNIX_SCM_MAX];
        int nfds = 0;
        if (mh->msg_control != 0 && mh->msg_controllen >= LNX_CMSG_HDRSZ) {
            if (!process_check_user_range((uint64_t)mh->msg_control, mh->msg_controllen))
                return -L_EINVAL;
            const uint8_t* cp = (const uint8_t*)mh->msg_control;
            const uint8_t* cend = cp + mh->msg_controllen;
            while (cp + LNX_CMSG_HDRSZ <= cend && nfds < UNIX_SCM_MAX) {
                const struct lnx_cmsghdr* ch = (const struct lnx_cmsghdr*)cp;
                if (ch->len < LNX_CMSG_HDRSZ || cp + ch->len > cend) break;
                if (ch->level == LNX_SOL_SOCKET && ch->type == LNX_SCM_RIGHTS) {
                    const int* uf = (const int*)(cp + LNX_CMSG_HDRSZ);
                    int cnt = (int)((ch->len - LNX_CMSG_HDRSZ) / 4);
                    for (int i = 0; i < cnt && nfds < UNIX_SCM_MAX; i++) {
                        struct file* sf = fd_get_file(uf[i]);
                        if (sf == NULL) {
                            for (int j = 0; j < nfds; j++) file_close(fds[j]);
                            return -L_EBADF;
                        }
                        fds[nfds++] = file_ref(sf);
                    }
                }
                cp += ch->len;
            }
        }
        int r = unix_sendmsg((unix_sock_t*)s, kiv, (int)mh->msg_iovlen, fds, nfds);
        if (r == -L_EAGAIN) {
            /* park; WAIT_RESTART re-executes the whole syscall (refs are
             * dropped first so they are re-taken on the retry) */
            for (int i = 0; i < nfds; i++) file_close(fds[i]);
            wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
        }
        if (r < 0) {
            for (int i = 0; i < nfds; i++) file_close(fds[i]);   /* undo refs */
            return (uint64_t)r;
        }
        return (uint64_t)r;
    }

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
        memcpy(kbuf + total, (const void*)(uintptr_t)iv[i].base, n);
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

    /* AF_UNIX: scatter read + SCM_RIGHTS delivery */
    if (kind == SOCK_UNIX) {
        if (!process_check_user_range(umsg, sizeof(struct lnx_msghdr))) return -L_EINVAL;
        struct lnx_msghdr* mh = (struct lnx_msghdr*)umsg;
        if (mh->msg_iovlen == 0 || mh->msg_iovlen > 64) return -L_EMSGSIZE;
        if (!process_check_user_range((uint64_t)mh->msg_iov,
                                      mh->msg_iovlen * sizeof(struct lnx_iovec)))
            return -L_EINVAL;
        struct unix_iovec kiv[64];
        for (uint64_t i = 0; i < mh->msg_iovlen; i++) {
            kiv[i].base = (void*)(uintptr_t)mh->msg_iov[i].base;
            kiv[i].len  = (size_t)mh->msg_iov[i].len;
            if (kiv[i].len != 0 &&
                !process_check_user_range((uint64_t)kiv[i].base, kiv[i].len))
                return -L_EINVAL;
        }
        struct file* ofds[UNIX_SCM_MAX];
        int nfds = 0;
        int r = unix_recvmsg((unix_sock_t*)s, kiv, (int)mh->msg_iovlen, ofds, &nfds);
        if (r == -L_EAGAIN) {
            if (mh->msg_flags & LNX_MSG_DONTWAIT) return (uint64_t)-L_EAGAIN;
            wait_block(lnx_frame_rsp, &g_unix_wq, WAIT_RESTART, 0, 0);
        }
        if (r < 0) return (uint64_t)r;

        mh->msg_flags = 0;
        /* install received fds and write the cmsg if the caller has room */
        int ifd[UNIX_SCM_MAX];
        int installed = 0;
        for (int i = 0; i < nfds; i++) {
            ifd[i] = fd_install(ofds[i]);
            if (ifd[i] < 0) break;
            installed++;
        }
        uint64_t need = LNX_CMSG_HDRSZ + (uint64_t)installed * 4;
        int written = 0;
        if (installed > 0 && mh->msg_control != 0 && mh->msg_controllen >= need &&
            process_check_user_range((uint64_t)mh->msg_control, need)) {
            struct lnx_cmsghdr* ch = (struct lnx_cmsghdr*)mh->msg_control;
            ch->len = need;
            ch->level = LNX_SOL_SOCKET;
            ch->type = LNX_SCM_RIGHTS;
            int* out = (int*)(mh->msg_control + LNX_CMSG_HDRSZ);
            for (int i = 0; i < installed; i++) out[i] = ifd[i];
            mh->msg_controllen = need;
            written = 1;
            installed = 0;      /* consumed */
        }
        for (int i = 0; i < installed; i++) fd_close(ifd[i]);  /* no room */
        if (mh->msg_control != 0 && !written) mh->msg_controllen = 0;
        return (uint64_t)r;
    }

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
            memcpy((void*)(uintptr_t)iv[i].base, kbuf + copied, n);
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

static uint64_t poll_scan(struct lnx_pollfd* fds, uint64_t nfds) {
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        int16_t rev = 0;
        struct file* f = fd_get_file((int)fds[i].fd);
        if (f == NULL || f->ops == NULL || f->ops->poll == NULL) {
            rev = LNX_POLLNVAL;
        } else {
            /* Kind-uniform readiness via the file layer: sockets probe
             * their ring/flags, ttys and memory files are always ready. */
            rev = (int16_t)(f->ops->poll(f) & 0xFFFF);
        }
        fds[i].revents = (int16_t)(rev & (fds[i].events | LNX_POLLERR | LNX_POLLNVAL));
        if (fds[i].revents) ready++;
    }
    return (uint64_t)ready;
}

/* poll(2): supports sockets + files/console. pollfd layout matches Linux
 * x86-64: { int fd; short events; short revents; }. Blocking uses the
 * P1.1 wait queue: park, wake re-executes the syscall, and the deadline
 * survives restarts in proc->block_scratch. */
static uint64_t sc_poll(uint64_t ufds, uint64_t nfds, uint64_t timeout_ms,
                        uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    if (nfds == 0) return 0;
    if (nfds > 256) return -L_EINVAL;
    if (!process_check_user_range(ufds, nfds * sizeof(struct lnx_pollfd))) return -L_EINVAL;
    struct lnx_pollfd* fds = (struct lnx_pollfd*)ufds;

    for (;;) {
        uint64_t ready = poll_scan(fds, nfds);
        if (ready != 0) {
            process_get_current()->block_scratch = 0;
            return ready;
        }
        if (timeout_ms == 0) return 0;

        /* Deadline persists across restarts (block_scratch) */
        process_t* proc = process_get_current();
        uint64_t deadline = proc->block_scratch;
        if (deadline == 0) {
            if (timeout_ms == 0xFFFFFFFF) {
                /* infinite: park until any wake source fires */
                wait_block(lnx_frame_rsp, &g_poll_wq, WAIT_RESTART, 0, 0);
                continue;
            }
            deadline = pit_uptime_us() + timeout_ms * 1000ULL;
            proc->block_scratch = deadline;
        }

        uint64_t now = pit_uptime_us();
        if (now >= deadline) {
            proc->block_scratch = 0;
            return 0;
        }
        netif_poll();
        /* Park until deadline; timer wake restarts the poll(2). Only the
         * timer-pool spin fallback returns here. */
        wait_block(lnx_frame_rsp, &g_poll_wq, WAIT_RESTART, 0,
                   deadline - now);
    }
}

/* getsockopt: SO_PEERCRED for AF_UNIX (Xorg local auth / dbus policy);
 * everything else reports success / zeroed values (SO_ERROR etc.). */
static uint64_t sc_getsockopt_stub(uint64_t fd, uint64_t level, uint64_t optname,
                                   uint64_t optval, uint64_t optlen, uint64_t u6) {
    (void)u6;
    if (level == LNX_SOL_SOCKET && optname == LNX_SO_PEERCRED) {
        enum sock_kind kind;
        void* s = sock_fd_get(fd, &kind);
        if (s == NULL || kind != SOCK_UNIX) return (uint64_t)-92;   /* ENOPROTOOPT */
        if (optlen < 12 || !process_check_user_range(optval, 12)) return -L_EINVAL;
        int pid, uid, gid;
        unix_peer_cred((unix_sock_t*)s, &pid, &uid, &gid);
        int* u = (int*)optval;
        u[0] = pid; u[1] = uid; u[2] = gid;
        if (optlen >= 16 && process_check_user_range(optlen, 4))
            *(uint32_t*)optlen = 12;
        return 0;
    }
    if (optval != 0 && optlen >= 4) {
        if (!process_check_user_range(optval, 4)) return -L_EINVAL;
        *(uint32_t*)optval = 0;
    }
    return 0;
}

static uint64_t sc_getsockname(uint64_t fd, uint64_t uaddr, uint64_t ulen_p,
                               uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    struct file* f = fd_get_file((int)fd);
    if (f == NULL) return -L_EBADF;
    if (f->kind == FILE_T_SOCK_UNIX)
        return (uint64_t)unix_put_addr((unix_sock_t*)f->priv, uaddr, ulen_p);
    /* INET: zeroed sockaddr_in is enough for the current consumers */
    if (uaddr != 0 && ulen_p != 0 &&
        process_check_user_range(uaddr, sizeof(struct lnx_sockaddr_in)) &&
        process_check_user_range(ulen_p, 4)) {
        struct lnx_sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.family = LNX_AF_INET;
        memcpy((void*)uaddr, &sa, sizeof(sa));
        *(uint32_t*)ulen_p = (uint32_t)sizeof(sa);
    }
    return 0;
}

static uint64_t sc_getpeername(uint64_t fd, uint64_t uaddr, uint64_t ulen_p,
                               uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u4; (void)u5; (void)u6;
    struct file* f = fd_get_file((int)fd);
    if (f == NULL) return -L_EBADF;
    if (f->kind == FILE_T_SOCK_UNIX) {
        unix_sock_t* s = (unix_sock_t*)f->priv;
        if (!unix_sock_has_peer(s)) return -L_ENOTCONN;
        return (uint64_t)unix_put_addr(unix_sock_peer(s), uaddr, ulen_p);
    }
    return 0;
}

static uint64_t sc_set_tid_address(uint64_t tidptr, uint64_t u2, uint64_t u3,
                                   uint64_t u4, uint64_t u5, uint64_t u6) {
    (void)u2; (void)u3; (void)u4; (void)u5; (void)u6;
    process_t* proc = process_get_current();
    if (proc == NULL) return 0;
    proc->clear_child_tid = tidptr;
    return proc->tid;
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
    /* Route through the unified file layer: tty files answer TCGETS /
     * TIOCGWINSZ, everything else reports ENOTTY (musl isatty() then
     * falls back to buffered mode - same as a pipe or file on Linux). */
    struct file* f = fd_get_file((int)fd);
    if (f == NULL || f->ops == NULL || f->ops->ioctl == NULL) return -L_EBADF;
    return (uint64_t)f->ops->ioctl(f, cmd, arg);
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
    strcpy(u->v[2], "3.10.0");
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
    lnx_table[LNX_clone] = sc_clone;
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

    /* P1.4: AF_UNIX + socket plumbing */
    lnx_table[LNX_listen]       = sc_listen;
    lnx_table[LNX_accept]       = sc_accept4;
    lnx_table[LNX_accept4]      = sc_accept4;
    lnx_table[LNX_socketpair]   = sc_socketpair;
    lnx_table[LNX_getsockname]  = sc_getsockname;
    lnx_table[LNX_getpeername]  = sc_getpeername;

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
