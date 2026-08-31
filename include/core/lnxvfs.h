#ifndef LNXVFS_H
#define LNXVFS_H

#include "lib/types.h"

/* Linux-ABI fd layer over the memory fs (Phase 1). Each open file is
 * cached in a kernel buffer; writes flush back to the fs on close(). */

#define LNX_MAX_FDS 16

struct process;
struct lnx_file;

/* Open descriptors are inherited by fork (table copied by pointer). */
int  lnxvfs_open(const char* path, int flags, int mode);
int  lnxvfs_close(int fd);
int  lnxvfs_read(int fd, char* buf, size_t count);
int  lnxvfs_write(int fd, const char* buf, size_t count);
int  lnxvfs_lseek(int fd, int whence, long long off);   /* returns new offset */
long long lnxvfs_seek_pos(int fd);
/* File-backed mmap support (Phase 3.0): size of the fd's file (negative
 * errno on failure) and read without moving the fd cursor. */
long long lnxvfs_filesize(int fd);
int  lnxvfs_pread(int fd, void* buf, size_t count, long long off);
int  lnxvfs_getdents64(int fd, void* ubuf, size_t count);
int  lnxvfs_fstat(int fd, void* ubuf);
int  lnxvfs_stat_path(const char* path, void* ubuf);
/* statx(2) format fill (musl 1.2.x stat/fstat all go through statx) */
int  lnxvfs_statx_fd(int fd, void* ubuf);
int  lnxvfs_statx_path(const char* path, void* ubuf);
int  lnxvfs_dup2(int oldfd, int newfd);
int  lnxvfs_is_directory_path(const char* path);

int  lnxvfs_mkdir(const char* path);
int  lnxvfs_rmdir(const char* path);
int  lnxvfs_unlink(const char* path);
int  lnxvfs_chdir(const char* path);
long long lnxvfs_getcwd(char* buf, size_t size);

/* Close every open fd of the process (flush + free). Called on exit. */
void lnxvfs_close_all(struct process* proc);

/* Copy the fd pointer table of parent into child (fork). */
void lnxvfs_inherit_fds(struct process* parent, struct process* child);

#endif
