#ifndef FS_H
#define FS_H

#include "lib/types.h"

#define MAX_PATH_LENGTH 260
#define MAX_DIR_ENTRIES 32
#define MAX_FILE_SIZE   (4096 * 1024)

#define FS_ERR_NONE     0
#define FS_ERR_EXISTS   -1
#define FS_ERR_FULL     -2
#define FS_ERR_INVALID  -3
#define FS_ERR_NOT_FOUND -4
#define FS_ERR_IO       -5

typedef enum {
    FS_TYPE_FILE,
    FS_TYPE_DIRECTORY
} fs_entry_type_t;

typedef struct {
    uint8_t jump[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media_type;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} __attribute__((packed)) fat32_bpb_t;

typedef struct {
    fat32_bpb_t bpb;
    uint32_t sectors_per_fat_32;
    uint16_t extended_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
    uint8_t boot_code[420];
    uint16_t boot_signature_word;
} __attribute__((packed)) fat32_boot_sector_t;

typedef struct {
    uint32_t free_count;
    uint32_t next_free;
    uint8_t reserved[480];
} __attribute__((packed)) fat32_fsinfo_t;

typedef struct {
    uint8_t name[11];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t last_modified_time;
    uint16_t last_modified_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat32_dir_entry_t;

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LONG_NAME  0x0F

#define FAT32_EOC_MARK  0x0FFFFFFF
#define FAT32_BAD_CLUSTER 0x0FFFFFF7

typedef struct fs_entry {
    char name[256];
    fs_entry_type_t type;
    uint32_t size;
    uint32_t first_cluster;
    uint32_t attributes;
    struct fs_entry* parent;
    struct fs_entry* children[MAX_DIR_ENTRIES];
} fs_entry_t;

void fs_init();
fs_entry_t* fs_root();
fs_entry_t* fs_current();
void fs_set_current(fs_entry_t* dir);
fs_entry_t* fs_resolve_path(const char* path);
fs_entry_t* fs_create_file(const char* name);
fs_entry_t* fs_create_dir(const char* name);
int fs_delete_entry(const char* name);
void fs_delete_entry_recursive(fs_entry_t* entry);
int fs_write_file(fs_entry_t* file, const uint8_t* data, size_t size);
int fs_read_file(fs_entry_t* file, uint8_t* buffer, size_t size);
int fs_get_last_error();
void fs_save();

#endif
