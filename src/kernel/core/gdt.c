#include "core/gdt.h"

gdt_entry_t gdt[5];
gdt_ptr_t gdt_ptr;

void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[index].base_low = (base & 0xFFFF);
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;

    gdt[index].limit_low = (limit & 0xFFFF);
    gdt[index].granularity = ((limit >> 16) & 0x0F) | (granularity & 0xF0);

    gdt[index].access = access;
}

void gdt_flush();

void gdt_init() {
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)&gdt;

    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);  /* 64-bit code: L=1, D=0 */
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);  /* 64-bit data */
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xAF);  /* 64-bit user code: L=1, D=0 */
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);  /* 64-bit user data */

    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));
    gdt_flush();
}

void gdt_reload() {
    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));
    gdt_flush();
}
