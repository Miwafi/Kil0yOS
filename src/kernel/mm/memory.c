#include <stdint.h>
#include "mm/memory.h"
#include "lib/string.h"
#include "drivers/io.h"
#include "drivers/vga.h"
#include "core/interrupts.h"

/* ======================================================================== */
/*  Legacy Heap Allocator (unchanged behaviour)                             */
/* ======================================================================== */

static uint8_t* heap_start;
static uint8_t* heap_end;

typedef struct heap_block {
    size_t size;
    struct heap_block* next;
    int free;
    int _pad;
} heap_block_t;

static heap_block_t* heap_list = NULL;

void memory_init(memory_map_t* map, size_t count) {
    uint8_t* default_start = (uint8_t*)0x200000;
    uint8_t* default_end   = (uint8_t*)0x10000000;

    if (map != NULL && count > 0) {
        uint64_t largest_size = 0;
        uint8_t* largest_start = default_start;

        for (size_t i = 0; i < count; i++) {
            memory_map_t* entry = &map[i];
            if (entry->type != 1) continue;

            uint64_t base  = ((uint64_t)entry->base_addr_high << 32) | entry->base_addr_low;
            uint64_t length = ((uint64_t)entry->length_high << 32) | entry->length_low;

            if (length > largest_size && base >= 0x200000) {
                largest_size = length;
                largest_start = (uint8_t*)base;
            }
        }

        if (largest_size > 0x100000) {
            heap_start = largest_start;
            heap_end   = largest_start + (largest_size > 0x10000000 ? 0x10000000 : largest_size);
        } else {
            heap_start = default_start;
            heap_end   = default_end;
        }
    } else {
        heap_start = default_start;
        heap_end   = default_end;
    }

    heap_list = (heap_block_t*)heap_start;
    heap_list->size = heap_end - heap_start - sizeof(heap_block_t);
    heap_list->next = NULL;
    heap_list->free = 1;
}

void* kmalloc(size_t size) {
    heap_block_t* current = heap_list;
    heap_block_t* prev    = NULL;

    while (current != NULL) {
        if (current->free && current->size >= size) {
            size_t remaining = current->size - size - sizeof(heap_block_t);
            if (remaining > 0) {
                heap_block_t* new_block = (heap_block_t*)((uint8_t*)(current + 1) + size);
                new_block->size = remaining;
                new_block->next = current->next;
                new_block->free = 1;
                current->next   = new_block;
            }
            current->size = size;
            current->free = 0;
            return (void*)(current + 1);
        }
        prev = current;
        current = current->next;
    }

    uint8_t* new_addr;
    if (prev != NULL) {
        new_addr = (uint8_t*)(prev + 1) + prev->size;
    } else if (heap_list != NULL) {
        new_addr = (uint8_t*)(heap_list + 1) + heap_list->size;
    } else {
        new_addr = heap_start;
    }

    if (new_addr + sizeof(heap_block_t) + size <= heap_end) {
        heap_block_t* block = (heap_block_t*)new_addr;
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
    if ((uint8_t*)ptr < heap_start + sizeof(heap_block_t) ||
        (uint8_t*)ptr >= heap_end) {
        return;
    }
    heap_block_t* block = (heap_block_t*)ptr - 1;
    if ((uint8_t*)block < heap_start || (uint8_t*)block >= heap_end) return;

    block->free = 1;
    heap_block_t* current = heap_list;
    while (current != NULL && current->next != NULL) {
        if (current->free && current->next->free) {
            current->size += sizeof(heap_block_t) + current->next->size;
            current->next  = current->next->next;
        }
        current = current->next;
    }
}

void* kcalloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    void* ptr = kmalloc(total);
    if (ptr != NULL) memset(ptr, 0, total);
    return ptr;
}

void* krealloc(void* ptr, size_t size) {
    if (ptr == NULL) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }
    heap_block_t* block = (heap_block_t*)ptr - 1;
    if (block->size >= size) return ptr;
    void* new_ptr = kmalloc(size);
    if (new_ptr != NULL) {
        memcpy(new_ptr, ptr, block->size);
        kfree(ptr);
    }
    return new_ptr;
}

/* ======================================================================== */
/*  PMM — Physical Memory Manager (bitmap-based, 4 KiB pages)              */
/* ======================================================================== */

static uint8_t pmm_bitmap[PMM_MAX_PAGES / 8];
static uint64_t pmm_last_page = 0;

static inline void bitmap_set(uint64_t page) {
    if (page < PMM_MAX_PAGES)
        pmm_bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint64_t page) {
    if (page < PMM_MAX_PAGES)
        pmm_bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bitmap_test(uint64_t page) {
    if (page >= PMM_MAX_PAGES) return 1;
    return (pmm_bitmap[page / 8] >> (page % 8)) & 1;
}

static void pmm_mark_region(uint64_t base, uint64_t length, int used) {
    uint64_t start_page = base / PAGE_SIZE;
    uint64_t end_page   = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;

    if (end_page > PMM_MAX_PAGES) end_page = PMM_MAX_PAGES;

    for (uint64_t p = start_page; p < end_page; p++) {
        if (used)
            bitmap_set(p);
        else
            bitmap_clear(p);
    }
}

/* Multiboot2 structures */
struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

#define MB2_TAG_END  0
#define MB2_TAG_MMAP 6

void pmm_init(uint64_t mb_info_phys) {
    /* Everything reserved by default */
    memset(pmm_bitmap, 0xFF, sizeof(pmm_bitmap));

    /* Parse multiboot2 mmap if present */
    if (mb_info_phys != 0) {
        uint8_t* info = (uint8_t*)mb_info_phys;
        uint32_t total_size = *(uint32_t*)info;

        uint8_t* tag = info + 8;
        while (tag < info + total_size) {
            struct mb2_tag* t = (struct mb2_tag*)tag;
            if (t->type == MB2_TAG_END) break;

            if (t->type == MB2_TAG_MMAP) {
                uint32_t entry_size = *(uint32_t*)(tag + 8);
                uint32_t entry_ver  = *(uint32_t*)(tag + 12);
                (void)entry_ver;

                uint32_t data_size  = t->size - 16;
                uint32_t num_entries = data_size / entry_size;

                for (uint32_t i = 0; i < num_entries; i++) {
                    uint8_t* e = tag + 16 + i * entry_size;
                    uint64_t base  = *(uint64_t*)(e + 0);
                    uint64_t len   = *(uint64_t*)(e + 8);
                    uint32_t mtype = *(uint32_t*)(e + 16);

                    if (mtype == 1 && base < (PMM_MAX_PAGES * PAGE_SIZE)) {
                        if (base + len > (PMM_MAX_PAGES * PAGE_SIZE))
                            len = (PMM_MAX_PAGES * PAGE_SIZE) - base;
                        pmm_mark_region(base, len, 0);
                    }
                }
            }

            uint32_t advance = (t->size + 7) & ~7;
            if (advance == 0) break; /* malformed */
            tag += advance;
        }
    }

    /* Reserve kernel image area */
    extern char kernel_start[];
    extern char kernel_end[];
    uint64_t kstart = (uint64_t)kernel_start;
    uint64_t kend   = (uint64_t)kernel_end;
    pmm_mark_region(kstart, kend - kstart, 1);

    /* Reserve the bitmap itself */
    pmm_mark_region((uint64_t)pmm_bitmap, sizeof(pmm_bitmap), 1);

    /* Reserve first 2 MiB (real-mode/BIOS stuff, boot page tables, etc.) */
    pmm_mark_region(0, 0x200000, 1);
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t p = pmm_last_page; p < PMM_MAX_PAGES; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            pmm_last_page = p + 1;
            return p * PAGE_SIZE;
        }
    }
    for (uint64_t p = 0; p < pmm_last_page; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            pmm_last_page = p + 1;
            return p * PAGE_SIZE;
        }
    }
    return 0; /* out of memory */
}

uint64_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    uint64_t consecutive = 0;
    uint64_t start = 0;

    for (uint64_t p = 0; p < PMM_MAX_PAGES; p++) {
        if (!bitmap_test(p)) {
            if (consecutive == 0) start = p;
            consecutive++;
            if (consecutive >= count) {
                for (uint64_t i = start; i < start + count; i++)
                    bitmap_set(i);
                return start * PAGE_SIZE;
            }
        } else {
            consecutive = 0;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys) {
    if (phys == 0) return;
    uint64_t p = phys / PAGE_SIZE;
    if (p < PMM_MAX_PAGES) bitmap_clear(p);
}

void pmm_free_pages(uint64_t phys, size_t count) {
    for (size_t i = 0; i < count; i++)
        pmm_free_page(phys + i * PAGE_SIZE);
}

/* ======================================================================== */
/*  VMM — Simple Virtual Memory Manager (4-level page tables)               */
/* ======================================================================== */

static uint64_t* vmm_pml4 = NULL;

void vmm_init(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    vmm_pml4 = (uint64_t*)(cr3 & ~0xFFF);
}

void vmm_reload_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3));
}

static inline uint64_t vmm_make_entry(uint64_t phys, uint64_t flags) {
    return (phys & ~0xFFF) | (flags & 0xFFF) | VMM_PRESENT;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!vmm_pml4) vmm_init();

    uint64_t pml4i = (virt >> 39) & 0x1FF;
    uint64_t pdpti = (virt >> 30) & 0x1FF;
    uint64_t pdi   = (virt >> 21) & 0x1FF;
    uint64_t pti   = (virt >> 12) & 0x1FF;

    if (!(vmm_pml4[pml4i] & VMM_PRESENT)) {
        uint64_t new_pdpt = pmm_alloc_page();
        if (!new_pdpt) PANIC("vmm_map_page: out of physical memory (pdpt)");
        vmm_pml4[pml4i] = vmm_make_entry(new_pdpt, VMM_WRITABLE);
        memset((void*)new_pdpt, 0, PAGE_SIZE);
    }
    uint64_t* pdpt = (uint64_t*)(vmm_pml4[pml4i] & ~0xFFF);

    if (!(pdpt[pdpti] & VMM_PRESENT)) {
        uint64_t new_pd = pmm_alloc_page();
        if (!new_pd) PANIC("vmm_map_page: out of physical memory (pd)");
        pdpt[pdpti] = vmm_make_entry(new_pd, VMM_WRITABLE);
        memset((void*)new_pd, 0, PAGE_SIZE);
    }
    uint64_t* pd = (uint64_t*)(pdpt[pdpti] & ~0xFFF);

    /* If there's a huge page here, overwrite it (caller beware) */
    if ((pd[pdi] & VMM_PRESENT) && (pd[pdi] & VMM_HUGE)) {
        pd[pdi] = 0;
    }

    if (!(pd[pdi] & VMM_PRESENT)) {
        uint64_t new_pt = pmm_alloc_page();
        if (!new_pt) PANIC("vmm_map_page: out of physical memory (pt)");
        pd[pdi] = vmm_make_entry(new_pt, VMM_WRITABLE);
        memset((void*)new_pt, 0, PAGE_SIZE);
    }
    uint64_t* pt = (uint64_t*)(pd[pdi] & ~0xFFF);

    pt[pti] = vmm_make_entry(phys, flags);
    vmm_reload_cr3();
}

void vmm_unmap_page(uint64_t virt) {
    if (!vmm_pml4) return;

    uint64_t pml4i = (virt >> 39) & 0x1FF;
    if (!(vmm_pml4[pml4i] & VMM_PRESENT)) return;

    uint64_t* pdpt = (uint64_t*)(vmm_pml4[pml4i] & ~0xFFF);
    uint64_t pdpti = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & VMM_PRESENT)) return;

    uint64_t* pd = (uint64_t*)(pdpt[pdpti] & ~0xFFF);
    uint64_t pdi = (virt >> 21) & 0x1FF;
    if (!(pd[pdi] & VMM_PRESENT)) return;

    if (pd[pdi] & VMM_HUGE) {
        pd[pdi] = 0;
        vmm_reload_cr3();
        return;
    }

    uint64_t* pt = (uint64_t*)(pd[pdi] & ~0xFFF);
    uint64_t pti = (virt >> 12) & 0x1FF;
    pt[pti] = 0;
    vmm_reload_cr3();
}

uint64_t vmm_get_phys(uint64_t virt) {
    if (!vmm_pml4) return 0;

    uint64_t pml4i = (virt >> 39) & 0x1FF;
    if (!(vmm_pml4[pml4i] & VMM_PRESENT)) return 0;

    uint64_t* pdpt = (uint64_t*)(vmm_pml4[pml4i] & ~0xFFF);
    uint64_t pdpti = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & VMM_PRESENT)) return 0;

    uint64_t* pd = (uint64_t*)(pdpt[pdpti] & ~0xFFF);
    uint64_t pdi = (virt >> 21) & 0x1FF;
    if (!(pd[pdi] & VMM_PRESENT)) return 0;

    if (pd[pdi] & VMM_HUGE) {
        return (pd[pdi] & ~0x1FFFFF) | (virt & 0x1FFFFF);
    }

    uint64_t* pt = (uint64_t*)(pd[pdi] & ~0xFFF);
    uint64_t pti = (virt >> 12) & 0x1FF;
    if (!(pt[pti] & VMM_PRESENT)) return 0;

    return (pt[pti] & ~0xFFF) | (virt & 0xFFF);
}

/* ======================================================================== */
/*  Panic / Assert                                                          */
/* ======================================================================== */

static void panic_serial_puts(const char* s) {
    while (*s) {
        if (*s == '\n') {
            while ((inb(0x3F8 + 5) & 0x20) == 0);
            outb(0x3F8, '\r');
        }
        while ((inb(0x3F8 + 5) & 0x20) == 0);
        outb(0x3F8, (uint8_t)*s);
        s++;
    }
}

static void panic_itoa(int num, char* buf, int base) {
    char tmp[32];
    int i = 0;
    int neg = 0;
    if (num < 0 && base == 10) { neg = 1; num = -num; }
    do {
        int digit = num % base;
        tmp[i++] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
        num /= base;
    } while (num > 0);
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

void panic(const char* msg, const char* file, int line) {
    disable_interrupts();

    char buf[16];
    panic_itoa(line, buf, 10);

    /* Serial output */
    panic_serial_puts("\n\n*** KERNEL PANIC ***\n");
    panic_serial_puts("Message: ");
    panic_serial_puts(msg);
    panic_serial_puts("\nFile: ");
    panic_serial_puts(file);
    panic_serial_puts("\nLine: ");
    panic_serial_puts(buf);
    panic_serial_puts("\nSystem halted.\n");

    /* VGA output */
    vga_set_color(vga_entry_color(COLOR_LIGHT_RED, COLOR_BLACK));
    vga_puts("\n\n*** KERNEL PANIC ***\n");
    vga_puts("Message: ");
    vga_puts(msg);
    vga_puts("\nFile: ");
    vga_puts(file);
    vga_puts("\nLine: ");
    vga_puts(buf);
    vga_puts("\nSystem halted.\n");

    __asm__ volatile("cli");
    for (;;) { __asm__ volatile("hlt"); }
}

void panic_assert(const char* cond, const char* file, int line) {
    panic(cond, file, line);
}
