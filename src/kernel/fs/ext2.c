#include "fs/ext2.h"
#include "fs/fs.h"
#include "drivers/disk.h"
#include "drivers/blkdev.h"
#include "drivers/video/vga.h"
#include "mm/memory.h"
#include "sched/scheduler.h"
#include "lib/string.h"
#include "lib/stdlib.h"

/* ---- on-disk constants ---------------------------------------------- */

#define EXT2_SUPER_MAGIC   0xEF53
#define EXT2_ROOT_INO      2
#define EXT2_LOSTFOUND_INO 11

#define EXT2_FT_REG_FILE   1
#define EXT2_FT_DIR        2

#define EXT2_S_IFDIR       0x4000
#define EXT2_S_IFREG       0x8000

/* superblock field offsets (superblock starts at byte 1024 = sector 2) */
#define SB_OFF_INODES_COUNT     0
#define SB_OFF_BLOCKS_COUNT     4
#define SB_OFF_FIRST_DATA_BLOCK 20
#define SB_OFF_LOG_BLOCK_SIZE   24
#define SB_OFF_BLOCKS_PER_GROUP 32
#define SB_OFF_FRAGS_PER_GROUP  36
#define SB_OFF_INODES_PER_GROUP 40
#define SB_OFF_MAGIC            56
#define SB_OFF_STATE            58
#define SB_OFF_REV_LEVEL        76
#define SB_OFF_FIRST_INO        84
#define SB_OFF_INODE_SIZE       88

/* group descriptor offsets (32 bytes each) */
#define GD_OFF_BLOCK_BITMAP 0
#define GD_OFF_INODE_BITMAP 4
#define GD_OFF_INODE_TABLE  8

/* inode field offsets */
#define INO_OFF_MODE            0
#define INO_OFF_SIZE            4
#define INO_OFF_BLOCK           40   /* i_block[15], 60 bytes */

/* limits */
#define EXT2_MAX_GROUPS      256     /* 2 GiB with 1 KiB blocks/8192 bpg */
#define EXT2_MAX_MOUNTS      4
#define EXT2_MOUNTPOINT_MAX  64

/* geometry produced by ext2_mkfs - the write driver requires exactly this
 * layout (1 KiB blocks so the block bitmap is one block, rev 1 so the
 * inode-size field is honored, 128-byte inodes, 2048 inodes = 256-block
 * table per group) */
#define MKFS_BLOCK_SIZE       1024
#define MKFS_BLOCKS_PER_GROUP 8192
#define MKFS_INODES_PER_GROUP 2048
#define MKFS_INODE_SIZE       128

/* ---- mount instance ----------------------------------------------------
 * One instance per mounted ext2 filesystem plus the boot root (root_mnt).
 * Group descriptors are cached at probe time; every block access goes
 * through the instance's blkdev (partition-relative LBAs). */
typedef struct ext2_mount {
    blkdev_t* dev;
    char devname[16];
    char mountpoint[EXT2_MOUNTPOINT_MAX];
    int writable;
    int ready;
    /* superblock */
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t rev_level;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t gd_block;      /* first block of the group descriptor table */
    /* cached group descriptors */
    int ngroups;
    uint32_t bg_block_bitmap[EXT2_MAX_GROUPS];
    uint32_t bg_inode_bitmap[EXT2_MAX_GROUPS];
    uint32_t bg_inode_table[EXT2_MAX_GROUPS];
    /* graft point (umount) */
    fs_entry_t* target;
    int prev_backend;       /* graft point backend, restored at umount */
} ext2_mount_t;

/* boot root: fs_init probes it; always read-only (overlay writes in fs.c) */
static ext2_mount_t root_mnt;

/* live mounts created by the mnt command */
static ext2_mount_t* mnt_table[EXT2_MAX_MOUNTS];
static int mnt_count = 0;

void ext2_set_dev(blkdev_t* dev) {
    root_mnt.dev = dev;
}

/* ---- low-level block I/O ------------------------------------------------ */

static int m_read_block(ext2_mount_t* m, uint32_t block, uint8_t* buffer) {
    uint32_t first_sector = block * m->sectors_per_block;
    if (m->dev != NULL) {
        return blkdev_read(m->dev, first_sector, m->sectors_per_block, buffer);
    }
    for (uint32_t s = 0; s < m->sectors_per_block; s++) {
        if (disk_read_sector(first_sector + s, buffer + s * DISK_SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

static int m_write_block(ext2_mount_t* m, uint32_t block, const uint8_t* buffer) {
    if (m->dev == NULL) return -1;   /* legacy shim has no write device here */
    uint32_t first_sector = block * m->sectors_per_block;
    return blkdev_write(m->dev, first_sector, m->sectors_per_block, buffer);
}

/* ---- bitmap allocation ---------------------------------------------------
 * Bit conventions (standard ext2, 1 KiB blocks):
 *   block bitmap bit i of group g  <-> block (first_data_block + g*bpg + i)
 *   inode bitmap bit i of group g  <-> inode (g*ipg + i + 1) */

static uint32_t m_alloc_block(ext2_mount_t* m) {
    if (!m->ready || m->dev == NULL) return 0;
    uint8_t* bm = (uint8_t*)kmalloc(m->block_size);
    if (bm == NULL) return 0;
    uint32_t result = 0;
    for (int g = 0; g < m->ngroups && result == 0; g++) {
        if (m_read_block(m, m->bg_block_bitmap[g], bm) != 0) break;
        for (uint32_t byte = 0; byte < m->block_size && result == 0; byte++) {
            if (bm[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (bm[byte] & (1u << bit)) continue;
                uint32_t b = m->first_data_block + (uint32_t)g * m->blocks_per_group
                           + byte * 8 + bit;
                if (b >= m->blocks_count) break;   /* rest of group beyond fs */
                bm[byte] |= (1u << bit);
                if (m_write_block(m, m->bg_block_bitmap[g], bm) == 0) result = b;
                break;
            }
        }
    }
    kfree(bm);
    if (result == 0) klog("[ext2] no free blocks\n");
    return result;
}

static uint32_t m_alloc_inode(ext2_mount_t* m) {
    if (!m->ready || m->dev == NULL) return 0;
    uint8_t* bm = (uint8_t*)kmalloc(m->block_size);
    if (bm == NULL) return 0;
    uint32_t result = 0;
    for (int g = 0; g < m->ngroups && result == 0; g++) {
        if (m_read_block(m, m->bg_inode_bitmap[g], bm) != 0) break;
        /* inodes_per_group bits, padded to byte granularity */
        uint32_t nbytes = (m->inodes_per_group + 7) / 8;
        for (uint32_t byte = 0; byte < nbytes && result == 0; byte++) {
            if (bm[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                uint32_t idx = byte * 8 + bit;
                if (idx >= m->inodes_per_group) break;
                if (bm[byte] & (1u << bit)) continue;
                uint32_t ino = (uint32_t)g * m->inodes_per_group + idx + 1;
                if (ino > m->inodes_count) break;
                bm[byte] |= (1u << bit);
                if (m_write_block(m, m->bg_inode_bitmap[g], bm) == 0) result = ino;
                break;
            }
        }
    }
    kfree(bm);
    if (result == 0) klog("[ext2] no free inodes\n");
    return result;
}

/* ---- inodes -------------------------------------------------------------- */

typedef struct {
    uint16_t mode;
    uint32_t size;
    uint32_t block[15];
} ext2_inode_t;

static int m_inode_locate(ext2_mount_t* m, uint32_t ino,
                          uint32_t* blk, uint32_t* off) {
    uint32_t group = (ino - 1) / m->inodes_per_group;
    uint32_t index = (ino - 1) % m->inodes_per_group;
    if (group >= (uint32_t)m->ngroups) return -1;
    /* 128/256-byte inodes never straddle a block boundary for the first
     * 100 bytes this driver touches */
    uint32_t byte_off = index * m->inode_size;
    *blk = m->bg_inode_table[group] + byte_off / m->block_size;
    *off = byte_off % m->block_size;
    return 0;
}

static int m_read_inode(ext2_mount_t* m, uint32_t ino, ext2_inode_t* out) {
    if (!m->ready || ino < 1 || ino > m->inodes_count) return -1;
    uint32_t blk, off;
    if (m_inode_locate(m, ino, &blk, &off) != 0) return -1;
    uint8_t* buf = (uint8_t*)kmalloc(m->block_size);
    if (buf == NULL) return -1;
    int rc = -1;
    if (m_read_block(m, blk, buf) == 0) {
        uint8_t* raw = buf + off;
        out->mode = *(uint16_t*)(raw + INO_OFF_MODE);
        out->size = *(uint32_t*)(raw + INO_OFF_SIZE);
        memcpy(out->block, raw + INO_OFF_BLOCK, 15 * sizeof(uint32_t));
        rc = 0;
    }
    kfree(buf);
    return rc;
}

static int m_write_inode(ext2_mount_t* m, uint32_t ino, const ext2_inode_t* in) {
    if (!m->ready || m->dev == NULL) return -1;
    uint32_t blk, off;
    if (m_inode_locate(m, ino, &blk, &off) != 0) return -1;
    uint8_t* buf = (uint8_t*)kmalloc(m->block_size);
    if (buf == NULL) return -1;
    int rc = -1;
    if (m_read_block(m, blk, buf) == 0) {
        uint8_t* raw = buf + off;
        *(uint16_t*)(raw + INO_OFF_MODE) = in->mode;
        *(uint32_t*)(raw + INO_OFF_SIZE) = in->size;
        memcpy(raw + INO_OFF_BLOCK, in->block, 15 * sizeof(uint32_t));
        rc = m_write_block(m, blk, buf);
    }
    kfree(buf);
    return rc;
}

/* ---- block mapping ------------------------------------------------------- */

/* Read-only bmap (read path). Write path resolves + allocates inline. */
static uint32_t m_bmap(ext2_mount_t* m, const uint32_t i_block[15], uint32_t bn,
                       uint8_t* ibuf) {
    uint32_t ppb = m->block_size / 4; /* pointers per block */

    if (bn < 12) return i_block[bn];
    bn -= 12;

    if (bn < ppb) { /* singly indirect */
        uint32_t ind = i_block[12];
        if (ind == 0) return 0;
        if (m_read_block(m, ind, ibuf) != 0) return 0;
        return *(uint32_t*)(ibuf + bn * 4);
    }
    bn -= ppb;

    if (bn < ppb * ppb) { /* doubly indirect */
        uint32_t dind = i_block[13];
        if (dind == 0) return 0;
        if (m_read_block(m, dind, ibuf) != 0) return 0;
        uint32_t l1 = *(uint32_t*)(ibuf + (bn / ppb) * 4);
        if (l1 == 0) return 0;
        if (m_read_block(m, l1, ibuf) != 0) return 0;
        return *(uint32_t*)(ibuf + (bn % ppb) * 4);
    }
    return 0;   /* triple indirect: read path unsupported (write path too) */
}

/* Indirect-block pointer read/patch (write path). */
static int m_ind_get(ext2_mount_t* m, uint32_t blk, uint32_t idx,
                     uint8_t* ibuf, uint32_t* out) {
    if (m_read_block(m, blk, ibuf) != 0) return -1;
    *out = *(uint32_t*)(ibuf + idx * 4);
    return 0;
}

static int m_ind_set(ext2_mount_t* m, uint32_t blk, uint32_t idx,
                     uint32_t val, uint8_t* ibuf) {
    if (m_read_block(m, blk, ibuf) != 0) return -1;
    *(uint32_t*)(ibuf + idx * 4) = val;
    return m_write_block(m, blk, ibuf);
}

/* ---- directory entries ---------------------------------------------------- */

static uint32_t dirent_min_len(uint32_t name_len) {
    return (8 + name_len + 3) & ~3u;
}

static void dirent_put(uint8_t* slot, uint32_t ino, uint16_t rec_len,
                       const char* name, uint8_t ftype) {
    uint8_t name_len = (uint8_t)strlen(name);
    *(uint32_t*)(slot + 0) = ino;
    *(uint16_t*)(slot + 4) = rec_len;
    slot[6] = name_len;
    slot[7] = ftype;
    memcpy(slot + 8, name, name_len);
}

/* Append/link `name` into directory `dir_ino`. Reuses dead slots or slack
 * after the last live entry, else appends a fresh block. */
static int m_add_dirent(ext2_mount_t* m, uint32_t dir_ino, const char* name,
                        uint32_t child_ino, uint8_t ftype) {
    ext2_inode_t dir;
    if (m_read_inode(m, dir_ino, &dir) != 0) return -1;
    if (!(dir.mode & EXT2_S_IFDIR)) return -1;

    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    uint32_t need = dirent_min_len(name_len);

    uint8_t* dbuf = (uint8_t*)kmalloc(m->block_size);
    uint8_t* ibuf = (uint8_t*)kmalloc(m->block_size);
    if (dbuf == NULL || ibuf == NULL) {
        if (dbuf) kfree(dbuf);
        if (ibuf) kfree(ibuf);
        return -1;
    }

    uint32_t nblocks = (dir.size + m->block_size - 1) / m->block_size;
    int rc = -1;
    for (uint32_t b = 0; b < nblocks && rc != 0; b++) {
        uint32_t phys = m_bmap(m, dir.block, b, ibuf);
        if (phys == 0) continue;
        if (m_read_block(m, phys, dbuf) != 0) continue;

        uint32_t off = 0;
        while (off + 8 <= m->block_size) {
            uint32_t ino0 = *(uint32_t*)(dbuf + off);
            uint16_t rec_len = *(uint16_t*)(dbuf + off + 4);
            if (rec_len < 8) break;
            uint8_t nl = dbuf[off + 6];

            if (ino0 == 0) {
                /* dead slot: reuse when it fits (dead space beyond the new
                 * entry stays orphaned until fsck - harmless) */
                if (rec_len >= need) {
                    dirent_put(dbuf + off, child_ino, rec_len, name, ftype);
                    if (m_write_block(m, phys, dbuf) == 0) rc = 0;
                    break;
                }
            } else {
                /* live entry: split its tail slack */
                uint32_t mine = dirent_min_len(nl);
                if (rec_len >= mine + need) {
                    *(uint16_t*)(dbuf + off + 4) = (uint16_t)mine;
                    dirent_put(dbuf + off + mine, child_ino,
                               (uint16_t)(rec_len - mine), name, ftype);
                    if (m_write_block(m, phys, dbuf) == 0) rc = 0;
                    break;
                }
            }
            off += rec_len;
        }
    }

    if (rc != 0) {
        /* append a fresh directory block */
        uint32_t nb = m_alloc_block(m);
        if (nb != 0) {
            memset(dbuf, 0, m->block_size);
            dirent_put(dbuf, child_ino, (uint16_t)m->block_size, name, ftype);
            if (m_write_block(m, nb, dbuf) == 0) {
                uint32_t bn = nblocks;   /* new logical block index */
                if (bn < 12) {
                    dir.block[bn] = nb;
                } else {
                    /* directory grew beyond direct blocks: build indirection */
                    uint32_t ppb = m->block_size / 4;
                    uint32_t l1 = bn - 12;
                    if (l1 < ppb) {
                        if (dir.block[12] == 0) {
                            uint32_t ind = m_alloc_block(m);
                            if (ind == 0) goto done;
                            memset(ibuf, 0, m->block_size);
                            if (m_write_block(m, ind, ibuf) != 0) goto done;
                            dir.block[12] = ind;
                        }
                        if (m_ind_set(m, dir.block[12], l1, nb, ibuf) != 0) goto done;
                    } else {
                        goto done;   /* dirs that big are not expected */
                    }
                }
                dir.size += m->block_size;
                if (m_write_inode(m, dir_ino, &dir) == 0) rc = 0;
            }
        }
    }
done:
    kfree(dbuf);
    kfree(ibuf);
    return rc;
}

/* ---- probe & tree build ---------------------------------------------------- */

static int ext2_probe_m(ext2_mount_t* m) {
    uint8_t buf[DISK_SECTOR_SIZE];
    /* superblock lives at byte 1024 = sector 2 (of the backing device:
     * absolute for the legacy shim, partition-relative for ext2_dev) */
    if (m->dev != NULL) {
        if (blkdev_read(m->dev, 2, 1, buf) != 0) return 0;
    } else {
        if (disk_read_sector(2, buf) != 0) return 0;
    }

    uint16_t magic = *(uint16_t*)(buf + SB_OFF_MAGIC);
    if (magic != EXT2_SUPER_MAGIC) return 0;

    memset(m->bg_block_bitmap, 0, sizeof(m->bg_block_bitmap));
    memset(m->bg_inode_bitmap, 0, sizeof(m->bg_inode_bitmap));
    memset(m->bg_inode_table, 0, sizeof(m->bg_inode_table));
    m->inodes_count      = *(uint32_t*)(buf + SB_OFF_INODES_COUNT);
    m->blocks_count      = *(uint32_t*)(buf + SB_OFF_BLOCKS_COUNT);
    m->first_data_block  = *(uint32_t*)(buf + SB_OFF_FIRST_DATA_BLOCK);
    uint32_t log_bs      = *(uint32_t*)(buf + SB_OFF_LOG_BLOCK_SIZE);
    m->blocks_per_group  = *(uint32_t*)(buf + SB_OFF_BLOCKS_PER_GROUP);
    m->inodes_per_group  = *(uint32_t*)(buf + SB_OFF_INODES_PER_GROUP);
    m->rev_level         = *(uint32_t*)(buf + SB_OFF_REV_LEVEL);

    if (log_bs > 6) return 0; /* block size up to 64 KiB - sanity limit */
    m->block_size = 1024u << log_bs;
    m->sectors_per_block = m->block_size / DISK_SECTOR_SIZE;
    m->inode_size = (m->rev_level >= 1)
        ? *(uint16_t*)(buf + SB_OFF_INODE_SIZE) : 128;
    if (m->inode_size < 128) return 0;
    /* implausible geometry would divide by zero or overflow the GDT cache */
    if (m->blocks_per_group == 0 ||
        m->blocks_per_group > 8 * m->block_size ||
        m->inodes_per_group == 0) return 0;

    /* group descriptor table: block first_data_block + 1 (1KiB blocks) or
     * block 1 (>= 2KiB blocks, where first_data_block is 0) */
    m->gd_block = m->first_data_block + 1;

    m->ngroups = (m->blocks_count + m->blocks_per_group - 1) / m->blocks_per_group;
    uint32_t ng_inodes = (m->inodes_count + m->inodes_per_group - 1)
                       / m->inodes_per_group;
    if (ng_inodes > m->ngroups) m->ngroups = (int)ng_inodes;
    if (m->ngroups > EXT2_MAX_GROUPS) {
        klog("[ext2] too many block groups\n");
        return 0;
    }

    /* cache the group descriptor table */
    {
        uint8_t* gb = (uint8_t*)kmalloc(m->block_size);
        if (gb == NULL) return 0;
        uint32_t cur_blk = 0xFFFFFFFFu;
        int ok = 1;
        for (int g = 0; g < m->ngroups; g++) {
            uint32_t gd_index = (uint32_t)g * 32;
            uint32_t blk = m->gd_block + gd_index / m->block_size;
            uint32_t off = gd_index % m->block_size;
            if (blk != cur_blk) {
                if (m_read_block(m, blk, gb) != 0) { ok = 0; break; }
                cur_blk = blk;
            }
            m->bg_block_bitmap[g] = *(uint32_t*)(gb + off + GD_OFF_BLOCK_BITMAP);
            m->bg_inode_bitmap[g] = *(uint32_t*)(gb + off + GD_OFF_INODE_BITMAP);
            m->bg_inode_table[g] = *(uint32_t*)(gb + off + GD_OFF_INODE_TABLE);
        }
        kfree(gb);
        if (!ok) return 0;
    }

    m->ready = 1;

    klog("[ext2] probe ok: ");
    char num[16];
    itoa((int)m->blocks_count, num, 10, sizeof(num));
    klog(num);
    klog(" blocks, block_size=");
    itoa((int)m->block_size, num, 10, sizeof(num));
    klog(num);
    klog(", inode_size=");
    itoa((int)m->inode_size, num, 10, sizeof(num));
    klog(num);
    klog("\n");
    return 1;
}

int ext2_probe(void) {
    return ext2_probe_m(&root_mnt);
}

static fs_entry_t* ext2_new_node(ext2_mount_t* m, const char* name,
                                 fs_entry_type_t type, uint32_t ino,
                                 uint32_t size, fs_entry_t* parent) {
    fs_entry_t* e = (fs_entry_t*)kmalloc(sizeof(fs_entry_t));
    if (e == NULL) return NULL;

    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->type = type;
    e->size = size;
    e->first_cluster = 0;
    e->attributes = (type == FS_TYPE_DIRECTORY) ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    e->parent = parent;
    e->backend = FS_BACKEND_EXT2;
    e->inode_no = ino;
    e->mem_data = NULL;
    e->mnt = m;
    memset(e->disk_name, 0, sizeof(e->disk_name));
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) e->children[i] = NULL;
    return e;
}

static void ext2_load_dir_recursive(ext2_mount_t* m, fs_entry_t* dir) {
    ext2_inode_t inode;
    if (m_read_inode(m, dir->inode_no, &inode) != 0) return;
    if (!(inode.mode & EXT2_S_IFDIR)) return;

    uint32_t nblocks = (inode.size + m->block_size - 1) / m->block_size;
    uint8_t* bbuf = (uint8_t*)kmalloc(m->block_size);
    uint8_t* ibuf = (uint8_t*)kmalloc(m->block_size);
    if (bbuf == NULL || ibuf == NULL) {
        if (bbuf) kfree(bbuf);
        if (ibuf) kfree(ibuf);
        return;
    }

    int child_idx = 0;
    for (uint32_t b = 0; b < nblocks && child_idx < MAX_DIR_ENTRIES; b++) {
        uint32_t phys = m_bmap(m, inode.block, b, ibuf);
        if (phys == 0) continue;
        if (m_read_block(m, phys, bbuf) != 0) continue;

        /* directory entries never span blocks (ext2 spec) */
        uint32_t off = 0;
        while (off + 8 <= m->block_size && child_idx < MAX_DIR_ENTRIES) {
            uint32_t ino = *(uint32_t*)(bbuf + off);
            uint16_t rec_len = *(uint16_t*)(bbuf + off + 4);
            uint8_t name_len = bbuf[off + 6];
            uint8_t ftype = bbuf[off + 7];
            if (rec_len < 8) break;

            if (ino != 0 && name_len > 0) {
                char name[256];
                if (name_len > (uint8_t)(sizeof(name) - 1)) name_len = sizeof(name) - 1;
                memcpy(name, bbuf + off + 8, name_len);
                name[name_len] = '\0';

                /* skip "." and ".." */
                if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                    fs_entry_type_t type;
                    if (ftype == EXT2_FT_DIR) {
                        type = FS_TYPE_DIRECTORY;
                    } else if (ftype == EXT2_FT_REG_FILE) {
                        type = FS_TYPE_FILE;
                    } else {
                        /* unknown filetype (rev-0 images): read the inode */
                        ext2_inode_t ci;
                        if (m_read_inode(m, ino, &ci) != 0) { off += rec_len; continue; }
                        type = (ci.mode & EXT2_S_IFDIR) ? FS_TYPE_DIRECTORY
                                                        : FS_TYPE_FILE;
                    }

                    uint32_t size = 0;
                    if (type == FS_TYPE_FILE) {
                        ext2_inode_t ci;
                        if (m_read_inode(m, ino, &ci) == 0) size = ci.size;
                    }

                    fs_entry_t* child = ext2_new_node(m, name, type, ino, size, dir);
                    if (child != NULL) {
                        dir->children[child_idx++] = child;
                        if (type == FS_TYPE_DIRECTORY) {
                            ext2_load_dir_recursive(m, child);
                        }
                    }
                }
            }
            off += rec_len;
        }
    }

    if (child_idx >= MAX_DIR_ENTRIES) {
        klog("[ext2] warning: directory truncated at MAX_DIR_ENTRIES\n");
    }

    kfree(bbuf);
    kfree(ibuf);
}

static fs_entry_t* ext2_build_tree_m(ext2_mount_t* m) {
    fs_entry_t* root = ext2_new_node(m, "/", FS_TYPE_DIRECTORY, EXT2_ROOT_INO, 0, NULL);
    if (root == NULL) return NULL;

    ext2_load_dir_recursive(m, root);

    if (root->children[0] == NULL) {
        klog("[ext2] warning: root directory is empty\n");
    }
    return root;
}

fs_entry_t* ext2_build_tree(void) {
    return ext2_build_tree_m(&root_mnt);
}

/* ---- file read --------------------------------------------------------- */

static int ext2_read_file_m(ext2_mount_t* m, uint32_t ino, uint8_t* buffer,
                            size_t size) {
    ext2_inode_t inode;
    if (m_read_inode(m, ino, &inode) != 0) return -1;
    if (size > inode.size) size = inode.size;

    uint8_t* bbuf = (uint8_t*)kmalloc(m->block_size);
    uint8_t* ibuf = (uint8_t*)kmalloc(m->block_size);
    if (bbuf == NULL || ibuf == NULL) {
        if (bbuf) kfree(bbuf);
        if (ibuf) kfree(ibuf);
        return -1;
    }

    size_t offset = 0;
    uint32_t nblocks = ((uint32_t)size + m->block_size - 1) / m->block_size;
    for (uint32_t b = 0; b < nblocks && offset < size; b++) {
        size_t copy = m->block_size;
        if (offset + copy > size) copy = size - offset;

        uint32_t phys = m_bmap(m, inode.block, b, ibuf);
        if (phys == 0) {
            /* sparse hole: ext2 semantics are a zero block, not EOF */
            memset(buffer + offset, 0, copy);
            offset += copy;
            continue;
        }
        if (m_read_block(m, phys, bbuf) != 0) break;
        memcpy(buffer + offset, bbuf, copy);
        offset += copy;
    }

    kfree(bbuf);
    kfree(ibuf);
    return (int)offset;
}

int ext2_read_file(uint32_t ino, uint8_t* buffer, size_t size) {
    return ext2_read_file_m(&root_mnt, ino, buffer, size);
}

int ext2_read_file_at(void* mount, uint32_t ino, uint8_t* buffer, size_t size) {
    ext2_mount_t* m = (ext2_mount_t*)mount;
    if (m == NULL || !m->ready) return -1;
    return ext2_read_file_m(m, ino, buffer, size);
}

/* ---- file write ----------------------------------------------------------
 * Full-content write (fs_write_file semantics). Direct, singly and doubly
 * indirect blocks; triple indirection is rejected (1 KiB blocks cover
 * ~64 MiB per file without it). Freed-below-new-size blocks leak: updates
 * always go through kilinstall's fresh mkfs, so no reclaim path exists. */
int ext2_write_file(void* mount, uint32_t ino, const uint8_t* data, size_t size) {
    ext2_mount_t* m = (ext2_mount_t*)mount;
    if (m == NULL || !m->ready || !m->writable) return -1;

    ext2_inode_t inode;
    if (m_read_inode(m, ino, &inode) != 0) return -1;
    if (!(inode.mode & EXT2_S_IFREG)) return -1;

    uint32_t bs = m->block_size;
    uint32_t ppb = bs / 4;
    uint32_t nblocks = (uint32_t)((size + bs - 1) / bs);

    uint8_t* scratch = (uint8_t*)kmalloc(bs);
    uint8_t* ib1 = (uint8_t*)kmalloc(bs);
    uint8_t* ib2 = (uint8_t*)kmalloc(bs);
    if (scratch == NULL || ib1 == NULL || ib2 == NULL) {
        if (scratch) kfree(scratch);
        if (ib1) kfree(ib1);
        if (ib2) kfree(ib2);
        return -1;
    }

    int rc = -1;
    size_t off = 0;
    for (uint32_t bn = 0; bn < nblocks; bn++, off += bs) {
        kernel_heartbeat_touch();   /* kwatchdog: multi-MB writes take >10s */
        uint32_t phys = 0;
        if (bn < 12) {
            phys = inode.block[bn];
            if (phys == 0) {
                phys = m_alloc_block(m);
                if (phys == 0) goto out;
                inode.block[bn] = phys;
            }
        } else {
            uint32_t l1 = bn - 12;
            if (l1 < ppb) {
                /* singly indirect */
                if (inode.block[12] == 0) {
                    uint32_t ind = m_alloc_block(m);
                    if (ind == 0) goto out;
                    inode.block[12] = ind;
                }
                if (m_ind_get(m, inode.block[12], l1, ib1, &phys) != 0) goto out;
                if (phys == 0) {
                    phys = m_alloc_block(m);
                    if (phys == 0) goto out;
                    if (m_ind_set(m, inode.block[12], l1, phys, ib1) != 0) goto out;
                }
            } else {
                uint32_t l2 = l1 - ppb;
                if (l2 >= ppb * ppb) {
                    klog("[ext2] triple indirect blocks not supported\n");
                    goto out;
                }
                /* doubly indirect */
                if (inode.block[13] == 0) {
                    uint32_t dind = m_alloc_block(m);
                    if (dind == 0) goto out;
                    inode.block[13] = dind;
                }
                uint32_t l1blk = 0;
                if (m_ind_get(m, inode.block[13], l2 / ppb, ib2, &l1blk) != 0) goto out;
                if (l1blk == 0) {
                    l1blk = m_alloc_block(m);
                    if (l1blk == 0) goto out;
                    if (m_ind_set(m, inode.block[13], l2 / ppb, l1blk, ib2) != 0) goto out;
                }
                if (m_ind_get(m, l1blk, l2 % ppb, ib1, &phys) != 0) goto out;
                if (phys == 0) {
                    phys = m_alloc_block(m);
                    if (phys == 0) goto out;
                    if (m_ind_set(m, l1blk, l2 % ppb, phys, ib1) != 0) goto out;
                }
            }
        }

        uint32_t copy = bs;
        if (off + copy > size) copy = (uint32_t)(size - off);
        memset(scratch, 0, bs);
        memcpy(scratch, data + off, copy);
        if (m_write_block(m, phys, scratch) != 0) goto out;
    }

    inode.size = (uint32_t)size;
    if (m_write_inode(m, ino, &inode) != 0) goto out;
    rc = (int)size;
out:
    kfree(scratch);
    kfree(ib1);
    kfree(ib2);
    return rc;
}

/* ---- node creation -------------------------------------------------------- */

uint32_t ext2_create_file(void* mount, uint32_t dir_ino, const char* name) {
    ext2_mount_t* m = (ext2_mount_t*)mount;
    if (m == NULL || !m->ready || !m->writable) return 0;
    if (name == NULL || name[0] == '\0' || strlen(name) > 255) return 0;

    uint32_t ino = m_alloc_inode(m);
    if (ino == 0) return 0;

    ext2_inode_t in;
    memset(&in, 0, sizeof(in));
    in.mode = 0x81A4;   /* S_IFREG | 0644 */
    in.size = 0;
    if (m_write_inode(m, ino, &in) != 0) return 0;
    if (m_add_dirent(m, dir_ino, name, ino, EXT2_FT_REG_FILE) != 0) return 0;
    return ino;
}

uint32_t ext2_create_dir(void* mount, uint32_t dir_ino, const char* name) {
    ext2_mount_t* m = (ext2_mount_t*)mount;
    if (m == NULL || !m->ready || !m->writable) return 0;
    if (name == NULL || name[0] == '\0' || strlen(name) > 255) return 0;

    uint32_t ino = m_alloc_inode(m);
    if (ino == 0) return 0;
    uint32_t blk = m_alloc_block(m);
    if (blk == 0) return 0;

    uint8_t* dbuf = (uint8_t*)kmalloc(m->block_size);
    if (dbuf == NULL) return 0;
    memset(dbuf, 0, m->block_size);
    dirent_put(dbuf, ino, 12, ".", EXT2_FT_DIR);
    dirent_put(dbuf + 12, dir_ino, (uint16_t)(m->block_size - 12), "..", EXT2_FT_DIR);
    int w = m_write_block(m, blk, dbuf);
    kfree(dbuf);
    if (w != 0) return 0;

    ext2_inode_t in;
    memset(&in, 0, sizeof(in));
    in.mode = 0x41ED;   /* S_IFDIR | 0755 */
    in.size = m->block_size;
    in.block[0] = blk;
    if (m_write_inode(m, ino, &in) != 0) return 0;
    if (m_add_dirent(m, dir_ino, name, ino, EXT2_FT_DIR) != 0) return 0;
    return ino;
}

/* ---- mkfs ------------------------------------------------------------------
 * Layout (all in 1 KiB blocks): block 0 boot, block 1 superblock, blocks
 * 2..(2+gdt_blocks-1) group descriptor table, then ONE contiguous metadata
 * run of 258 blocks per group (block bitmap, inode bitmap, 256-block inode
 * table). Data starts at meta_end. Groups are only used for bitmaps - the
 * metadata run is global, so bit 0 of group 0 covers block 1 and every
 * block below meta_end is marked used in whichever group's bitmap covers it. */
int ext2_mkfs(const char* devname, int force) {
    if (devname == NULL) return -1;
    blkdev_t* dev = blkdev_find(devname);
    if (dev == NULL) {
        klog("[mkfs] no such device: ");
        klog(devname);
        klog("\n");
        return -1;
    }
    if (!force && root_mnt.ready && root_mnt.dev == dev) {
        klog("[mkfs] refused: device hosts the root filesystem\n");
        return -1;
    }
    for (int i = 0; i < mnt_count; i++) {
        if (mnt_table[i]->dev == dev) {
            klog("[mkfs] refused: device is mounted\n");
            return -1;
        }
    }
    if (dev->sector_count < 16384) {
        klog("[mkfs] device too small (min 8 MiB)\n");
        return -1;
    }

    static ext2_mount_t mk;   /* scratch instance (BSS, not on the heap) */
    memset(&mk, 0, sizeof(mk));
    mk.dev = dev;
    mk.ready = 1;
    mk.block_size = MKFS_BLOCK_SIZE;
    mk.sectors_per_block = 2;
    mk.first_data_block = 1;
    mk.blocks_per_group = MKFS_BLOCKS_PER_GROUP;
    mk.inodes_per_group = MKFS_INODES_PER_GROUP;
    mk.inode_size = MKFS_INODE_SIZE;
    mk.rev_level = 1;
    mk.blocks_count = dev->sector_count / 2;
    mk.ngroups = (int)((mk.blocks_count + MKFS_BLOCKS_PER_GROUP - 1)
                     / MKFS_BLOCKS_PER_GROUP);
    if (mk.ngroups > EXT2_MAX_GROUPS) {
        klog("[mkfs] device too large (max 2 GiB)\n");
        return -1;
    }
    mk.inodes_count = (uint32_t)mk.ngroups * MKFS_INODES_PER_GROUP;

    uint32_t gdt_blocks = (mk.ngroups * 32 + MKFS_BLOCK_SIZE - 1) / MKFS_BLOCK_SIZE;
    uint32_t meta_end = 2 + gdt_blocks + (uint32_t)mk.ngroups * 258;
    if (meta_end + 2 >= mk.blocks_count) {
        klog("[mkfs] metadata does not fit\n");
        return -1;
    }

    /* per-group metadata block numbers */
    for (int g = 0; g < mk.ngroups; g++) {
        uint32_t base = 2 + gdt_blocks + (uint32_t)g * 258;
        mk.bg_block_bitmap[g] = base;
        mk.bg_inode_bitmap[g] = base + 1;
        mk.bg_inode_table[g] = base + 2;
    }

    uint8_t* zb = (uint8_t*)kmalloc(MKFS_BLOCK_SIZE);
    uint8_t* bm = (uint8_t*)kmalloc(MKFS_BLOCK_SIZE);
    uint8_t* gd = (uint8_t*)kmalloc(MKFS_BLOCK_SIZE);
    uint8_t* tb = (uint8_t*)kmalloc(MKFS_BLOCK_SIZE);
    uint8_t* sb = (uint8_t*)kmalloc(MKFS_BLOCK_SIZE);
    if (zb == NULL || bm == NULL || gd == NULL || tb == NULL || sb == NULL) {
        if (zb) kfree(zb);
        if (bm) kfree(bm);
        if (gd) kfree(gd);
        if (tb) kfree(tb);
        if (sb) kfree(sb);
        return -1;
    }

    int rc = -1;
    do {
        /* zero all metadata blocks (superblock gets rewritten below; data
         * area keeps whatever was there - free blocks need no zeroing) */
        memset(zb, 0, MKFS_BLOCK_SIZE);
        for (uint32_t b = 1; b < meta_end; b++) {
            kernel_heartbeat_touch();   /* kwatchdog: large disks zero slowly */
            if (m_write_block(&mk, b, zb) != 0) break;
        }

        /* group descriptor table */
        for (int g = 0; g < mk.ngroups; g++) {
            uint32_t gd_index = (uint32_t)g * 32;
            uint32_t blk = 2 + gd_index / MKFS_BLOCK_SIZE;
            uint32_t off = gd_index % MKFS_BLOCK_SIZE;
            *(uint32_t*)(gd + off + GD_OFF_BLOCK_BITMAP) = mk.bg_block_bitmap[g];
            *(uint32_t*)(gd + off + GD_OFF_INODE_BITMAP) = mk.bg_inode_bitmap[g];
            *(uint32_t*)(gd + off + GD_OFF_INODE_TABLE) = mk.bg_inode_table[g];
            if (g + 1 == mk.ngroups ||
                (((uint32_t)(g + 1)) * 32) % MKFS_BLOCK_SIZE == 0) {
                if (m_write_block(&mk, blk, gd) != 0) goto out;
                if (g + 1 < mk.ngroups) memset(gd, 0, MKFS_BLOCK_SIZE);
            }
        }

        /* block bitmaps: bits below meta_end and beyond blocks_count used */
        for (int g = 0; g < mk.ngroups; g++) {
            memset(bm, 0, MKFS_BLOCK_SIZE);
            for (uint32_t i = 0; i < MKFS_BLOCKS_PER_GROUP; i++) {
                uint32_t b = mk.first_data_block
                           + (uint32_t)g * MKFS_BLOCKS_PER_GROUP + i;
                if (b >= mk.blocks_count) {
                    bm[i / 8] |= (uint8_t)(1u << (i % 8));
                } else if (b < meta_end + 2) {   /* metadata run + root dir + lost+found */
                    bm[i / 8] |= (uint8_t)(1u << (i % 8));
                }
            }
            if (m_write_block(&mk, mk.bg_block_bitmap[g], bm) != 0) goto out;
        }

        /* inode bitmaps: inodes 1..11 reserved (badblocks, root, 3..10
         * reserved, 11 = lost+found) */
        for (int g = 0; g < mk.ngroups; g++) {
            memset(bm, 0, MKFS_BLOCK_SIZE);
            if (g == 0) {
                for (uint32_t i = 0; i < EXT2_LOSTFOUND_INO; i++) {
                    bm[i / 8] |= (uint8_t)(1u << (i % 8));
                }
            }
            if (m_write_block(&mk, mk.bg_inode_bitmap[g], bm) != 0) goto out;
        }

        /* root directory block: ".", "..", "lost+found" */
        memset(zb, 0, MKFS_BLOCK_SIZE);
        dirent_put(zb, EXT2_ROOT_INO, 12, ".", EXT2_FT_DIR);
        dirent_put(zb + 12, EXT2_ROOT_INO, 12, "..", EXT2_FT_DIR);
        dirent_put(zb + 24, EXT2_LOSTFOUND_INO, (uint16_t)(MKFS_BLOCK_SIZE - 24),
                   "lost+found", EXT2_FT_DIR);
        if (m_write_block(&mk, meta_end, zb) != 0) goto out;

        /* lost+found directory block */
        memset(zb, 0, MKFS_BLOCK_SIZE);
        dirent_put(zb, EXT2_LOSTFOUND_INO, 12, ".", EXT2_FT_DIR);
        dirent_put(zb + 12, EXT2_ROOT_INO, (uint16_t)(MKFS_BLOCK_SIZE - 12),
                   "..", EXT2_FT_DIR);
        if (m_write_block(&mk, meta_end + 1, zb) != 0) goto out;

        /* root inode (2) and lost+found inode (11) */
        memset(zb, 0, MKFS_BLOCK_SIZE);
        {
            uint8_t* raw = zb + 128;   /* inode index 1 = inode 2 */
            *(uint16_t*)(raw + INO_OFF_MODE) = 0x41ED;
            *(uint32_t*)(raw + INO_OFF_SIZE) = MKFS_BLOCK_SIZE;
            *(uint32_t*)(raw + INO_OFF_BLOCK) = meta_end;
            if (m_write_block(&mk, mk.bg_inode_table[0], zb) != 0) goto out;

            memset(zb, 0, MKFS_BLOCK_SIZE);
            raw = zb + 256;   /* inode index 10 lives in the table's 2nd block */
            *(uint16_t*)(raw + INO_OFF_MODE) = 0x41ED;
            *(uint32_t*)(raw + INO_OFF_SIZE) = MKFS_BLOCK_SIZE;
            *(uint32_t*)(raw + INO_OFF_BLOCK) = meta_end + 1;
            if (m_write_block(&mk, mk.bg_inode_table[0] + 1, zb) != 0) goto out;
        }

        /* superblock. frags_per_group MUST equal blocks_per_group (the
         * fragment size == block size invariant) and first_ino must be
         * >= 11 on rev1 - e2fsprogs and GRUB both reject the fs without
         * them ("superblock corrupt"), while our own driver never reads
         * these fields, which is why the selftest still passed. */
        memset(sb, 0, MKFS_BLOCK_SIZE);
        *(uint32_t*)(sb + SB_OFF_INODES_COUNT) = mk.inodes_count;
        *(uint32_t*)(sb + SB_OFF_BLOCKS_COUNT) = mk.blocks_count;
        *(uint32_t*)(sb + SB_OFF_FIRST_DATA_BLOCK) = mk.first_data_block;
        *(uint32_t*)(sb + SB_OFF_LOG_BLOCK_SIZE) = 0;   /* 1 KiB */
        *(uint32_t*)(sb + SB_OFF_BLOCKS_PER_GROUP) = mk.blocks_per_group;
        *(uint32_t*)(sb + SB_OFF_FRAGS_PER_GROUP) = mk.blocks_per_group;
        *(uint32_t*)(sb + SB_OFF_INODES_PER_GROUP) = mk.inodes_per_group;
        *(uint16_t*)(sb + SB_OFF_MAGIC) = EXT2_SUPER_MAGIC;
        *(uint16_t*)(sb + SB_OFF_STATE) = 1;            /* clean */
        *(uint32_t*)(sb + SB_OFF_REV_LEVEL) = 1;
        *(uint32_t*)(sb + SB_OFF_FIRST_INO) = 11;
        *(uint16_t*)(sb + SB_OFF_INODE_SIZE) = MKFS_INODE_SIZE;
        if (m_write_block(&mk, 1, sb) != 0) goto out;

        klog("[mkfs] ext2 on ");
        klog(devname);
        klog(": ");
        char num[16];
        itoa((int)mk.blocks_count, num, 10, sizeof(num));
        klog(num);
        klog(" blocks, ");
        itoa(mk.ngroups, num, 10, sizeof(num));
        klog(num);
        klog(" groups\n");
        rc = 0;
    } while (0);
out:
    kfree(zb);
    kfree(bm);
    kfree(gd);
    kfree(tb);
    kfree(sb);
    return rc;
}

/* ---- mount registry / graft ------------------------------------------------ */

static int subtree_has_backend(fs_entry_t* dir, fs_backend_t be) {
    if (dir->backend == be) return 1;
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir->children[i] != NULL && subtree_has_backend(dir->children[i], be)) {
            return 1;
        }
    }
    return 0;
}

static int subtree_has_node(fs_entry_t* dir, fs_entry_t* node) {
    if (dir == node) return 1;
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir->children[i] != NULL && subtree_has_node(dir->children[i], node)) {
            return 1;
        }
    }
    return 0;
}

int ext2_mount_is_writable(void* mount) {
    ext2_mount_t* m = (ext2_mount_t*)mount;
    return (m != NULL && m->ready) ? m->writable : 0;
}

void* ext2_mount(const char* devname, const char* mountpoint, int writable) {
    if (devname == NULL || mountpoint == NULL) return NULL;
    if (mnt_count >= EXT2_MAX_MOUNTS) {
        klog("[mnt] mount table full\n");
        return NULL;
    }
    blkdev_t* dev = blkdev_find(devname);
    if (dev == NULL) {
        klog("[mnt] no such device: ");
        klog(devname);
        klog("\n");
        return NULL;
    }
    /* Mounting over the root device is refused only for the selftest; a
     * writable remount after ext2_mkfs is exactly how kilinstall's update
     * path writes the fresh filesystem back (the old tree is fully cached
     * in MEM overlays by then), so this is a warning, not an error. */
    if (root_mnt.ready && root_mnt.dev == dev) {
        klog("[mnt] warning: device hosts the root filesystem\n");
    }
    for (int i = 0; i < mnt_count; i++) {
        if (mnt_table[i]->dev == dev) {
            klog("[mnt] device already mounted\n");
            return NULL;
        }
    }

    fs_entry_t* target = fs_resolve_path(mountpoint);
    if (target == NULL || target->type != FS_TYPE_DIRECTORY) {
        klog("[mnt] mountpoint not a directory\n");
        return NULL;
    }
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (target->children[i] != NULL) {
            klog("[mnt] mountpoint not empty\n");
            return NULL;
        }
    }

    ext2_mount_t* m = (ext2_mount_t*)kmalloc(sizeof(ext2_mount_t));
    if (m == NULL) return NULL;
    memset(m, 0, sizeof(ext2_mount_t));
    m->dev = dev;
    m->writable = writable;
    strncpy(m->devname, devname, sizeof(m->devname) - 1);
    strncpy(m->mountpoint, mountpoint, sizeof(m->mountpoint) - 1);

    if (!ext2_probe_m(m)) {
        klog("[mnt] no ext2 filesystem on ");
        klog(devname);
        klog("\n");
        kfree(m);
        return NULL;
    }

    /* build the disk subtree and graft its children under the mountpoint */
    fs_entry_t* sub = ext2_build_tree_m(m);
    if (sub == NULL) {
        kfree(m);
        return NULL;
    }
    int idx = 0;
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        fs_entry_t* c = sub->children[i];
        if (c == NULL) continue;
        sub->children[i] = NULL;
        c->parent = target;
        if (idx < MAX_DIR_ENTRIES) target->children[idx++] = c;
    }
    kfree(sub);

    /* The graft point must advertise itself as an ext2 directory with the
     * root inode: fs_create_file/fs_create_dir only take the real on-disk
     * path when parent->backend == FS_BACKEND_EXT2 (a plain MEM mountpoint
     * would silently route every new file into a RAM overlay). */
    m->prev_backend = (int)target->backend;
    target->mnt = m;
    target->backend = FS_BACKEND_EXT2;
    target->inode_no = EXT2_ROOT_INO;
    m->target = target;
    mnt_table[mnt_count++] = m;

    klog("[mnt] ");
    klog(devname);
    klog(" on ");
    klog(mountpoint);
    klog(writable ? " (rw)\n" : " (ro)\n");
    return m;
}

int ext2_umount(const char* mountpoint) {
    if (mountpoint == NULL) return -1;
    int found = -1;
    for (int i = 0; i < mnt_count; i++) {
        if (strcmp(mnt_table[i]->mountpoint, mountpoint) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        klog("[mnt] not a mountpoint\n");
        return -1;
    }
    ext2_mount_t* m = mnt_table[found];
    fs_entry_t* target = m->target;

    /* cwd inside the subtree would dangle */
    for (fs_entry_t* e = fs_current(); e != NULL; e = e->parent) {
        if (e == target) {
            klog("[mnt] busy: cwd inside mount\n");
            return -1;
        }
    }
    /* overlay nodes would be lost */
    if (subtree_has_backend(target, FS_BACKEND_MEM)) {
        klog("[mnt] busy: overlay files present\n");
        return -1;
    }
    /* a nested mount's graft point would dangle */
    for (int j = 0; j < mnt_count; j++) {
        if (j != found && mnt_table[j]->target != NULL &&
            subtree_has_node(target, mnt_table[j]->target)) {
            klog("[mnt] busy: nested mount below\n");
            return -1;
        }
    }

    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (target->children[i] != NULL) {
            fs_delete_entry_recursive(target->children[i]);
            target->children[i] = NULL;
        }
    }
    target->mnt = NULL;
    target->backend = (fs_backend_t)m->prev_backend;
    target->inode_no = 0;
    mnt_table[found] = mnt_table[--mnt_count];
    mnt_table[mnt_count] = NULL;
    kfree(m);
    klog("[mnt] umounted ");
    klog(mountpoint);
    klog("\n");
    return 0;
}

int ext2_mount_count(void) {
    return mnt_count;
}

const char* ext2_mount_devname(int i) {
    return (i >= 0 && i < mnt_count) ? mnt_table[i]->devname : NULL;
}

const char* ext2_mount_path(int i) {
    return (i >= 0 && i < mnt_count) ? mnt_table[i]->mountpoint : NULL;
}

int ext2_mount_is_rw(int i) {
    return (i >= 0 && i < mnt_count) ? mnt_table[i]->writable : 0;
}

/* ---- selftest ---------------------------------------------------------------
 * mkfs -> mount -> write/read-back a 300 KiB pattern (12 direct + 256 singly
 * indirect + 32 doubly indirect blocks) + a small file -> umount. Runs on
 * sd0p1 when it exists, else on raw sd0 (test/Live disks; the boot root is
 * explicitly refused). */
int ext2_selftest(void) {
    const char* devname = NULL;
    blkdev_t* dev = blkdev_find("sd0p1");
    if (dev == NULL) dev = blkdev_find("sd0");
    if (dev == NULL) {
        klog("[mnt] selftest: no sd device\n");
        return -1;
    }
    if (root_mnt.ready && root_mnt.dev == dev) {
        klog("[mnt] selftest: refused, device hosts the root filesystem\n");
        return -1;
    }
    devname = dev->name;

    if (ext2_mkfs(devname, 0) != 0) return -1;

    /* mountpoint: create if missing, must be empty */
    fs_entry_t* mp = fs_resolve_path("/mnt_t");
    if (mp == NULL) mp = fs_create_dir("/mnt_t");
    if (mp == NULL || mp->type != FS_TYPE_DIRECTORY) {
        klog("[mnt] selftest: cannot create /mnt_t\n");
        return -1;
    }
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (mp->children[i] != NULL) {
            klog("[mnt] selftest: /mnt_t not empty\n");
            return -1;
        }
    }

    if (ext2_mount(devname, "/mnt_t", 1) == NULL) return -1;

    int rc = -1;
    uint8_t* wbuf = (uint8_t*)kmalloc(300 * 1024);
    uint8_t* rbuf = (uint8_t*)kmalloc(300 * 1024);
    if (wbuf != NULL && rbuf != NULL) {
        for (size_t i = 0; i < 300 * 1024; i++) {
            wbuf[i] = (uint8_t)(i * 7 + 3);
        }
        fs_entry_t* f = fs_create_file("/mnt_t/big.bin");
        int w = (f != NULL) ? fs_write_file(f, wbuf, 300 * 1024) : -1;
        int r = (w == 300 * 1024) ? fs_read_file(f, rbuf, 300 * 1024) : -1;
        if (r == 300 * 1024 && memcmp(wbuf, rbuf, 300 * 1024) == 0) {
            fs_entry_t* h = fs_create_file("/mnt_t/hello.txt");
            const char* msg = "mnt selftest";
            int w2 = (h != NULL) ? fs_write_file(h, (const uint8_t*)msg, strlen(msg)) : -1;
            uint8_t back[32];
            int r2 = (w2 >= 0) ? fs_read_file(h, back, sizeof(back)) : -1;
            if (r2 == (int)strlen(msg) && memcmp(back, msg, strlen(msg)) == 0) {
                klog("[mnt] MNT_SELFTEST_OK\n");
                rc = 0;
            }
        }
    }
    if (wbuf != NULL) kfree(wbuf);
    if (rbuf != NULL) kfree(rbuf);

    ext2_umount("/mnt_t");
    fs_delete_entry("/mnt_t");

    if (rc != 0) klog("[mnt] MNT_SELFTEST_FAILED\n");
    return rc;
}
