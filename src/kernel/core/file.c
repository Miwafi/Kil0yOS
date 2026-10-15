#include "core/file.h"
#include "core/process.h"
#include "core/tty.h"
#include "lib/stdlib.h"
#include "lib/string.h"

/* Unified fd layer (P1.2): refcounted struct file objects + a per-process
 * fd table. The console (fd 0/1/2) is exposed as real TTY file objects so
 * syscalls no longer special-case "fd < 3". */

/* --- console TTY backend -------------------------------------------------- */

static int tty_f_read(struct file* f, void* buf, size_t count) {
    (void)f;
    if (count == 0) return 0;
    return tty_read((char*)buf, count);
}

static int tty_f_write(struct file* f, const void* buf, size_t count) {
    (void)f;
    return tty_write((const char*)buf, count);
}

static int tty_f_poll(struct file* f) {
    (void)f;
    return POLL_IN | POLL_OUT;
}

/* ioctl: zeroed termios (TCGETS) makes musl treat the console as a tty and
 * line-buffer stdout; TIOCGWINSZ reports the fixed 1024x768 GOP grid. */
static int tty_f_ioctl(struct file* f, uint64_t req, uint64_t arg) {
    (void)f;
    process_t* proc = process_get_current();
    if (proc == NULL || !process_check_user_range(arg, 60)) return -22;
    if (req == 0x5401) {                    /* TCGETS */
        memset((void*)arg, 0, 60);
        return 0;
    }
    if (req == 0x5413) {                    /* TIOCGWINSZ */
        struct { uint16_t rows, cols, xpixel, ypixel; }* w = (void*)arg;
        if (!process_check_user_range(arg, sizeof(*w))) return -22;
        w->rows = 24;
        w->cols = 80;
        w->xpixel = 1024;
        w->ypixel = 768;
        return 0;
    }
    return -25;                             /* ENOTTY */
}

static int tty_f_close(struct file* f) {
    (void)f;                                /* stateless backend */
    return 0;
}

static const struct file_ops tty_ops = {
    .read = tty_f_read,
    .write = tty_f_write,
    .poll = tty_f_poll,
    .ioctl = tty_f_ioctl,
    .close = tty_f_close,
};

/* --- refcounted file objects ---------------------------------------------- */

struct file* file_new(enum file_kind kind, const struct file_ops* ops, void* priv) {
    struct file* f = (struct file*)kmalloc(sizeof(struct file));
    if (f == NULL) return NULL;
    f->ops = ops;
    f->kind = kind;
    f->priv = priv;
    f->refs = 1;
    f->flags = 0;
    return f;
}

struct file* file_ref(struct file* f) {
    if (f) f->refs++;
    return f;
}

int file_close(struct file* f) {
    if (f == NULL) return -1;
    int r = 0;
    if (--f->refs == 0) {
        if (f->ops && f->ops->close) r = f->ops->close(f);
        kfree(f);
    }
    return r;
}

/* --- per-process fd table (files_t group, P1.3) ---------------------------- */

int fd_install(struct file* f) {
    process_t* proc = process_get_current();
    if (proc == NULL || proc->files == NULL || f == NULL) return -1;
    for (int fd = 0; fd < LNX_MAX_FDS; fd++) {
        if (proc->files->fds[fd] == NULL) {
            proc->files->fds[fd] = f;
            return fd;
        }
    }
    return -1;
}

struct file* fd_get_file(int fd) {
    process_t* proc = process_get_current();
    if (proc == NULL || proc->files == NULL || fd < 0 || fd >= LNX_MAX_FDS)
        return NULL;
    return proc->files->fds[fd];
}

int fd_close(int fd) {
    process_t* proc = process_get_current();
    if (proc == NULL || proc->files == NULL || fd < 0 || fd >= LNX_MAX_FDS)
        return -9;
    struct file* f = proc->files->fds[fd];
    if (f == NULL) return -9;               /* -EBADF */
    proc->files->fds[fd] = NULL;
    return file_close(f);
}

int fd_dup(int fd) {
    struct file* f = fd_get_file(fd);
    if (f == NULL) return -9;
    return fd_install(file_ref(f));
}

int fd_dup2(int fd, int fd2) {
    if (fd == fd2) return fd2;
    struct file* f = fd_get_file(fd);
    if (f == NULL) return -9;
    if (fd2 < 0 || fd2 >= LNX_MAX_FDS) return -9;
    fd_close(fd2);
    process_t* proc = process_get_current();
    if (proc == NULL || proc->files == NULL) return -9;
    proc->files->fds[fd2] = file_ref(f);
    return fd2;
}

void fd_table_init(struct process* p) {
    if (p == NULL) return;
    p->files = (files_t*)kmalloc(sizeof(files_t));
    if (p->files == NULL) return;
    p->files->refs = 1;
    for (int i = 0; i < LNX_MAX_FDS; i++) p->files->fds[i] = NULL;
    for (int i = 0; i < 3; i++) {
        struct file* f = file_new(FILE_T_TTY, &tty_ops, NULL);
        if (f == NULL) return;
        p->files->fds[i] = f;
    }
}

/* fork: child gets its own copy of the fd table (each entry ref'd) */
void fd_table_fork(struct process* parent, struct process* child) {
    if (parent == NULL || child == NULL) return;
    child->files = (files_t*)kmalloc(sizeof(files_t));
    if (child->files == NULL) return;
    child->files->refs = 1;
    for (int i = 0; i < LNX_MAX_FDS; i++) {
        child->files->fds[i] = file_ref(parent->files->fds[i]);
    }
}

/* clone(CLONE_FILES): child shares the parent's fd table by refcount */
void fd_table_share(struct process* parent, struct process* child) {
    if (parent == NULL || child == NULL || parent->files == NULL) return;
    child->files = parent->files;
    child->files->refs++;
}

/* Drop the caller's reference to its fd group; the last reference
 * closes every file still open in the table. */
void fd_table_exit(struct process* p) {
    if (p == NULL || p->files == NULL) return;
    files_t* t = p->files;
    p->files = NULL;
    if (--t->refs > 0) return;
    for (int i = 0; i < LNX_MAX_FDS; i++) {
        struct file* f = t->fds[i];
        if (f) file_close(f);
    }
    kfree(t);
}
