#ifndef FS_H
#define FS_H

#include "types.h"

#define MAX_PATH_LENGTH 256
#define MAX_FILES       64
#define MAX_DIR_ENTRIES 32
#define MAX_FILE_SIZE   4096

#define FS_ERR_NONE     0
#define FS_ERR_EXISTS   -1
#define FS_ERR_FULL     -2
#define FS_ERR_INVALID  -3

typedef enum {
    FS_TYPE_FILE,
    FS_TYPE_DIRECTORY
} fs_entry_type_t;

typedef struct fs_entry {
    char name[64];
    fs_entry_type_t type;
    uint32_t size;
    uint32_t permissions;
    struct fs_entry* parent;
    struct fs_entry* children[MAX_DIR_ENTRIES];
    uint8_t data[];
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

#endif