#include "core/isr.h"
#include "core/idt.h"
#include "drivers/io.h"
#include "core/interrupts.h"
#include "sched/scheduler.h"
#include "timer/pit.h"

#define IRQ0 32

void (*irq_handlers[16])(interrupt_frame_t*) = {0};

void register_irq_handler(uint8_t irq, void (*handler)(interrupt_frame_t*)) {
    irq_handlers[irq] = handler;
}

void isr_set_gate(int num, void (*handler)()) {
    idt_set_gate(num, (uint64_t)handler, 0x08, 0x8E);
}

void isr_init() {
    isr_set_gate(0, isr0);
    isr_set_gate(1, isr1);
    isr_set_gate(2, isr2);
    isr_set_gate(3, isr3);
    isr_set_gate(4, isr4);
    isr_set_gate(5, isr5);
    isr_set_gate(6, isr6);
    isr_set_gate(7, isr7);
    isr_set_gate(8, isr8);
    isr_set_gate(9, isr9);
    isr_set_gate(10, isr10);
    isr_set_gate(11, isr11);
    isr_set_gate(12, isr12);
    isr_set_gate(13, isr13);
    isr_set_gate(14, isr14);
    isr_set_gate(15, isr15);
    isr_set_gate(16, isr16);
    isr_set_gate(17, isr17);
    isr_set_gate(18, isr18);
    isr_set_gate(19, isr19);
    isr_set_gate(20, isr20);
    isr_set_gate(21, isr21);
    isr_set_gate(22, isr22);
    isr_set_gate(23, isr23);
    isr_set_gate(24, isr24);
    isr_set_gate(25, isr25);
    isr_set_gate(26, isr26);
    isr_set_gate(27, isr27);
    isr_set_gate(28, isr28);
    isr_set_gate(29, isr29);
    isr_set_gate(30, isr30);
    isr_set_gate(31, isr31);

    isr_set_gate(32, irq0);
    isr_set_gate(33, irq1);
    isr_set_gate(34, irq2);
    isr_set_gate(35, irq3);
    isr_set_gate(36, irq4);
    isr_set_gate(37, irq5);
    isr_set_gate(38, irq6);
    isr_set_gate(39, irq7);
    isr_set_gate(40, irq8);
    isr_set_gate(41, irq9);
    isr_set_gate(42, irq10);
    isr_set_gate(43, irq11);
    isr_set_gate(44, irq12);
    isr_set_gate(45, irq13);
    isr_set_gate(46, irq14);
    isr_set_gate(47, irq15);
}

uint64_t isr_handler(interrupt_frame_t* frame) {
    (void)frame;
    return (uint64_t)frame;
}

uint64_t irq_handler(interrupt_frame_t* frame) {
    uint8_t irq_num = frame->interrupt_number - IRQ0;

    if (irq_handlers[irq_num] != 0) {
        irq_handlers[irq_num](frame);
    }

    if (irq_num == 0) {
        pit_ticks++;
        pic_send_eoi(0);
        return scheduler_tick((uint64_t)frame);
    }

    /* Non-zero IRQ handlers must send EOI themselves; do NOT send fallback EOI here
     * to avoid double-EOI which corrupts PIC state (especially on VirtualBox).
     */
    return (uint64_t)frame;
}
