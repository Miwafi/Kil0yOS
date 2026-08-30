#ifndef EXT2_H
#define EXT2_H

#include "lib/types.h"

struct fs_entry;

/* ext2 read-only driver (Phase 2.1).
 * Block device: drivers/disk.h (ATA PIO/DMA or RAM disk fallback).
 * The mounted tree is exposed through the shared fs_entry_t namespace,
 * writes are handled by the memory overlay in fs.c (Phase 2.2). */

/* Probe the disk for an ext2 superblock (magic 0xEF53 at byte 1080).
 * Returns 1 and caches the superblock/group layout on success. */
int ext2_probe(void);

/* Build the in-memory fs_entry_t tree from the ext2 root inode (inode 2).
 * Returns the tree root ("/") or NULL on failure. */
struct fs_entry* ext2_build_tree(void);

/* Read up to `size` bytes of an ext2 file's data into `buffer`.
 * Returns the number of bytes read, or a negative value on error. */
int ext2_read_file(uint32_t inode_no, uint8_t* buffer, size_t size);

#endif
