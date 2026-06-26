#include "core/idt.h"

idt_entry_t idt[256];
idt_ptr_t idt_ptr;

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags) {
    idt[num].offset_low = (handler & 0xFFFF);
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].selector = selector;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].reserved = 0;
}

void idt_init() {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;

    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}

void idt_reload() {
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}
