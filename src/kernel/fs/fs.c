#include "fs/fs.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "drivers/disk.h"

static fat32_boot_sector_t boot_sector;
static uint8_t* fat_buffer;
static fs_entry_t* root;
static fs_entry_t* current;

static int fs_last_error = FS_ERR_NONE;

static uint32_t get_sectors_per_fat() {
    return boot_sector.sectors_per_fat_32;
}

static uint32_t get_first_fat_sector() {
    return boot_sector.bpb.reserved_sectors;
}

static uint32_t get_first_data_sector() {
    return get_first_fat_sector() + (boot_sector.bpb.fat_count * get_sectors_per_fat());
}

static uint32_t cluster_to_sector(uint32_t cluster) {
    return get_first_data_sector() + (cluster - 2) * boot_sector.bpb.sectors_per_cluster;
}

static uint32_t get_fat_entry_sector(uint32_t cluster) {
    uint32_t entry_offset = cluster * 4;
    return get_first_fat_sector() + (entry_offset / boot_sector.bpb.bytes_per_sector);
}

static uint32_t get_fat_entry_offset(uint32_t cluster) {
    uint32_t entry_offset = cluster * 4;
    return entry_offset % boot_sector.bpb.bytes_per_sector;
}

static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t sector = get_fat_entry_sector(cluster);
    uint32_t offset = get_fat_entry_offset(cluster);
    uint8_t buffer[DISK_SECTOR_SIZE];
    
    if (disk_read_sector(sector, buffer) != 0) {
        return 0;
    }
    
    return *(uint32_t*)(buffer + offset);
}

static int fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t sector = get_fat_entry_sector(cluster);
    uint32_t offset = get_fat_entry_offset(cluster);
    uint8_t buffer[DISK_SECTOR_SIZE];
    
    if (disk_read_sector(sector, buffer) != 0) {
        return -1;
    }
    
    *(uint32_t*)(buffer + offset) = value;
    
    return disk_write_sector(sector, buffer);
}

static uint32_t fat_alloc_cluster() {
    uint32_t data_sectors = DISK_MAX_SECTORS - get_first_data_sector();
    uint32_t max_cluster = 2 + (data_sectors / boot_sector.bpb.sectors_per_cluster);

    for (uint32_t cluster = 2; cluster < max_cluster; cluster++) {
        uint32_t entry = fat_read_entry(cluster);
        if (entry == 0) {
            if (fat_write_entry(cluster, FAT32_EOC_MARK) == 0) {
                /* Update FSInfo sector */
                uint8_t buffer[DISK_SECTOR_SIZE];
                if (disk_read_sector(boot_sector.fs_info_sector, buffer) == 0) {
                    fat32_fsinfo_t* fsinfo = (fat32_fsinfo_t*)buffer;
                    fsinfo->next_free = cluster;
                    if (fsinfo->free_count == 0xFFFFFFFF) {
                        /* Count free clusters on first allocation */
                        uint32_t free_count = 0;
                        for (uint32_t c = 2; c < max_cluster; c++) {
                            if (fat_read_entry(c) == 0) free_count++;
                        }
                        fsinfo->free_count = free_count;
                    } else if (fsinfo->free_count > 0) {
                        fsinfo->free_count--;
                    }
                    disk_write_sector(boot_sector.fs_info_sector, buffer);
                }
                return cluster;
            }
        }
    }
    return 0;
}

static int fat_free_cluster_chain(uint32_t first_cluster) {
    uint32_t cluster = first_cluster;
    uint32_t freed_count = 0;

    while (cluster != 0 && cluster != FAT32_EOC_MARK) {
        uint32_t next = fat_read_entry(cluster);
        if (fat_write_entry(cluster, 0) != 0) {
            return -1;
        }
        freed_count++;
        cluster = next;
    }

    /* Update FSInfo sector */
    if (freed_count > 0) {
        uint8_t buffer[DISK_SECTOR_SIZE];
        if (disk_read_sector(boot_sector.fs_info_sector, buffer) == 0) {
            fat32_fsinfo_t* fsinfo = (fat32_fsinfo_t*)buffer;
            if (fsinfo->free_count != 0xFFFFFFFF) {
                fsinfo->free_count += freed_count;
            }
            /* Reset next_free to first cluster (will be updated on next allocation) */
            fsinfo->next_free = 2;
            disk_write_sector(boot_sector.fs_info_sector, buffer);
        }
    }

    return 0;
}

static void parse_short_name(const uint8_t* name_11, char* output) {
    int i, j = 0;
    
    for (i = 0; i < 8; i++) {
        if (name_11[i] == ' ') break;
        output[j++] = name_11[i];
    }
    
    if (name_11[8] != ' ') {
        output[j++] = '.';
        for (i = 8; i < 11; i++) {
            if (name_11[i] == ' ') break;
            output[j++] = name_11[i];
        }
    }
    
    output[j] = '\0';
}

static int read_directory_entries(uint32_t cluster, fat32_dir_entry_t** entries, int* count) {
    *count = 0;
    *entries = NULL;
    
    if (cluster == 0) {
        cluster = boot_sector.root_cluster;
    }
    
    uint32_t current_cluster = cluster;
    int total_entries = 0;
    fat32_dir_entry_t* all_entries = NULL;

    /* Pass 1: count valid entries */
    uint32_t scan_cluster = current_cluster;
    while (scan_cluster != 0 && scan_cluster != FAT32_EOC_MARK) {
        uint32_t sector = cluster_to_sector(scan_cluster);
        int sectors_per_cluster = boot_sector.bpb.sectors_per_cluster;
        for (int s = 0; s < sectors_per_cluster; s++) {
            uint8_t buffer[DISK_SECTOR_SIZE];
            if (disk_read_sector(sector + s, buffer) != 0) return -1;
            fat32_dir_entry_t* dir_entries = (fat32_dir_entry_t*)buffer;
            for (int i = 0; i < 16; i++) {
                if (dir_entries[i].name[0] == 0x00) goto done_counting;
                if (dir_entries[i].name[0] == 0xE5) continue;
                if ((dir_entries[i].attributes & ATTR_LONG_NAME) == ATTR_LONG_NAME) continue;
                if (dir_entries[i].attributes == ATTR_VOLUME_ID) continue;
                total_entries++;
            }
        }
        scan_cluster = fat_read_entry(scan_cluster);
    }
done_counting:

    /* Single allocation for all entries */
    if (total_entries > 0) {
        all_entries = (fat32_dir_entry_t*)kmalloc(total_entries * sizeof(fat32_dir_entry_t));
        if (all_entries == NULL) return -1;
    }

    /* Pass 2: copy entries */
    int idx = 0;
    while (current_cluster != 0 && current_cluster != FAT32_EOC_MARK) {
        uint32_t sector = cluster_to_sector(current_cluster);
        int sectors_per_cluster = boot_sector.bpb.sectors_per_cluster;

        for (int s = 0; s < sectors_per_cluster; s++) {
            uint8_t buffer[DISK_SECTOR_SIZE];
            if (disk_read_sector(sector + s, buffer) != 0) {
                kfree(all_entries);
                return -1;
            }

            fat32_dir_entry_t* dir_entries = (fat32_dir_entry_t*)buffer;
            for (int i = 0; i < 16; i++) {
                if (dir_entries[i].name[0] == 0x00) {
                    *entries = all_entries;
                    *count = idx;
                    return 0;
                }

                if (dir_entries[i].name[0] == 0xE5) continue;
                if ((dir_entries[i].attributes & ATTR_LONG_NAME) == ATTR_LONG_NAME) continue;
                if (dir_entries[i].attributes == ATTR_VOLUME_ID) continue;

                memcpy(&all_entries[idx], &dir_entries[i], sizeof(fat32_dir_entry_t));
                idx++;
            }
        }

        current_cluster = fat_read_entry(current_cluster);
    }
    
    *entries = all_entries;
    *count = total_entries;
    return 0;
}

static fs_entry_t* fs_create_entry_from_dir(fat32_dir_entry_t* dir_entry, fs_entry_t* parent) {
    fs_entry_t* entry = (fs_entry_t*)kmalloc(sizeof(fs_entry_t));
    if (entry == NULL) return NULL;
    
    parse_short_name(dir_entry->name, entry->name);
    entry->type = (dir_entry->attributes & ATTR_DIRECTORY) ? FS_TYPE_DIRECTORY : FS_TYPE_FILE;
    entry->size = dir_entry->file_size;
    entry->first_cluster = ((uint32_t)dir_entry->first_cluster_high << 16) | dir_entry->first_cluster_low;
    entry->attributes = dir_entry->attributes;
    entry->parent = parent;
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        entry->children[i] = NULL;
    }
    
    return entry;
}

static void fs_load_directory(fs_entry_t* dir) {
    if (dir->type != FS_TYPE_DIRECTORY) return;
    
    fat32_dir_entry_t* entries;
    int count;
    
    if (read_directory_entries(dir->first_cluster, &entries, &count) != 0) {
        return;
    }
    
    int idx = 0;
    for (int i = 0; i < count && idx < MAX_DIR_ENTRIES; i++) {
        fs_entry_t* child = fs_create_entry_from_dir(&entries[i], dir);
        if (child != NULL) {
            dir->children[idx++] = child;
        }
    }
    
    kfree(entries);
}

static int fs_write_directory_entry(uint32_t cluster, fat32_dir_entry_t* entry) {
    if (cluster == 0) {
        cluster = boot_sector.root_cluster;
    }
    
    uint32_t current_cluster = cluster;
    
    while (current_cluster != 0 && current_cluster != FAT32_EOC_MARK) {
        uint32_t sector = cluster_to_sector(current_cluster);
        int sectors_per_cluster = boot_sector.bpb.sectors_per_cluster;
        
        for (int s = 0; s < sectors_per_cluster; s++) {
            uint8_t buffer[DISK_SECTOR_SIZE];
            if (disk_read_sector(sector + s, buffer) != 0) {
                return -1;
            }
            
            fat32_dir_entry_t* dir_entries = (fat32_dir_entry_t*)buffer;
            for (int i = 0; i < 16; i++) {
                if (dir_entries[i].name[0] == 0x00 || dir_entries[i].name[0] == 0xE5) {
                    memcpy(&dir_entries[i], entry, sizeof(fat32_dir_entry_t));
                    if (disk_write_sector(sector + s, buffer) != 0) {
                        return -1;
                    }
                    return 0;
                }
            }
        }
        
        current_cluster = fat_read_entry(current_cluster);
    }
    
    uint32_t new_cluster = fat_alloc_cluster();
    if (new_cluster == 0) return -1;
    
    if (fat_write_entry(current_cluster, new_cluster) != 0) {
        fat_write_entry(new_cluster, 0);
        return -1;
    }
    
    uint32_t new_sector = cluster_to_sector(new_cluster);
    uint8_t buffer[DISK_SECTOR_SIZE] = {0};
    fat32_dir_entry_t* dir_entries = (fat32_dir_entry_t*)buffer;
    memcpy(&dir_entries[0], entry, sizeof(fat32_dir_entry_t));
    
    for (int s = 0; s < boot_sector.bpb.sectors_per_cluster; s++) {
        if (disk_write_sector(new_sector + s, buffer) != 0) {
            fat_write_entry(new_cluster, 0);
            fat_write_entry(current_cluster, FAT32_EOC_MARK);
            return -1;
        }
        memset(buffer, 0, DISK_SECTOR_SIZE);
    }
    
    return 0;
}

static void format_short_name(const char* name, uint8_t* output) {
    memset(output, ' ', 11);

    /* FAT32 illegal characters: * ? : " < > | / \ */
    const char* illegal_chars = "*?:\"<>|/\\";

    const char* dot = strchr(name, '.');
    int name_len = dot ? (dot - name) : strlen(name);
    int ext_len = dot ? strlen(dot + 1) : 0;

    if (name_len > 8) name_len = 8;
    if (ext_len > 3) ext_len = 3;

    int output_idx = 0;
    for (int i = 0; i < name_len && output_idx < 8; i++) {
        char c = name[i];

        /* Skip illegal characters */
        int illegal = 0;
        for (const char* p = illegal_chars; *p; p++) {
            if (c == *p) {
                illegal = 1;
                break;
            }
        }
        if (illegal) continue;

        /* Convert to uppercase */
        if (c >= 'a' && c <= 'z') c -= 32;
        output[output_idx++] = c;
    }

    /* FAT32: first byte 0xE5 maps to 0x05 */
    if ((uint8_t)output[0] == 0xE5) output[0] = 0x05;

    if (dot && ext_len > 0) {
        int ext_idx = 0;
        for (int i = 0; i < ext_len && ext_idx < 3; i++) {
            char c = dot[1 + i];

            /* Skip illegal characters */
            int illegal = 0;
            for (const char* p = illegal_chars; *p; p++) {
                if (c == *p) {
                    illegal = 1;
                    break;
                }
            }
            if (illegal) continue;

            /* Convert to uppercase */
            if (c >= 'a' && c <= 'z') c -= 32;
            output[8 + ext_idx++] = c;
        }
    }
}

static void fs_format() {
    memset(&boot_sector, 0, sizeof(fat32_boot_sector_t));
    
    boot_sector.bpb.jump[0] = 0xEB;
    boot_sector.bpb.jump[1] = 0x58;
    boot_sector.bpb.jump[2] = 0x90;
    memcpy(boot_sector.bpb.oem_name, "KIL0YOS ", 8);
    boot_sector.bpb.bytes_per_sector = DISK_SECTOR_SIZE;
    boot_sector.bpb.sectors_per_cluster = 1;
    boot_sector.bpb.reserved_sectors = 32;
    boot_sector.bpb.fat_count = 1;
    boot_sector.bpb.root_entries = 0;
    boot_sector.bpb.total_sectors_16 = 0;
    boot_sector.bpb.media_type = 0xF8;
    boot_sector.bpb.sectors_per_fat_16 = 0;
    boot_sector.bpb.sectors_per_track = 1;
    boot_sector.bpb.heads = 1;
    boot_sector.bpb.hidden_sectors = 0;
    boot_sector.bpb.total_sectors_32 = DISK_MAX_SECTORS;
    
    uint32_t data_sectors = DISK_MAX_SECTORS - boot_sector.bpb.reserved_sectors;
    uint32_t sectors_per_fat = ((data_sectors / boot_sector.bpb.sectors_per_cluster) + 1 + 511) / 512 + 1;
    boot_sector.sectors_per_fat_32 = sectors_per_fat;
    boot_sector.extended_flags = 0;
    boot_sector.fs_version = 0;
    boot_sector.root_cluster = 2;
    boot_sector.fs_info_sector = 1;
    boot_sector.backup_boot_sector = 0;
    boot_sector.drive_number = 0x80;
    boot_sector.reserved1 = 0;
    boot_sector.boot_signature = 0x29;
    boot_sector.volume_id = 0x12345678;
    memcpy(boot_sector.volume_label, "KIL0YOS    ", 11);
    memcpy(boot_sector.fs_type, "FAT32   ", 8);
    boot_sector.boot_signature_word = 0xAA55;
    
    uint8_t buffer[DISK_SECTOR_SIZE];
    memset(buffer, 0, DISK_SECTOR_SIZE);
    memcpy(buffer, &boot_sector, sizeof(fat32_boot_sector_t));
    disk_write_sector(0, buffer);
    
    fat32_fsinfo_t fsinfo;
    memset(&fsinfo, 0, sizeof(fat32_fsinfo_t));
    fsinfo.free_count = 0xFFFFFFFF;
    fsinfo.next_free = 0xFFFFFFFF;
    memcpy(buffer, &fsinfo, sizeof(fat32_fsinfo_t));
    disk_write_sector(1, buffer);
    
    memset(buffer, 0, DISK_SECTOR_SIZE);
    for (uint32_t i = boot_sector.bpb.reserved_sectors;
         i < boot_sector.bpb.reserved_sectors + sectors_per_fat; i++) {
        disk_write_sector(i, buffer);
    }

    /* Initialize FAT table: clear all entries, then set first two */
    memset(buffer, 0, DISK_SECTOR_SIZE);
    *(uint32_t*)buffer = 0x0FFFFFF8;
    *(uint32_t*)(buffer + 4) = FAT32_EOC_MARK;
    disk_write_sector(boot_sector.bpb.reserved_sectors, buffer);

    /* Clear remaining FAT sectors */
    for (uint32_t i = boot_sector.bpb.reserved_sectors + 1;
         i < boot_sector.bpb.reserved_sectors + sectors_per_fat; i++) {
        memset(buffer, 0, DISK_SECTOR_SIZE);
        disk_write_sector(i, buffer);
    }
    
    uint32_t root_sector = cluster_to_sector(2);
    memset(buffer, 0, DISK_SECTOR_SIZE);
    disk_write_sector(root_sector, buffer);
}

int fs_load() {
    uint8_t buffer[DISK_SECTOR_SIZE];
    if (disk_read_sector(0, buffer) != 0) {
        return -1;
    }
    
    memcpy(&boot_sector, buffer, sizeof(fat32_boot_sector_t));
    
    if (memcmp(boot_sector.fs_type, "FAT32   ", 8) != 0) {
        return -1;
    }
    
    root = (fs_entry_t*)kmalloc(sizeof(fs_entry_t));
    if (root == NULL) return -1;
    
    strcpy(root->name, "/");
    root->type = FS_TYPE_DIRECTORY;
    root->size = 0;
    root->first_cluster = boot_sector.root_cluster;
    root->attributes = ATTR_DIRECTORY;
    root->parent = NULL;
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        root->children[i] = NULL;
    }
    
    fs_load_directory(root);
    current = root;
    
    return 0;
}

void fs_init() {
    disk_init();
    
    if (fs_load() == 0) {
        return;
    }
    
    fs_format();
    fs_load();
    
    fs_create_dir("home");
    fs_create_dir("bin");
    fs_create_dir("etc");
    fs_create_dir("tmp");
    
    fs_entry_t* home = fs_resolve_path("/home");
    if (home != NULL) {
        fs_set_current(home);
        fs_create_dir("user");
        fs_set_current(root);
    }
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

/* Extract parent directory and base name from path like "home/test.txt" */
static void resolve_parent_and_name(const char* name, fs_entry_t** parent_out, const char** base_out) {
    const char* last_slash = strrchr(name, '/');
    if (last_slash != NULL) {
        char dir_path[MAX_PATH_LENGTH];
        int dir_len = last_slash - name;
        if (dir_len > 0 && dir_len < MAX_PATH_LENGTH - 1) {
            strncpy(dir_path, name, dir_len);
            dir_path[dir_len] = '\0';
            *parent_out = fs_resolve_path(dir_path);
            *base_out = last_slash + 1;
        } else if (dir_len == 0) {
            *parent_out = root;
            *base_out = last_slash + 1;
        } else {
            *parent_out = current;
            *base_out = name;
        }
    } else {
        *parent_out = current;
        *base_out = name;
    }
}

fs_entry_t* fs_create_file(const char* name) {
    fs_last_error = FS_ERR_NONE;
    
    if (name == NULL || strlen(name) >= 256) {
        fs_last_error = FS_ERR_INVALID;
        return NULL;
    }

    fs_entry_t* parent_dir;
    const char* base_name;
    resolve_parent_and_name(name, &parent_dir, &base_name);

    if (parent_dir == NULL || base_name == NULL || *base_name == '\0') {
        fs_last_error = FS_ERR_INVALID;
        return NULL;
    }

    int exists = fs_check_entry_exists(parent_dir, base_name);
    if (exists == FS_ERR_EXISTS) {
        fs_last_error = FS_ERR_EXISTS;
        return NULL;
    }

    int full = fs_check_dir_full(parent_dir);
    if (full == FS_ERR_FULL) {
        fs_last_error = FS_ERR_FULL;
        return NULL;
    }

    fat32_dir_entry_t dir_entry;
    memset(&dir_entry, 0, sizeof(fat32_dir_entry_t));
    format_short_name(base_name, dir_entry.name);
    dir_entry.attributes = ATTR_ARCHIVE;
    dir_entry.first_cluster_low = 0;
    dir_entry.first_cluster_high = 0;
    dir_entry.file_size = 0;

    if (fs_write_directory_entry(parent_dir->first_cluster, &dir_entry) != 0) {
        fs_last_error = FS_ERR_IO;
        return NULL;
    }

    fs_entry_t* entry = (fs_entry_t*)kmalloc(sizeof(fs_entry_t));
    if (entry == NULL) {
        fs_last_error = FS_ERR_FULL;
        return NULL;
    }

    strcpy(entry->name, base_name);
    entry->type = FS_TYPE_FILE;
    entry->size = 0;
    entry->first_cluster = 0;
    entry->attributes = ATTR_ARCHIVE;
    entry->parent = parent_dir;

    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (parent_dir->children[i] == NULL) {
            parent_dir->children[i] = entry;
            break;
        }
    }
    
    return entry;
}

fs_entry_t* fs_create_dir(const char* name) {
    fs_last_error = FS_ERR_NONE;
    
    if (name == NULL || strlen(name) >= 256) {
        fs_last_error = FS_ERR_INVALID;
        return NULL;
    }

    fs_entry_t* parent_dir;
    const char* base_name;
    resolve_parent_and_name(name, &parent_dir, &base_name);

    if (parent_dir == NULL || base_name == NULL || *base_name == '\0') {
        fs_last_error = FS_ERR_INVALID;
        return NULL;
    }

    int exists = fs_check_entry_exists(parent_dir, base_name);
    if (exists == FS_ERR_EXISTS) {
        fs_last_error = FS_ERR_EXISTS;
        return NULL;
    }

    int full = fs_check_dir_full(parent_dir);
    if (full == FS_ERR_FULL) {
        fs_last_error = FS_ERR_FULL;
        return NULL;
    }
    
    uint32_t new_cluster = fat_alloc_cluster();
    if (new_cluster == 0) {
        fs_last_error = FS_ERR_FULL;
        return NULL;
    }
    
    uint32_t sector = cluster_to_sector(new_cluster);
    uint8_t buffer[DISK_SECTOR_SIZE] = {0};
    for (int s = 0; s < boot_sector.bpb.sectors_per_cluster; s++) {
        disk_write_sector(sector + s, buffer);
    }

    /* 写入 "." 目录项 (当前目录) */
    fat32_dir_entry_t dot_entry;
    memset(&dot_entry, 0, sizeof(fat32_dir_entry_t));
    dot_entry.name[0] = '.';
    dot_entry.attributes = ATTR_DIRECTORY;
    dot_entry.first_cluster_low = new_cluster & 0xFFFF;
    dot_entry.first_cluster_high = (new_cluster >> 16) & 0xFFFF;
    memset(buffer, 0, DISK_SECTOR_SIZE);
    memcpy(buffer, &dot_entry, sizeof(fat32_dir_entry_t));

    /* 写入 ".." 目录项 (父目录) */
    fat32_dir_entry_t dotdot_entry;
    memset(&dotdot_entry, 0, sizeof(fat32_dir_entry_t));
    dotdot_entry.name[0] = '.';
    dotdot_entry.name[1] = '.';
    dotdot_entry.attributes = ATTR_DIRECTORY;
    uint32_t parent_cluster = parent_dir->first_cluster;
    if (parent_cluster == 0) parent_cluster = boot_sector.root_cluster;
    dotdot_entry.first_cluster_low = parent_cluster & 0xFFFF;
    dotdot_entry.first_cluster_high = (parent_cluster >> 16) & 0xFFFF;
    memcpy(buffer + sizeof(fat32_dir_entry_t), &dotdot_entry, sizeof(fat32_dir_entry_t));
    disk_write_sector(sector, buffer);

    fat32_dir_entry_t dir_entry;
    memset(&dir_entry, 0, sizeof(fat32_dir_entry_t));
    format_short_name(base_name, dir_entry.name);
    dir_entry.attributes = ATTR_DIRECTORY;
    dir_entry.first_cluster_low = new_cluster & 0xFFFF;
    dir_entry.first_cluster_high = (new_cluster >> 16) & 0xFFFF;
    dir_entry.file_size = 0;

    if (fs_write_directory_entry(parent_dir->first_cluster, &dir_entry) != 0) {
        fat_write_entry(new_cluster, 0);
        fs_last_error = FS_ERR_IO;
        return NULL;
    }
    
    fs_entry_t* entry = (fs_entry_t*)kmalloc(sizeof(fs_entry_t));
    if (entry == NULL) {
        fat_write_entry(new_cluster, 0);
        fs_last_error = FS_ERR_FULL;
        return NULL;
    }
    
    strcpy(entry->name, base_name);
    entry->type = FS_TYPE_DIRECTORY;
    entry->size = 0;
    entry->first_cluster = new_cluster;
    entry->attributes = ATTR_DIRECTORY;
    entry->parent = parent_dir;

    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        entry->children[i] = NULL;
    }

    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (parent_dir->children[i] == NULL) {
            parent_dir->children[i] = entry;
            break;
        }
    }
    
    return entry;
}

int fs_write_file(fs_entry_t* file, const uint8_t* data, size_t size) {
    if (file == NULL || file->type != FS_TYPE_FILE || data == NULL) return -1;
    
    if (size > MAX_FILE_SIZE) {
        size = MAX_FILE_SIZE;
    }
    
    if (file->first_cluster != 0) {
        fat_free_cluster_chain(file->first_cluster);
    }
    
    uint32_t first_cluster = fat_alloc_cluster();
    if (first_cluster == 0) return -1;
    
    file->first_cluster = first_cluster;
    file->size = size;
    
    uint32_t current_cluster = first_cluster;
    size_t offset = 0;
    
    while (offset < size) {
        uint32_t sector = cluster_to_sector(current_cluster);
        int sectors_per_cluster = boot_sector.bpb.sectors_per_cluster;
        
        for (int s = 0; s < sectors_per_cluster && offset < size; s++) {
            uint8_t buffer[DISK_SECTOR_SIZE] = {0};
            size_t copy_size = DISK_SECTOR_SIZE;
            if (offset + copy_size > size) {
                copy_size = size - offset;
            }
            memcpy(buffer, data + offset, copy_size);
            
            if (disk_write_sector(sector + s, buffer) != 0) {
                fat_free_cluster_chain(first_cluster);
                return -1;
            }
            
            offset += copy_size;
        }
        
        if (offset < size) {
            uint32_t next_cluster = fat_alloc_cluster();
            if (next_cluster == 0) {
                fat_free_cluster_chain(first_cluster);
                return -1;
            }
            fat_write_entry(current_cluster, next_cluster);
            current_cluster = next_cluster;
        }
    }

    /* Update parent directory entry (cluster number and file size) */
    if (file->parent != NULL) {
        uint32_t parent_cluster = file->parent->first_cluster;
        uint32_t current_cluster = parent_cluster;
        int entry_updated = 0;

        while (current_cluster != 0 && current_cluster != FAT32_EOC_MARK && !entry_updated) {
            uint32_t sector = cluster_to_sector(current_cluster);
            int sectors_per_cluster = boot_sector.bpb.sectors_per_cluster;
            for (int s = 0; s < sectors_per_cluster && !entry_updated; s++) {
                uint8_t buffer[DISK_SECTOR_SIZE];
                if (disk_read_sector(sector + s, buffer) != 0) break;
                fat32_dir_entry_t* dir_entries = (fat32_dir_entry_t*)buffer;
                for (int j = 0; j < 16; j++) {
                    char entry_name[256];
                    parse_short_name(dir_entries[j].name, entry_name);
                    if (strcmp(entry_name, file->name) == 0 &&
                        dir_entries[j].name[0] != 0xE5) {
                        dir_entries[j].first_cluster_low = first_cluster & 0xFFFF;
                        dir_entries[j].first_cluster_high = (first_cluster >> 16) & 0xFFFF;
                        dir_entries[j].file_size = size;
                        disk_write_sector(sector + s, buffer);
                        entry_updated = 1;
                        break;
                    }
                }
            }
            current_cluster = fat_read_entry(current_cluster);
        }
    }

    return size;
}

int fs_read_file(fs_entry_t* file, uint8_t* buffer, size_t size) {
    if (file == NULL || file->type != FS_TYPE_FILE || buffer == NULL) return -1;
    
    if (size > file->size) {
        size = file->size;
    }
    
    if (file->first_cluster == 0) {
        return 0;
    }
    
    uint32_t current_cluster = file->first_cluster;
    size_t offset = 0;
    
    while (offset < size && current_cluster != FAT32_EOC_MARK) {
        uint32_t sector = cluster_to_sector(current_cluster);
        int sectors_per_cluster = boot_sector.bpb.sectors_per_cluster;
        
        for (int s = 0; s < sectors_per_cluster && offset < size; s++) {
            uint8_t sector_buffer[DISK_SECTOR_SIZE];
            if (disk_read_sector(sector + s, sector_buffer) != 0) {
                return offset > 0 ? offset : -1;
            }
            
            size_t copy_size = DISK_SECTOR_SIZE;
            if (offset + copy_size > size) {
                copy_size = size - offset;
            }
            
            memcpy(buffer + offset, sector_buffer, copy_size);
            offset += copy_size;
        }
        
        current_cluster = fat_read_entry(current_cluster);
    }
    
    return offset;
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
    
    if (entry->first_cluster != 0) {
        fat_free_cluster_chain(entry->first_cluster);
    }
    
    kfree(entry);
}

int fs_delete_entry(const char* name) {
    if (name == NULL) return -1;

    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (current->children[i] != NULL &&
            strcmp(current->children[i]->name, name) == 0) {

            /* 将磁盘目录项标记为已删除 (0xE5) */
            uint32_t cluster = current->first_cluster;
            uint32_t current_cluster = cluster;
            while (current_cluster != 0 && current_cluster != FAT32_EOC_MARK) {
                uint32_t sector = cluster_to_sector(current_cluster);
                int sectors_per_cluster = boot_sector.bpb.sectors_per_cluster;
                for (int s = 0; s < sectors_per_cluster; s++) {
                    uint8_t buffer[DISK_SECTOR_SIZE];
                    if (disk_read_sector(sector + s, buffer) != 0) break;
                    fat32_dir_entry_t* dir_entries = (fat32_dir_entry_t*)buffer;
                    for (int j = 0; j < 16; j++) {
                        char entry_name[256];
                        parse_short_name(dir_entries[j].name, entry_name);
                        if (strcmp(entry_name, name) == 0) {
                            dir_entries[j].name[0] = 0xE5;
                            disk_write_sector(sector + s, buffer);
                            goto delete_done;
                        }
                    }
                }
                current_cluster = fat_read_entry(current_cluster);
            }
delete_done:
            fs_delete_entry_recursive(current->children[i]);
            current->children[i] = NULL;
            return 0;
        }
    }

    return -1;
}

int fs_get_last_error() {
    return fs_last_error;
}

static void fs_save_file(fs_entry_t* file);
static void fs_save_directory(fs_entry_t* dir);

void fs_save() {
    uint8_t buffer[DISK_SECTOR_SIZE];

    /* 1. Save boot sector */
    memset(buffer, 0, DISK_SECTOR_SIZE);
    memcpy(buffer, &boot_sector, sizeof(fat32_boot_sector_t));
    disk_write_sector(0, buffer);

    /* 2. Sync FAT table to disk (all modified entries) */
    /* FAT entries are already written by fat_write_entry() during operations,
       so we just need to ensure the FAT table is consistent */
    /* No additional action needed as fat_write_entry() writes immediately */

    /* 3. Save root directory and all subdirectories recursively */
    if (root != NULL) {
        fs_save_directory(root);
    }
}

/*
 * Save file content clusters to disk.
 * Reads from in-memory fs_entry_t and writes to disk clusters.
 */
static void fs_save_file(fs_entry_t* file) {
    if (file == NULL || file->type != FS_TYPE_FILE) return;
    if (file->first_cluster == 0) return;

    /* File content is already written by fs_write_file() during edit/save,
       so we just need to ensure the directory entry is updated */
    /* The directory entry update is handled by fs_save_directory() */
}

/*
 * Write all in-memory directory entries of 'dir' back to disk.
 * Recursively saves all child directories and files.
 * Supports multi-cluster directories.
 */
static void fs_save_directory(fs_entry_t* dir) {
    if (dir == NULL || dir->type != FS_TYPE_DIRECTORY) return;
    if (dir->first_cluster == 0) return;

    /* First, recursively save all child directories and files */
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir->children[i] != NULL) {
            if (dir->children[i]->type == FS_TYPE_DIRECTORY) {
                fs_save_directory(dir->children[i]);
            } else if (dir->children[i]->type == FS_TYPE_FILE) {
                fs_save_file(dir->children[i]);
            }
        }
    }

    /* Count directory entries */
    int entry_count = 0;
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir->children[i] != NULL) entry_count++;
    }

    /* Calculate required clusters */
    int entries_per_cluster = (DISK_SECTOR_SIZE * boot_sector.bpb.sectors_per_cluster) / sizeof(fat32_dir_entry_t);
    int clusters_needed = (entry_count + entries_per_cluster - 1) / entries_per_cluster;

    /* Build directory content in memory */
    uint8_t* dir_data = (uint8_t*)kmalloc(DISK_SECTOR_SIZE * boot_sector.bpb.sectors_per_cluster);
    if (dir_data == NULL) return;

    /* Write entries to clusters */
    int entry_offset = 0;
    uint32_t current_cluster = dir->first_cluster;
    int cluster_index = 0;

    while (current_cluster != 0 && current_cluster != FAT32_EOC_MARK && cluster_index < clusters_needed) {
        memset(dir_data, 0, DISK_SECTOR_SIZE * boot_sector.bpb.sectors_per_cluster);

        /* Fill this cluster with entries */
        int entries_in_this_cluster = 0;
        int max_entries_this_cluster = entries_per_cluster;

        for (int i = 0; i < MAX_DIR_ENTRIES && entries_in_this_cluster < max_entries_this_cluster; i++) {
            fs_entry_t* child = dir->children[i];
            if (child == NULL) continue;

            /* Skip entries already written in previous clusters */
            if (entry_offset >= cluster_index * entries_per_cluster) {
                fat32_dir_entry_t de;
                memset(&de, 0, sizeof(fat32_dir_entry_t));

                format_short_name(child->name, de.name);
                de.attributes = child->attributes;
                de.first_cluster_low = child->first_cluster & 0xFFFF;
                de.first_cluster_high = (child->first_cluster >> 16) & 0xFFFF;
                de.file_size = child->size;

                int local_offset = entry_offset % entries_per_cluster;
                memcpy(dir_data + local_offset * sizeof(fat32_dir_entry_t), &de, sizeof(fat32_dir_entry_t));
                entries_in_this_cluster++;
            }
            entry_offset++;
        }

        /* Write this cluster to disk */
        uint32_t sector = cluster_to_sector(current_cluster);
        for (int s = 0; s < boot_sector.bpb.sectors_per_cluster; s++) {
            disk_write_sector(sector + s, dir_data + s * DISK_SECTOR_SIZE);
        }

        /* Move to next cluster */
        current_cluster = fat_read_entry(current_cluster);
        cluster_index++;
    }

    kfree(dir_data);
}
