#include "idt.h"

static idt_entry_t idt[256];
static idt_ptr_t idt_ptr;

void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags) {
    idt[num].offset_low = (handler & 0xFFFF);
    idt[num].offset_high = (handler >> 16) & 0xFFFF;
    idt[num].selector = selector;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

void idt_init() {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t) &idt;
    
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}