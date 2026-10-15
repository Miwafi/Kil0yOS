#ifndef FILE_H
#define FILE_H

#include "lib/types.h"

/* Unified per-process descriptor layer (P1.2). Every open object - VFS
 * file, TCP/UDP socket, console TTY - is a struct file installed in the
 * process fd table (process->fds[]). Dispatch goes through file_ops so
 * read/write/poll/ioctl/close work uniformly across kinds. Socket fds
 * live in the same table (no SOCK_FD_BASE split), which also makes
 * dup2/close/poll work on them like on Linux. */

#define LNX_MAX_FDS 64          /* per-process fd table size */

enum file_kind {
    FILE_T_TTY     = 1,         /* console line discipline (fd 0/1/2) */
    FILE_T_VFS,                 /* memory-fs file/dir payload (lnxvfs) */
    FILE_T_SOCK_TCP,
    FILE_T_SOCK_UDP,
    FILE_T_SOCK_UNIX,           /* P1.4 */
    /* P1.7: FILE_T_PIPE, FILE_T_EVENTFD;
     * P1.9: FILE_T_DEVFB, FILE_T_EVDEV */
};

/* poll(2) readiness bits (subset used by sc_poll/POLL rewrite) */
#define POLL_IN    0x001
#define POLL_OUT   0x004
#define POLL_ERR   0x008
#define POLL_NVAL  0x020

struct file;

struct file_ops {
    /* All return -errno on failure. read/write may return -EAGAIN when
     * the object would block; the syscall layer parks the process on the
     * matching wait queue and restarts the syscall (P1.1). */
    int (*read)(struct file* f, void* buf, size_t count);
    int (*write)(struct file* f, const void* buf, size_t count);
    /* Nonblocking readiness probe: POLL_IN|POLL_OUT bits. */
    int (*poll)(struct file* f);
    int (*ioctl)(struct file* f, uint64_t req, uint64_t arg);
    /* Release the backend object. file_close() kfrees the struct file
     * itself after ops->close returns. */
    int (*close)(struct file* f);
};

struct file {
    const struct file_ops* ops;
    enum file_kind kind;
    void* priv;                 /* backend object (lnx payload, socket...) */
    int refs;                   /* fork shares; freed at last close */
    int flags;                  /* open flags (O_NONBLOCK etc.) */
};

struct process;

/* file objects */
struct file* file_new(enum file_kind kind, const struct file_ops* ops, void* priv);
struct file* file_ref(struct file* f);
int          file_close(struct file* f);       /* -1 on NULL, else ops->close */

/* fd table */
int          fd_install(struct file* f);       /* lowest free fd, -1 */
struct file* fd_get_file(int fd);              /* NULL unless 0<=fd<MAX */
int          fd_close(int fd);                 /* 0, or -EBADF if empty */
int          fd_dup(int fd);
int          fd_dup2(int fd, int fd2);
void         fd_table_init(struct process* p); /* fresh table: tty 0/1/2 */
void         fd_table_fork(struct process* parent, struct process* child);
void         fd_table_share(struct process* parent, struct process* child);
void         fd_table_exit(struct process* p); /* drop group ref (last closes) */

#endif /* FILE_H */
