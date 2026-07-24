#ifndef GDT_H
#define GDT_H

#include "lib/types.h"

/* GDT entry structure (8 bytes for 32-bit descriptors) */
typedef struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* GDT pointer structure */
typedef struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

/* TSS descriptor (16 bytes for 64-bit) */
typedef struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid_low;
    uint8_t  access;
    uint8_t  limit_high_flags;
    uint8_t  base_mid_high;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed)) tss_descriptor_t;

/* GDT selectors */
#define KERNEL_CS   0x08
#define KERNEL_DS   0x10
#define USER_CS     0x18
#define USER_DS     0x20
#define TSS_SEL     0x28

/* GDT size: 5 existing entries + 2 for TSS = 7 entries, but we use 8 for alignment */
#define GDT_ENTRIES 8

extern gdt_entry_t gdt[];
extern gdt_ptr_t gdt_ptr;

void gdt_init(void);
void gdt_reload(void);
void gdt_set_tss(uint64_t tss_addr, uint32_t tss_size);

#endif
