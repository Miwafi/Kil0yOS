#include "fs.h"
#include "memory.h"
#include "string.h"
#include "disk.h"

#define FS_MAGIC 0x4B494C4F
#define FS_VERSION 1

#define FS_SUPERBLOCK_SECTOR 0
#define FS_ENTRY_START_SECTOR 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t free_sector;
} fs_superblock_t;

typedef struct {
    char name[64];
    uint32_t type;
    uint32_t size;
    uint32_t permissions;
    uint32_t parent_idx;
    uint32_t children[MAX_DIR_ENTRIES];
    uint8_t data[MAX_FILE_SIZE];
} fs_disk_entry_t;

static fs_entry_t* root;
static fs_entry_t* current;

static fs_entry_t* fs_create_entry_from_disk(const fs_disk_entry_t* disk_entry, fs_entry_t* parent) {
    fs_entry_t* entry = (fs_entry_t*)kmalloc(sizeof(fs_entry_t) + ((disk_entry->type == FS_TYPE_FILE) ? MAX_FILE_SIZE : 0));
    strcpy(entry->name, disk_entry->name);
    entry->type = disk_entry->type;
    entry->size = disk_entry->size;
    entry->permissions = disk_entry->permissions;
    entry->parent = parent;
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        entry->children[i] = NULL;
    }
    
    if (entry->type == FS_TYPE_FILE && entry->size > 0) {
        memcpy(entry->data, disk_entry->data, entry->size);
    }
    
    return entry;
}

static void fs_count_entries(fs_entry_t* entry, int* count) {
    if (entry == NULL) return;
    (*count)++;
    
    if (entry->type == FS_TYPE_DIRECTORY) {
        for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
            fs_count_entries(entry->children[i], count);
        }
    }
}

static void fs_build_index_map(fs_entry_t* entry, fs_entry_t** map, int* idx) {
    if (entry == NULL) return;
    map[*idx] = entry;
    (*idx)++;
    
    if (entry->type == FS_TYPE_DIRECTORY) {
        for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
            fs_build_index_map(entry->children[i], map, idx);
        }
    }
}

void fs_save() {
    int entry_count = 0;
    fs_count_entries(root, &entry_count);
    
    if (entry_count == 0) return;
    
    int total_sectors = 1 + ((entry_count * sizeof(fs_disk_entry_t)) + DISK_SECTOR_SIZE - 1) / DISK_SECTOR_SIZE;
    if (total_sectors > DISK_MAX_SECTORS) return;
    
    uint8_t* buffer = (uint8_t*)kmalloc(total_sectors * DISK_SECTOR_SIZE);
    if (buffer == NULL) return;
    
    fs_entry_t** index_map = (fs_entry_t**)kmalloc(entry_count * sizeof(fs_entry_t*));
    if (index_map == NULL) {
        kfree(buffer);
        return;
    }
    
    int idx = 0;
    fs_build_index_map(root, index_map, &idx);
    
    fs_superblock_t* sb = (fs_superblock_t*)buffer;
    sb->magic = FS_MAGIC;
    sb->version = FS_VERSION;
    sb->entry_count = entry_count;
    sb->free_sector = total_sectors;
    
    fs_disk_entry_t* entries = (fs_disk_entry_t*)(buffer + DISK_SECTOR_SIZE);
    
    for (int i = 0; i < entry_count; i++) {
        fs_entry_t* entry = index_map[i];
        fs_disk_entry_t* disk_entry = &entries[i];
        
        strcpy(disk_entry->name, entry->name);
        disk_entry->type = entry->type;
        disk_entry->size = entry->size;
        disk_entry->permissions = entry->permissions;
        
        if (entry->parent == NULL) {
            disk_entry->parent_idx = 0xFFFFFFFF;
        } else {
            for (int j = 0; j < entry_count; j++) {
                if (index_map[j] == entry->parent) {
                    disk_entry->parent_idx = j;
                    break;
                }
            }
        }
        
        for (int j = 0; j < MAX_DIR_ENTRIES; j++) {
            if (entry->children[j] != NULL) {
                for (int k = 0; k < entry_count; k++) {
                    if (index_map[k] == entry->children[j]) {
                        disk_entry->children[j] = k;
                        break;
                    }
                }
            } else {
                disk_entry->children[j] = 0xFFFFFFFF;
            }
        }
        
        if (entry->type == FS_TYPE_FILE) {
            memcpy(disk_entry->data, entry->data, MAX_FILE_SIZE);
        }
    }
    
    for (int i = 0; i < total_sectors; i++) {
        disk_write_sector(i, buffer + i * DISK_SECTOR_SIZE);
    }
    
    kfree(index_map);
    kfree(buffer);
}

int fs_load() {
    uint8_t sb_buffer[DISK_SECTOR_SIZE];
    if (disk_read_sector(FS_SUPERBLOCK_SECTOR, sb_buffer) != 0) {
        return -1;
    }
    
    fs_superblock_t* sb = (fs_superblock_t*)sb_buffer;
    if (sb->magic != FS_MAGIC || sb->version != FS_VERSION) {
        return -1;
    }
    
    int total_sectors = 1 + ((sb->entry_count * sizeof(fs_disk_entry_t)) + DISK_SECTOR_SIZE - 1) / DISK_SECTOR_SIZE;
    uint8_t* buffer = (uint8_t*)kmalloc(total_sectors * DISK_SECTOR_SIZE);
    if (buffer == NULL) return -1;
    
    if (disk_read_sector(FS_SUPERBLOCK_SECTOR, buffer) != 0) {
        kfree(buffer);
        return -1;
    }
    
    for (int i = 1; i < total_sectors; i++) {
        if (disk_read_sector(i, buffer + i * DISK_SECTOR_SIZE) != 0) {
            kfree(buffer);
            return -1;
        }
    }
    
    fs_disk_entry_t* disk_entries = (fs_disk_entry_t*)(buffer + DISK_SECTOR_SIZE);
    
    fs_entry_t** entry_map = (fs_entry_t**)kmalloc(sb->entry_count * sizeof(fs_entry_t*));
    if (entry_map == NULL) {
        kfree(buffer);
        return -1;
    }
    
    for (uint32_t i = 0; i < sb->entry_count; i++) {
        entry_map[i] = fs_create_entry_from_disk(&disk_entries[i], NULL);
    }
    
    root = entry_map[0];
    current = root;
    
    for (uint32_t i = 0; i < sb->entry_count; i++) {
        fs_entry_t* entry = entry_map[i];
        
        if (disk_entries[i].parent_idx != 0xFFFFFFFF) {
            entry->parent = entry_map[disk_entries[i].parent_idx];
        }
        
        for (int j = 0; j < MAX_DIR_ENTRIES; j++) {
            if (disk_entries[i].children[j] != 0xFFFFFFFF) {
                entry->children[j] = entry_map[disk_entries[i].children[j]];
            }
        }
    }
    
    kfree(entry_map);
    kfree(buffer);
    return 0;
}

void fs_init() {
    disk_init();
    
    if (fs_load() == 0) {
        return;
    }
    
    root = (fs_entry_t*)kmalloc(sizeof(fs_entry_t));
    strcpy(root->name, "/");
    root->type = FS_TYPE_DIRECTORY;
    root->size = 0;
    root->permissions = 0755;
    root->parent = NULL;
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        root->children[i] = NULL;
    }
    
    current = root;
    
    fs_entry_t* home = fs_create_dir("home");
    fs_create_dir("bin");
    fs_create_dir("etc");
    fs_create_dir("tmp");
    
    fs_set_current(home);
    fs_create_dir("user");
    fs_set_current(root);
}

fs_entry_t* fs_root() {
    return root;
}

fs_entry_t* fs_current() {
    return current;
}

void fs_set_current(fs_entry_t* dir) {
    if (dir && dir->type == FS_TYPE_DIRECTORY) {
        current = dir;
    }
}

fs_entry_t* fs_resolve_path(const char* path) {
    if (path == NULL || *path == '\0') return current;
    
    fs_entry_t* start = (*path == '/') ? root : current;
    if (*path == '/') path++;
    
    if (*path == '\0') return root;
    
    char* path_copy = (char*)kmalloc(strlen(path) + 1);
    if (path_copy == NULL) return NULL;
    
    strcpy(path_copy, path);
    
    char* token = strtok(path_copy, "/");
    fs_entry_t* current_entry = start;
    
    while (token != NULL) {
        if (strcmp(token, ".") == 0) {
            token = strtok(NULL, "/");
            continue;
        }
        
        if (strcmp(token, "..") == 0) {
            if (current_entry->parent != NULL) {
                current_entry = current_entry->parent;
            }
            token = strtok(NULL, "/");
            continue;
        }
        
        int found = 0;
        for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
            if (current_entry->children[i] != NULL && 
                strcmp(current_entry->children[i]->name, token) == 0) {
                current_entry = current_entry->children[i];
                found = 1;
                break;
            }
        }
        
        if (!found) {
            kfree(path_copy);
            return NULL;
        }
        
        token = strtok(NULL, "/");
    }
    
    kfree(path_copy);
    return current_entry;
}

static int fs_check_entry_exists(fs_entry_t* dir, const char* name) {
    if (dir == NULL || name == NULL) return FS_ERR_INVALID;
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir->children[i] != NULL && 
            strcmp(dir->children[i]->name, name) == 0) {
            return FS_ERR_EXISTS;
        }
    }
    
    return FS_ERR_NONE;
}

static int fs_check_dir_full(fs_entry_t* dir) {
    if (dir == NULL) return FS_ERR_INVALID;
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir->children[i] == NULL) {
            return FS_ERR_NONE;
        }
    }
    
    return FS_ERR_FULL;
}

static int fs_last_error = FS_ERR_NONE;

static fs_entry_t* fs_create_entry(const char* name, fs_entry_type_t type, uint32_t permissions, size_t extra_size) {
    fs_last_error = FS_ERR_NONE;
    
    if (name == NULL || strlen(name) >= 64) {
        fs_last_error = FS_ERR_INVALID;
        return NULL;
    }
    
    int exists = fs_check_entry_exists(current, name);
    if (exists == FS_ERR_EXISTS) {
        fs_last_error = FS_ERR_EXISTS;
        return NULL;
    }
    
    int full = fs_check_dir_full(current);
    if (full == FS_ERR_FULL) {
        fs_last_error = FS_ERR_FULL;
        return NULL;
    }
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (current->children[i] == NULL) {
            fs_entry_t* entry = (fs_entry_t*)kmalloc(sizeof(fs_entry_t) + extra_size);
            strcpy(entry->name, name);
            entry->type = type;
            entry->size = 0;
            entry->permissions = permissions;
            entry->parent = current;
            
            for (int j = 0; j < MAX_DIR_ENTRIES; j++) {
                entry->children[j] = NULL;
            }
            
            if (extra_size > 0) {
                memset(entry->data, 0, extra_size);
            }
            
            current->children[i] = entry;
            return entry;
        }
    }
    
    fs_last_error = FS_ERR_FULL;
    return NULL;
}

int fs_get_last_error() {
    return fs_last_error;
}

fs_entry_t* fs_create_file(const char* name) {
    return fs_create_entry(name, FS_TYPE_FILE, 0644, MAX_FILE_SIZE);
}

int fs_write_file(fs_entry_t* file, const uint8_t* data, size_t size) {
    if (file == NULL || file->type != FS_TYPE_FILE || data == NULL) return -1;
    
    if (size > MAX_FILE_SIZE) {
        size = MAX_FILE_SIZE;
    }
    
    memcpy(file->data, data, size);
    file->size = size;
    
    return size;
}

int fs_read_file(fs_entry_t* file, uint8_t* buffer, size_t size) {
    if (file == NULL || file->type != FS_TYPE_FILE || buffer == NULL) return -1;
    
    if (size > file->size) {
        size = file->size;
    }
    
    memcpy(buffer, file->data, size);
    
    return size;
}

fs_entry_t* fs_create_dir(const char* name) {
    return fs_create_entry(name, FS_TYPE_DIRECTORY, 0755, 0);
}

void fs_delete_entry_recursive(fs_entry_t* entry) {
    if (entry == NULL) return;
    
    if (entry->type == FS_TYPE_DIRECTORY) {
        for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
            if (entry->children[i] != NULL) {
                fs_delete_entry_recursive(entry->children[i]);
                entry->children[i] = NULL;
            }
        }
    }
    
    kfree(entry);
}

int fs_delete_entry(const char* name) {
    if (name == NULL) return -1;
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (current->children[i] != NULL && 
            strcmp(current->children[i]->name, name) == 0) {
            fs_delete_entry_recursive(current->children[i]);
            current->children[i] = NULL;
            return 0;
        }
    }
    
    return -1;
}