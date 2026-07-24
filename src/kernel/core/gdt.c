#include "core/gdt.h"
#include "lib/string.h"

/* GDT with space for TSS (8 entries) */
gdt_entry_t gdt[GDT_ENTRIES];
gdt_ptr_t gdt_ptr;

/* TSS descriptor at index 5-6 (16 bytes) */
static tss_descriptor_t* tss_desc = (tss_descriptor_t*)&gdt[5];

void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[index].base_low = (base & 0xFFFF);
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;

    gdt[index].limit_low = (limit & 0xFFFF);
    gdt[index].granularity = ((limit >> 16) & 0x0F) | (granularity & 0xF0);

    gdt[index].access = access;
}

void gdt_set_tss(uint64_t tss_addr, uint32_t tss_size) {
    /* Clear the TSS descriptor area (indices 5 and 6) */
    memset(&gdt[5], 0, 16);

    /* Set TSS descriptor for 64-bit mode
     * Format: 16-byte descriptor (system descriptor + extended descriptor)
     */
    tss_descriptor_t* desc = (tss_descriptor_t*)&gdt[5];

    desc->limit_low = tss_size & 0xFFFF;
    desc->base_low = tss_addr & 0xFFFF;
    desc->base_mid_low = (tss_addr >> 16) & 0xFF;
    desc->access = 0x89;  /* Present, 64-bit TSS available */
    desc->limit_high_flags = ((tss_size >> 16) & 0x0F) | 0x00;
    desc->base_mid_high = (tss_addr >> 24) & 0xFF;
    desc->base_high = (tss_addr >> 32) & 0xFFFFFFFF;
    desc->reserved = 0;
}

void gdt_flush(void);

void gdt_init(void) {
    /* Clear GDT */
    memset(gdt, 0, sizeof(gdt));

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)&gdt;

    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Kernel code segment (selector 0x08)
     * Access: Present, Ring 0, Code, Executable, Readable
     * Granularity: 64-bit, 4KB pages, limit = 0xFFFFF (4GB)
     */
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);  /* L=1, D=0 for 64-bit */

    /* Kernel data segment (selector 0x10) */
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* User code segment (selector 0x18)
     * Access: Present, Ring 3, Code, Executable, Readable
     */
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xAF);  /* L=1, D=0 for 64-bit */

    /* User data segment (selector 0x20) */
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* TSS descriptor will be set by tss_init() at indices 5-6 */

    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));
    gdt_flush();
}

void gdt_reload(void) {
    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));
    gdt_flush();
}