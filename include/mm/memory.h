#ifndef MEMORY_H
#define MEMORY_H

#include "lib/types.h"

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

/* Legacy memory map structure (kept for compat with old memory_init) */
typedef struct memory_map {
    uint32_t base_addr_low;
    uint32_t base_addr_high;
    uint32_t length_low;
    uint32_t length_high;
    uint32_t type;
    uint32_t acpi;
} memory_map_t;

/* --- Heap allocator --- */
void memory_init(memory_map_t* map, size_t count);
void* kmalloc(size_t size);
void kfree(void* ptr);
void* kcalloc(size_t nmemb, size_t size);
void* krealloc(void* ptr, size_t size);

/* --- PMM --- */
#define PMM_MAX_PAGES (1024ULL * 1024ULL)  /* 4GB / 4KB */

void pmm_init(uint64_t mb_info_phys);
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(size_t count);
void pmm_free_page(uint64_t phys);
void pmm_free_pages(uint64_t phys, size_t count);
void pmm_get_stats(uint64_t* total_pages, uint64_t* used_pages, uint64_t* free_pages);

/* --- VMM --- */
#define VMM_PRESENT  0x001
#define VMM_WRITABLE 0x002
#define VMM_USER     0x004
#define VMM_WT       0x008
#define VMM_CD       0x010
#define VMM_ACCESSED 0x020
#define VMM_DIRTY    0x040
#define VMM_HUGE     0x080
#define VMM_GLOBAL   0x100
#define VMM_NX       0x8000000000000000ULL

void vmm_init(void);
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_get_phys(uint64_t virt);
void vmm_reload_cr3(void);

/* --- Panic / Assert --- */
void panic(const char* msg, const char* file, int line);
void panic_assert(const char* cond, const char* file, int line);

#define PANIC(msg) panic(msg, __FILE__, __LINE__)
#define ASSERT(cond) ((cond) ? (void)0 : panic_assert(#cond, __FILE__, __LINE__))

#endif
