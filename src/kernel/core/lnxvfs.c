#include "core/lnxvfs.h"
#include "core/process.h"
#include "fs/fs.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "mm/memory.h"

/* --- Linux errno values --- */
#define L_EBADF   9
#define L_ENOMEM  12
#define L_EINVAL  22
#define L_ENOENT  2
#define L_EISDIR  21
#define L_ENOTDIR 20
#define L_EEXIST  17
#define L_ENOTEMPTY 39
#define L_ERANGE  34

/* open(2) flags */
#define L_O_ACCMODE   3
#define L_O_CREAT     0x40
#define L_O_TRUNC     0x200
#define L_O_DIRECTORY 0x10000

/* seek */
#define L_SEEK_SET 0
#define L_SEEK_CUR 1
#define L_SEEK_END 2

#define L_AT_FDCWD        -100
#define L_AT_EMPTY_PATH   0x1000

typedef struct lnx_file {
    int used;
    int dirty;
    fs_entry_t* entry;
    uint8_t* cache;      /* whole-file content (memory fs files are small) */
    size_t size;         /* cache size == file size */
    size_t pos;          /* read/write cursor */
    int dir_index;       /* next child index for getdents64 */
} lnx_file_t;

static lnx_file_t* fd_get(int fd) {
    process_t* proc = process_get_current();
    if (proc == NULL || fd < 0 || fd >= LNX_MAX_FDS) return NULL;
    /* fds[0..2] are console (NULL) unless redirected via dup2 */
    return proc->fds[fd];
}

static int fd_alloc(void) {
    process_t* proc = process_get_current();
    if (proc == NULL) return -1;
    for (int i = 3; i < LNX_MAX_FDS; i++) {
        if (proc->fds[i] == NULL) return i;
    }
    return -1;
}

/* Create a file or directory at an arbitrary absolute/relative path by
 * temporarily re-rooting the fs cwd at the parent directory. */
static fs_entry_t* fs_create_at(const char* path, int make_dir) {
    char parent_path[MAX_PATH_LENGTH];
    strncpy(parent_path, path, sizeof(parent_path) - 1);
    parent_path[sizeof(parent_path) - 1] = 0;

    char* slash = NULL;
    for (char* p = parent_path; *p; p++) {
        if (*p == '/') slash = p;
    }
    const char* base;
    if (slash == NULL) {
        base = parent_path;               /* relative, current dir */
    } else if (slash == parent_path) {
        base = slash + 1;                 /* "/name" -> root */
        if (base[0] == 0) return NULL;
    } else {
        base = slash + 1;
        *slash = 0;                       /* parent_path = dir part */
    }

    fs_entry_t* parent = (slash == NULL || slash == parent_path)
        ? ((slash == parent_path) ? fs_root() : fs_current())
        : fs_resolve_path(parent_path);
    if (parent == NULL || parent->type != FS_TYPE_DIRECTORY) return NULL;

    fs_entry_t* prev = fs_current();
    fs_set_current(parent);
    fs_entry_t* f = make_dir ? fs_create_dir(base) : fs_create_file(base);
    fs_set_current(prev);
    return f;
}

int lnxvfs_open(const char* path, int flags, int mode) {
    (void)mode;
    if (path == NULL) return -L_EINVAL;
    if (path[0] == 0) return -L_ENOENT;

    fs_entry_t* e = fs_resolve_path(path);
    if (e == NULL) {
        if (!(flags & L_O_CREAT)) return -L_ENOENT;
        e = fs_create_at(path, 0);
        if (e == NULL) return -L_ENOENT;
    } else if (flags & L_O_DIRECTORY && e->type != FS_TYPE_DIRECTORY) {
        return -L_ENOTDIR;
    }

    int fd = fd_alloc();
    if (fd < 0) return -L_ENOMEM;

    lnx_file_t* f = kmalloc(sizeof(lnx_file_t));
    if (f == NULL) return -L_ENOMEM;
    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->entry = e;

    if (e->type == FS_TYPE_FILE) {
        f->size = e->size;
        if (f->size > 0) {
            f->cache = kmalloc(f->size);
            if (f->cache == NULL) { kfree(f); return -L_ENOMEM; }
            fs_read_file(e, f->cache, f->size);
        }
        if (flags & L_O_TRUNC) {
            f->size = 0;
            f->dirty = 1;
        }
    }

    process_t* proc = process_get_current();
    proc->fds[fd] = f;
    return fd;
}

static void fd_flush(lnx_file_t* f) {
    if (f == NULL || !f->dirty || f->entry == NULL) return;
    if (f->entry->type != FS_TYPE_FILE) return;
    fs_write_file(f->entry, f->cache, f->size);
    f->dirty = 0;
}

int lnxvfs_close(int fd) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL) return -L_EBADF;
    fd_flush(f);
    process_t* proc = process_get_current();
    proc->fds[fd] = NULL;
    if (f->cache) kfree(f->cache);
    kfree(f);
    return 0;
}

void lnxvfs_close_all(struct process* proc) {
    if (proc == NULL) return;
    for (int i = 3; i < LNX_MAX_FDS; i++) {
        lnx_file_t* f = proc->fds[i];
        if (f == NULL) continue;
        fd_flush(f);
        if (f->cache) kfree(f->cache);
        kfree(f);
        proc->fds[i] = NULL;
    }
}

void lnxvfs_inherit_fds(struct process* parent, struct process* child) {
    if (parent == NULL || child == NULL) return;
    for (int i = 0; i < LNX_MAX_FDS; i++) child->fds[i] = parent->fds[i];
}

int lnxvfs_read(int fd, char* buf, size_t count) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL) return -L_EBADF;
    if (f->entry->type == FS_TYPE_DIRECTORY) return -L_EISDIR;
    if (f->pos >= f->size) return 0;
    size_t n = f->size - f->pos;
    if (n > count) n = count;
    memcpy(buf, f->cache + f->pos, n);
    f->pos += n;
    return (int)n;
}

int lnxvfs_write(int fd, const char* buf, size_t count) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL) return -L_EBADF;
    if (f->entry->type == FS_TYPE_DIRECTORY) return -L_EISDIR;

    if (f->pos + count > f->size) {
        size_t nsize = f->pos + count;
        uint8_t* ncache = krealloc(f->cache, nsize ? nsize : 1);
        if (ncache == NULL) return -L_ENOMEM;
        f->cache = ncache;
        f->size = nsize;
    }
    memcpy(f->cache + f->pos, buf, count);
    f->pos += count;
    f->dirty = 1;
    return (int)count;
}

long long lnxvfs_seek_pos(int fd) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL) return -1;
    return (long long)f->pos;
}

/* Size of the fd's file (for file-backed mmap bounds). */
long long lnxvfs_filesize(int fd) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL || f->entry == NULL) return -L_EBADF;
    if (f->entry->type != FS_TYPE_FILE) return -L_EISDIR;
    return (long long)f->size;
}

/* Read from the fd's file at an offset without moving the cursor.
 * Returns bytes read (short read at EOF) or negative errno. */
int lnxvfs_pread(int fd, void* buf, size_t count, long long off) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL || f->entry == NULL) return -L_EBADF;
    if (f->entry->type != FS_TYPE_FILE) return -L_EISDIR;
    if (off < 0) return -L_EINVAL;
    if (off >= (long long)f->size || count == 0) return 0;
    size_t avail = f->size - (size_t)off;
    if (count > avail) count = avail;
    memcpy(buf, f->cache + (size_t)off, count);
    return (int)count;
}

int lnxvfs_lseek(int fd, int whence, long long off) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL) return -L_EBADF;

    long long base;
    switch (whence) {
    case L_SEEK_SET: base = 0; break;
    case L_SEEK_CUR: base = (long long)f->pos; break;
    case L_SEEK_END: base = (long long)f->size; break;
    default: return -L_EINVAL;
    }
    long long np = base + off;
    if (np < 0) return -L_EINVAL;
    if (whence != L_SEEK_END && np > (long long)f->size) np = f->size;

    /* Files only; directory cursors move through getdents64 */
    if (f->entry->type == FS_TYPE_DIRECTORY) return -L_EINVAL;

    f->pos = (size_t)np;
    return (int)np;
}

/* --- directory iteration (getdents64) --- */

struct lnx_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];    /* NUL-terminated, then padded to 8 */
};

#define L_DT_REG 8
#define L_DT_DIR 4

int lnxvfs_getdents64(int fd, void* ubuf, size_t count) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL) return -L_EBADF;
    if (f->entry->type != FS_TYPE_DIRECTORY) return -L_ENOTDIR;

    fs_entry_t* dir = f->entry;
    uint8_t* out = (uint8_t*)ubuf;
    size_t used = 0;

    while (f->dir_index < MAX_DIR_ENTRIES) {
        fs_entry_t* c = dir->children[f->dir_index];
        f->dir_index++;
        if (c == NULL || c->name[0] == 0) continue;

        size_t namelen = strlen(c->name);
        size_t reclen = 19 + namelen + 1;          /* header + name + NUL */
        reclen = (reclen + 7) & ~(size_t)7;        /* 8-byte align */
        if (used + reclen > count) {
            if (used == 0) return -L_EINVAL;       /* buffer too small */
            break;                                  /* deliver what we have */
        }

        struct lnx_dirent64* d = (struct lnx_dirent64*)(out + used);
        memset(d, 0, reclen);
        d->d_ino = 1 + (uint64_t)f->dir_index;
        d->d_off = (int64_t)f->dir_index;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = (c->type == FS_TYPE_DIRECTORY) ? L_DT_DIR : L_DT_REG;
        memcpy(d->d_name, c->name, namelen + 1);
        used += reclen;
    }
    return (int)used;
}

/* --- stat family --- */

#define L_S_IFDIR  0x4000
#define L_S_IFREG  0x8000
#define L_S_IRWXU  0755

struct lnx_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_ns;
    int64_t  st_mtime;
    int64_t  st_mtime_ns;
    int64_t  st_ctime;
    int64_t  st_ctime_ns;
    int64_t  __unused[3];
};

/* File identity: ld.so matches already-loaded objects by (dev,ino)
 * (_dl_get_file_id). Zero/zero would make every file match the main
 * executable (whose id is zeroed via __RTLD_OPENEXEC), so fill in a
 * nonzero device and an entry-pointer-based stable inode. */
#define LNX_VFS_DEV 0xFE01ULL

static void stat_fill(struct lnx_stat* s, fs_entry_t* e) {
    memset(s, 0, sizeof(*s));
    s->st_dev = LNX_VFS_DEV;
    s->st_ino = (uint64_t)(uintptr_t)e;
    s->st_nlink = 1;
    if (e->type == FS_TYPE_DIRECTORY) {
        s->st_mode = L_S_IFDIR | 0755;
    } else {
        s->st_mode = L_S_IFREG | 0755;
        s->st_size = (int64_t)e->size;
        s->st_blocks = (s->st_size + 511) / 512;
    }
    s->st_blksize = 512;
}

int lnxvfs_fstat(int fd, void* ubuf) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL) return -L_EBADF;
    if (!process_check_user_range((uint64_t)ubuf, sizeof(struct lnx_stat))) {
        return -L_EINVAL;
    }
    stat_fill((struct lnx_stat*)ubuf, f->entry);
    return 0;
}

int lnxvfs_stat_path(const char* path, void* ubuf) {
    if (path == NULL || path[0] == 0) return -L_ENOENT;
    fs_entry_t* e = fs_resolve_path(path);
    if (e == NULL) return -L_ENOENT;
    if (!process_check_user_range((uint64_t)ubuf, sizeof(struct lnx_stat))) {
        return -L_EINVAL;
    }
    stat_fill((struct lnx_stat*)ubuf, e);
    return 0;
}

int lnxvfs_is_directory_path(const char* path) {
    if (path == NULL) return 0;
    fs_entry_t* e = fs_resolve_path(path);
    return (e != NULL && e->type == FS_TYPE_DIRECTORY);
}

/* --- statx (musl 1.2.x stat/fstat implementation detail) ------------- */

#define STATX_BASIC_STATS 0x7ff

struct lnx_statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    uint32_t __pad;
};

struct lnx_statx {
    uint32_t stx_mask;              /* 0x00 */
    uint32_t stx_blksize;           /* 0x04 */
    uint64_t stx_attributes;        /* 0x08 */
    uint32_t stx_nlink;             /* 0x10 */
    uint32_t stx_uid;               /* 0x14 */
    uint32_t stx_gid;               /* 0x18 */
    uint16_t stx_mode;              /* 0x1c */
    uint8_t  __spare0[1];           /* 0x1e */
    uint8_t  __pad1;                /* 0x1f */
    uint64_t stx_ino;               /* 0x20 */
    uint64_t stx_size;              /* 0x28 */
    uint64_t stx_blocks;            /* 0x30 */
    uint64_t stx_attributes_mask;   /* 0x38 */
    struct lnx_statx_timestamp stx_atime;    /* 0x40 */
    struct lnx_statx_timestamp stx_btime;    /* 0x50 */
    struct lnx_statx_timestamp stx_ctime;    /* 0x60 */
    struct lnx_statx_timestamp stx_mtime;    /* 0x70 */
    uint32_t stx_rdev_major;        /* 0x80 */
    uint32_t stx_rdev_minor;        /* 0x84 */
    uint32_t stx_dev_major;         /* 0x88 */
    uint32_t stx_dev_minor;         /* 0x8c */
    uint64_t stx_mnt_id;            /* 0x90 */
};                                  /* sizeof == 0x98 (152) */

static void statx_fill(struct lnx_statx* x, fs_entry_t* e) {
    memset(x, 0, sizeof(*x));
    x->stx_mask = STATX_BASIC_STATS;
    x->stx_blksize = 512;
    x->stx_attributes_mask = 0;
    x->stx_nlink = 1;
    x->stx_ino = (uint64_t)(uintptr_t)e;
    x->stx_dev_major = (LNX_VFS_DEV >> 8) & 0xFFF;
    x->stx_dev_minor = LNX_VFS_DEV & 0xFF;
    if (e->type == FS_TYPE_DIRECTORY) {
        x->stx_mode = L_S_IFDIR | 0755;
    } else {
        x->stx_mode = L_S_IFREG | 0755;
        x->stx_size = (uint64_t)e->size;
        x->stx_blocks = (x->stx_size + 511) / 512;
    }
}

static int statx_check_ubuf(void* ubuf) {
    return process_check_user_range((uint64_t)ubuf, sizeof(struct lnx_statx));
}

int lnxvfs_statx_fd(int fd, void* ubuf) {
    lnx_file_t* f = fd_get(fd);
    if (f == NULL) return -L_EBADF;
    if (!statx_check_ubuf(ubuf)) return -L_EINVAL;
    statx_fill((struct lnx_statx*)ubuf, f->entry);
    return 0;
}

int lnxvfs_statx_path(const char* path, void* ubuf) {
    if (path == NULL || path[0] == 0) return -L_ENOENT;
    fs_entry_t* e = fs_resolve_path(path);
    if (e == NULL) return -L_ENOENT;
    if (!statx_check_ubuf(ubuf)) return -L_EINVAL;
    statx_fill((struct lnx_statx*)ubuf, e);
    return 0;
}

int lnxvfs_dup2(int oldfd, int newfd) {
    process_t* proc = process_get_current();
    if (proc == NULL) return -L_EBADF;
    if (oldfd < 0 || oldfd >= LNX_MAX_FDS || newfd < 0 || newfd >= LNX_MAX_FDS) {
        return -L_EBADF;
    }
    if (oldfd == newfd) return oldfd;
    if (oldfd >= 3 && proc->fds[oldfd] == NULL) return -L_EBADF;

    if (newfd >= 3 && proc->fds[newfd] != NULL) {
        lnxvfs_close(newfd);
    }
    proc->fds[newfd] = proc->fds[oldfd];   /* fd 0/1/2: console alias */
    return newfd;
}

/* --- namespace mutation --- */

int lnxvfs_mkdir(const char* path) {
    if (path == NULL || path[0] == 0) return -L_EINVAL;
    if (fs_resolve_path(path) != NULL) return -L_EEXIST;
    if (fs_create_at(path, 1) == NULL) return -L_ENOENT;
    return 0;
}

int lnxvfs_rmdir(const char* path) {
    if (path == NULL || path[0] == 0) return -L_EINVAL;
    fs_entry_t* e = fs_resolve_path(path);
    if (e == NULL) return -L_ENOENT;
    if (e->type != FS_TYPE_DIRECTORY) return -L_ENOTDIR;
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (e->children[i] != NULL) return -L_ENOTEMPTY;
    }
    if (e == fs_root()) return -L_EINVAL;
    fs_delete_entry_recursive(e);
    return 0;
}

int lnxvfs_unlink(const char* path) {
    if (path == NULL || path[0] == 0) return -L_EINVAL;
    fs_entry_t* e = fs_resolve_path(path);
    if (e == NULL) return -L_ENOENT;
    if (e->type != FS_TYPE_FILE) return -L_EISDIR;
    fs_delete_entry_recursive(e);
    return 0;
}

int lnxvfs_chdir(const char* path) {
    if (path == NULL || path[0] == 0) return -L_ENOENT;
    fs_entry_t* e = fs_resolve_path(path);
    if (e == NULL) return -L_ENOENT;
    if (e->type != FS_TYPE_DIRECTORY) return -L_ENOTDIR;
    fs_set_current(e);
    return 0;
}

long long lnxvfs_getcwd(char* buf, size_t size) {
    if (buf == NULL || size == 0) return -L_EINVAL;
    if (!process_check_user_range((uint64_t)buf, size)) return -L_EINVAL;

    /* Build the path by collecting names up to the root, then writing
     * them in reverse order after a leading '/'. */
    const char* segs[24];
    int nseg = 0;
    fs_entry_t* e = fs_current();
    while (e != NULL && e != fs_root() && e->parent != e && nseg < 24) {
        segs[nseg++] = e->name;
        e = e->parent;
    }

    size_t pos = 0;
    if (pos + 1 >= size) return -L_ERANGE;
    buf[pos++] = '/';
    for (int i = nseg - 1; i >= 0; i--) {
        size_t l = strlen(segs[i]);
        if (pos + l + 1 >= size) return -L_ERANGE;
        memcpy(buf + pos, segs[i], l);
        pos += l;
        if (i > 0) buf[pos++] = '/';
    }
    buf[pos] = 0;
    return (long long)pos;
}
