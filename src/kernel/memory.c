#include <stdint.h>
#include "memory.h"
#include "string.h"

static uint8_t* heap_start;
static uint8_t* heap_end;

typedef struct heap_block {
    size_t size;
    struct heap_block* next;
    int free;
} heap_block_t;

static heap_block_t* heap_list = NULL;

void memory_init(memory_map_t* map, size_t count) {
    uint8_t* default_start = (uint8_t*)0x200000;
    uint8_t* default_end = (uint8_t*)0x10000000;
    
    if (map != NULL && count > 0) {
        uint64_t largest_size = 0;
        uint8_t* largest_start = default_start;
        
        for (size_t i = 0; i < count; i++) {
            memory_map_t* entry = &map[i];
            
            if (entry->type != 1) continue;
            
            uint64_t base = ((uint64_t)entry->base_addr_high << 32) | entry->base_addr_low;
            uint64_t length = ((uint64_t)entry->length_high << 32) | entry->length_low;
            
            if (length > largest_size && base >= 0x200000) {
                largest_size = length;
                largest_start = (uint8_t*)base;
            }
        }
        
        if (largest_size > 0x100000) {
            heap_start = largest_start;
            heap_end = largest_start + (largest_size > 0x10000000 ? 0x10000000 : largest_size);
        } else {
            heap_start = default_start;
            heap_end = default_end;
        }
    } else {
        heap_start = default_start;
        heap_end = default_end;
    }
    
    heap_list = (heap_block_t*)heap_start;
    heap_list->size = heap_end - heap_start - sizeof(heap_block_t);
    heap_list->next = NULL;
    heap_list->free = 1;
}

void* kmalloc(size_t size) {
    heap_block_t* current = heap_list;
    heap_block_t* prev = NULL;
    
    while (current != NULL) {
        if (current->free && current->size >= size) {
            size_t remaining_size = current->size - size - sizeof(heap_block_t);
            
            if (remaining_size > 0) {
                heap_block_t* new_block = (heap_block_t*)((uint8_t*)(current + 1) + size);
                new_block->size = remaining_size;
                new_block->next = current->next;
                new_block->free = 1;
                current->next = new_block;
            }
            
            current->size = size;
            current->free = 0;
            return (void*)(current + 1);
        }
        prev = current;
        current = current->next;
    }
    
    uint8_t* new_block_addr;
    
    if (prev != NULL) {
        new_block_addr = (uint8_t*)(prev + 1) + prev->size;
    } else if (heap_list != NULL) {
        new_block_addr = (uint8_t*)(heap_list + 1) + heap_list->size;
    } else {
        new_block_addr = heap_start;
    }
    
    if (new_block_addr + sizeof(heap_block_t) + size <= heap_end) {
        heap_block_t* block = (heap_block_t*)new_block_addr;
        block->size = size;
        block->next = NULL;
        block->free = 0;
        
        if (prev != NULL) {
            prev->next = block;
        } else if (heap_list == NULL) {
            heap_list = block;
        }
        
        return (void*)(block + 1);
    }
    
    return NULL;
}

void kfree(void* ptr) {
    if (ptr == NULL) return;
    
    heap_block_t* block = (heap_block_t*)ptr - 1;
    block->free = 1;
    
    heap_block_t* current = heap_list;
    while (current != NULL && current->next != NULL) {
        if (current->free && current->next->free) {
            current->size += sizeof(heap_block_t) + current->next->size;
            current->next = current->next->next;
        }
        current = current->next;
    }
}

void* kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* ptr = kmalloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* krealloc(void* ptr, size_t size) {
    if (ptr == NULL) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    heap_block_t* block = (heap_block_t*)ptr - 1;
    
    if (block->size >= size) {
        return ptr;
    }
    
    void* new_ptr = kmalloc(size);
    if (new_ptr != NULL) {
        memcpy(new_ptr, ptr, block->size);
        kfree(ptr);
    }
    
    return new_ptr;
}