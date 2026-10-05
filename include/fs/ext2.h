#ifndef EXT2_H
#define EXT2_H

#include "lib/types.h"

struct fs_entry;
struct blkdev;

/* ext2 driver (read: Phase 2.1, write + mounts: Phase 2.4 "mnt").
 *
 * Block device: drivers/disk.h (ATA PIO/DMA or RAM disk fallback) via the
 * blkdev layer - ext2_set_dev selects the backing device (a real partition)
 * or NULL for the legacy absolute-LBA shim.
 *
 * Two kinds of instances live in ext2.c:
 *  - the boot root (ext2_probe/ext2_build_tree/ext2_read_file): read-only,
 *    overlays in fs.c shadow it on writes;
 *  - mount instances created by ext2_mount(): optional write support, used
 *    by the "mnt" command and routed from fs.c through the fs_entry_t.mnt
 *    handle (opaque void* here). */

/* Route ext2 block I/O through `dev` (partition-relative LBAs). NULL
 * restores the legacy disk_read_sector behavior. Must be called before
 * ext2_probe(). */
void ext2_set_dev(struct blkdev* dev);

/* Probe the boot-root device for an ext2 superblock (magic 0xEF53 at byte
 * 1080). Returns 1 and caches the superblock/group layout on success. */
int ext2_probe(void);

/* Build the in-memory fs_entry_t tree from the ext2 root inode (inode 2).
 * Returns the tree root ("/") or NULL on failure. */
struct fs_entry* ext2_build_tree(void);

/* Read up to `size` bytes of an ext2 file's data into `buffer`.
 * Returns the number of bytes read, or a negative value on error. */
int ext2_read_file(uint32_t inode_no, uint8_t* buffer, size_t size);

/* ---- mount instances (the "mnt" command) ------------------------------ */

/* Mount the ext2 filesystem on blkdev `devname` at the existing EMPTY
 * directory `mountpoint` (graft model: the disk tree's children are linked
 * under the mountpoint node). Returns an opaque mount handle or NULL. */
void* ext2_mount(const char* devname, const char* mountpoint, int writable);

/* Detach the mount at `mountpoint`. Refuses when the cwd is inside the
 * subtree, overlay (MEM) nodes exist in it, or another mount is nested
 * below. Returns 0 on success. */
int ext2_umount(const char* mountpoint);

/* Mount registry iteration (for the "mnt" listing). */
int ext2_mount_count(void);
const char* ext2_mount_devname(int i);
const char* ext2_mount_path(int i);
int ext2_mount_is_rw(int i);

/* Write-gate helper: 1 when `mnt` is a live mount instance with write
 * support. Safe on any fs_entry_t.mnt value (NULL / root instance). */
int ext2_mount_is_writable(void* mnt);

/* Node operations routed from fs.c (fs_entry_t.mnt must be the handle). */
uint32_t ext2_create_file(void* mnt, uint32_t dir_ino, const char* name);
uint32_t ext2_create_dir(void* mnt, uint32_t dir_ino, const char* name);
int ext2_write_file(void* mnt, uint32_t ino, const uint8_t* data, size_t size);
int ext2_read_file_at(void* mnt, uint32_t ino, uint8_t* buffer, size_t size);

/* Format `devname` (a blkdev name, e.g. "sd0" or "sd0p1") as ext2 with the
 * geometry the write driver supports: 1 KiB blocks, rev 1, 128-byte inodes,
 * 8192 blocks/group. Refuses the device hosting the boot root or any live
 * mount; pass force=1 to override the boot-root refusal (kilinstall's
 * update path re-formats its own boot device). Returns 0 on success. */
int ext2_mkfs(const char* devname, int force);

/* End-to-end selftest on sd0p1 (or raw sd0 when no MBR partition exists):
 * mkfs -> mount -> create/write/read-back a 300 KiB file (exercises direct,
 * singly and doubly indirect blocks) plus a small file -> umount. Prints
 * MNT_SELFTEST_OK on success. Returns 0 on success. */
int ext2_selftest(void);

#endif
