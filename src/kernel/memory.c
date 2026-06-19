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
    heap_start = (uint8_t*)0x200000;
    heap_end = (uint8_t*)0x10000000;
    
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
    
    uint8_t* new_block_addr = prev ? (uint8_t*)(prev + 1) + prev->size : heap_start;
    
    if (new_block_addr + sizeof(heap_block_t) + size <= heap_end) {
        heap_block_t* block = (heap_block_t*)new_block_addr;
        block->size = size;
        block->next = NULL;
        block->free = 0;
        
        if (prev != NULL) {
            prev->next = block;
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