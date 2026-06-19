#include "fs.h"
#include "memory.h"
#include "string.h"

static fs_entry_t* root;
static fs_entry_t* current;

void fs_init() {
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