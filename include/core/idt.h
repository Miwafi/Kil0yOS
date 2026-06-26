#ifndef IDT_H
#define IDT_H

#include "lib/types.h"

typedef struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

#include "core/isr.h"

extern idt_entry_t idt[256];
extern idt_ptr_t idt_ptr;

void idt_init();
void idt_reload();
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags);

#endif
