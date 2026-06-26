#ifndef GDT_H
#define GDT_H

#include "lib/types.h"

typedef struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

extern gdt_entry_t gdt[5];
extern gdt_ptr_t gdt_ptr;

void gdt_init();
void gdt_reload();

#endif
