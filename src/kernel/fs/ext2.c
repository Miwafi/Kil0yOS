#include "fs/ext2.h"
#include "fs/fs.h"
#include "drivers/disk.h"
#include "drivers/vga.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "lib/stdlib.h"

/* ---- on-disk constants ---------------------------------------------- */

#define EXT2_SUPER_MAGIC   0xEF53
#define EXT2_ROOT_INO      2

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
#define SB_OFF_INODES_PER_GROUP 40
#define SB_OFF_MAGIC            56
#define SB_OFF_REV_LEVEL        76
#define SB_OFF_INODE_SIZE       88

/* group descriptor offsets (32 bytes each) */
#define GD_OFF_INODE_TABLE      8

/* inode field offsets */
#define INO_OFF_MODE            0
#define INO_OFF_SIZE            4
#define INO_OFF_BLOCK           40   /* i_block[15], 60 bytes */

/* ---- cached layout --------------------------------------------------- */

static struct {
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
} sb;

static int sb_ready = 0;

/* ---- low-level helpers ------------------------------------------------ */

static int ext2_read_block(uint32_t block, uint8_t* buffer) {
    uint32_t first_sector = block * sb.sectors_per_block;
    for (uint32_t s = 0; s < sb.sectors_per_block; s++) {
        if (disk_read_sector(first_sector + s, buffer + s * DISK_SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Resolve logical block `bn` of an inode to a physical block number.
 * Supports direct blocks and all three indirection levels. `ibuf` must be
 * a scratch buffer of sb.block_size bytes (used for indirect blocks). */
static uint32_t ext2_bmap(const uint32_t i_block[15], uint32_t bn, uint8_t* ibuf) {
    uint32_t ppb = sb.block_size / 4; /* pointers per block */

    if (bn < 12) return i_block[bn];
    bn -= 12;

    if (bn < ppb) { /* singly indirect */
        uint32_t ind = i_block[12];
        if (ind == 0) return 0;
        if (ext2_read_block(ind, ibuf) != 0) return 0;
        return *(uint32_t*)(ibuf + bn * 4);
    }
    bn -= ppb;

    if (bn < ppb * ppb) { /* doubly indirect */
        uint32_t dind = i_block[13];
        if (dind == 0) return 0;
        if (ext2_read_block(dind, ibuf) != 0) return 0;
        uint32_t l1 = *(uint32_t*)(ibuf + (bn / ppb) * 4);
        if (l1 == 0) return 0;
        if (ext2_read_block(l1, ibuf) != 0) return 0;
        return *(uint32_t*)(ibuf + (bn % ppb) * 4);
    }
    bn -= ppb * ppb;

    /* triply indirect */
    uint32_t tind = i_block[14];
    if (tind == 0) return 0;
    if (ext2_read_block(tind, ibuf) != 0) return 0;
    uint32_t l1 = *(uint32_t*)(ibuf + (bn / (ppb * ppb)) * 4);
    if (l1 == 0) return 0;
    if (ext2_read_block(l1, ibuf) != 0) return 0;
    uint32_t l2 = *(uint32_t*)(ibuf + ((bn / ppb) % ppb) * 4);
    if (l2 == 0) return 0;
    if (ext2_read_block(l2, ibuf) != 0) return 0;
    return *(uint32_t*)(ibuf + (bn % ppb) * 4);
}

typedef struct {
    uint16_t mode;
    uint32_t size;
    uint32_t block[15];
} ext2_inode_t;

static int ext2_read_inode(uint32_t ino, ext2_inode_t* out) {
    if (!sb_ready || ino < 1 || ino > sb.inodes_count) return -1;

    uint32_t group = (ino - 1) / sb.inodes_per_group;
    uint32_t index = (ino - 1) % sb.inodes_per_group;

    /* group descriptor for `group` */
    uint32_t gd_index = group * 32;
    uint32_t gd_block = sb.gd_block + gd_index / sb.block_size;
    uint32_t gd_off = gd_index % sb.block_size;

    uint8_t* buf = (uint8_t*)kmalloc(sb.block_size);
    if (buf == NULL) return -1;

    if (ext2_read_block(gd_block, buf) != 0) {
        kfree(buf);
        return -1;
    }
    uint32_t inode_table = *(uint32_t*)(buf + gd_off + GD_OFF_INODE_TABLE);

    /* inode table entry (may be sb.inode_size > 128; we only need the
     * first 100 bytes, all within the containing block for 128/256 sizes) */
    uint32_t byte_off = index * sb.inode_size;
    uint32_t inode_block = inode_table + byte_off / sb.block_size;
    uint32_t in_block_off = byte_off % sb.block_size;

    if (ext2_read_block(inode_block, buf) != 0) {
        kfree(buf);
        return -1;
    }

    uint8_t* raw = buf + in_block_off;
    out->mode = *(uint16_t*)(raw + INO_OFF_MODE);
    out->size = *(uint32_t*)(raw + INO_OFF_SIZE);
    memcpy(out->block, raw + INO_OFF_BLOCK, 15 * sizeof(uint32_t));

    kfree(buf);
    return 0;
}

/* ---- probe & tree build ----------------------------------------------- */

int ext2_probe(void) {
    uint8_t buf[DISK_SECTOR_SIZE];
    /* superblock lives at byte 1024 = sector 2 */
    if (disk_read_sector(2, buf) != 0) return 0;

    uint16_t magic = *(uint16_t*)(buf + SB_OFF_MAGIC);
    if (magic != EXT2_SUPER_MAGIC) return 0;

    memset(&sb, 0, sizeof(sb));
    sb.inodes_count      = *(uint32_t*)(buf + SB_OFF_INODES_COUNT);
    sb.blocks_count      = *(uint32_t*)(buf + SB_OFF_BLOCKS_COUNT);
    sb.first_data_block  = *(uint32_t*)(buf + SB_OFF_FIRST_DATA_BLOCK);
    uint32_t log_bs      = *(uint32_t*)(buf + SB_OFF_LOG_BLOCK_SIZE);
    sb.blocks_per_group  = *(uint32_t*)(buf + SB_OFF_BLOCKS_PER_GROUP);
    sb.inodes_per_group  = *(uint32_t*)(buf + SB_OFF_INODES_PER_GROUP);
    sb.rev_level         = *(uint32_t*)(buf + SB_OFF_REV_LEVEL);

    if (log_bs > 6) return 0; /* block size up to 64 KiB - sanity limit */
    sb.block_size = 1024u << log_bs;
    sb.sectors_per_block = sb.block_size / DISK_SECTOR_SIZE;
    sb.inode_size = (sb.rev_level >= 1)
        ? *(uint16_t*)(buf + SB_OFF_INODE_SIZE) : 128;
    if (sb.inode_size < 128) return 0;

    /* group descriptor table: block first_data_block + 1 (1KiB blocks) or
     * block 1 (>= 2KiB blocks, where first_data_block is 0) */
    sb.gd_block = sb.first_data_block + 1;
    sb_ready = 1;

    klog("[ext2] probe ok: ");
    char num[16];
    itoa((int)sb.blocks_count, num, 10, sizeof(num));
    klog(num);
    klog(" blocks, block_size=");
    itoa((int)sb.block_size, num, 10, sizeof(num));
    klog(num);
    klog(", inode_size=");
    itoa((int)sb.inode_size, num, 10, sizeof(num));
    klog(num);
    klog("\n");
    return 1;
}

static fs_entry_t* ext2_new_node(const char* name, fs_entry_type_t type,
                                 uint32_t ino, uint32_t size,
                                 fs_entry_t* parent) {
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
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) e->children[i] = NULL;
    return e;
}

static void ext2_load_dir_recursive(fs_entry_t* dir) {
    ext2_inode_t inode;
    if (ext2_read_inode(dir->inode_no, &inode) != 0) return;
    if (!(inode.mode & EXT2_S_IFDIR)) return;

    uint32_t nblocks = (inode.size + sb.block_size - 1) / sb.block_size;
    uint8_t* bbuf = (uint8_t*)kmalloc(sb.block_size);
    uint8_t* ibuf = (uint8_t*)kmalloc(sb.block_size);
    if (bbuf == NULL || ibuf == NULL) {
        if (bbuf) kfree(bbuf);
        if (ibuf) kfree(ibuf);
        return;
    }

    int child_idx = 0;
    for (uint32_t b = 0; b < nblocks && child_idx < MAX_DIR_ENTRIES; b++) {
        uint32_t phys = ext2_bmap(inode.block, b, ibuf);
        if (phys == 0) continue;
        if (ext2_read_block(phys, bbuf) != 0) continue;

        /* directory entries never span blocks (ext2 spec) */
        uint32_t off = 0;
        while (off + 8 <= sb.block_size && child_idx < MAX_DIR_ENTRIES) {
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
                        if (ext2_read_inode(ino, &ci) != 0) { off += rec_len; continue; }
                        type = (ci.mode & EXT2_S_IFDIR) ? FS_TYPE_DIRECTORY
                                                        : FS_TYPE_FILE;
                    }

                    uint32_t size = 0;
                    if (type == FS_TYPE_FILE) {
                        ext2_inode_t ci;
                        if (ext2_read_inode(ino, &ci) == 0) size = ci.size;
                    }

                    fs_entry_t* child = ext2_new_node(name, type, ino, size, dir);
                    if (child != NULL) {
                        dir->children[child_idx++] = child;
                        if (type == FS_TYPE_DIRECTORY) {
                            ext2_load_dir_recursive(child);
                        }
                    }
                }
            }
            off += rec_len;
        }
    }

    kfree(bbuf);
    kfree(ibuf);
}

fs_entry_t* ext2_build_tree(void) {
    fs_entry_t* root = ext2_new_node("/", FS_TYPE_DIRECTORY, EXT2_ROOT_INO, 0, NULL);
    if (root == NULL) return NULL;

    ext2_load_dir_recursive(root);

    if (root->children[0] == NULL) {
        klog("[ext2] warning: root directory is empty\n");
    }
    return root;
}

/* ---- file read --------------------------------------------------------- */

int ext2_read_file(uint32_t ino, uint8_t* buffer, size_t size) {
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -1;
    if (size > inode.size) size = inode.size;

    uint8_t* bbuf = (uint8_t*)kmalloc(sb.block_size);
    uint8_t* ibuf = (uint8_t*)kmalloc(sb.block_size);
    if (bbuf == NULL || ibuf == NULL) {
        if (bbuf) kfree(bbuf);
        if (ibuf) kfree(ibuf);
        return -1;
    }

    size_t offset = 0;
    uint32_t nblocks = ((uint32_t)size + sb.block_size - 1) / sb.block_size;
    for (uint32_t b = 0; b < nblocks && offset < size; b++) {
        uint32_t phys = ext2_bmap(inode.block, b, ibuf);
        if (phys == 0) break;
        if (ext2_read_block(phys, bbuf) != 0) break;

        size_t copy = sb.block_size;
        if (offset + copy > size) copy = size - offset;
        memcpy(buffer + offset, bbuf, copy);
        offset += copy;
    }

    kfree(bbuf);
    kfree(ibuf);
    return (int)offset;
}
