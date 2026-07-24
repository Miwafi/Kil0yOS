#include "core/isr.h"
#include "core/idt.h"
#include "drivers/io.h"
#include "core/interrupts.h"
#include "sched/scheduler.h"
#include "timer/pit.h"
#include "core/syscall.h"
#include "core/tss.h"

#define IRQ0 32

void (*irq_handlers[16])(interrupt_frame_t*) = {0};

void register_irq_handler(uint8_t irq, void (*handler)(interrupt_frame_t*)) {
    irq_handlers[irq] = handler;
}

void isr_set_gate(int num, void (*handler)(), uint8_t type) {
    /* Set DPL to 0 for hardware interrupts, 3 for software interrupts (like syscall) */
    uint8_t dpl = (type == 0xEE) ? 3 : 0;  /* 0xEE = interrupt gate, DPL=3 */
    uint8_t flags = type | dpl;
    idt_set_gate(num, (uint64_t)handler, 0x08, flags);
}

void isr_init() {
    /* CPU exceptions (ISR 0-31) */
    isr_set_gate(0, isr0, 0x8E);
    isr_set_gate(1, isr1, 0x8E);
    isr_set_gate(2, isr2, 0x8E);
    isr_set_gate(3, isr3, 0x8E);
    isr_set_gate(4, isr4, 0x8E);
    isr_set_gate(5, isr5, 0x8E);
    isr_set_gate(6, isr6, 0x8E);
    isr_set_gate(7, isr7, 0x8E);
    isr_set_gate(8, isr8, 0x8E);
    isr_set_gate(9, isr9, 0x8E);
    isr_set_gate(10, isr10, 0x8E);
    isr_set_gate(11, isr11, 0x8E);
    isr_set_gate(12, isr12, 0x8E);
    isr_set_gate(13, isr13, 0x8E);
    isr_set_gate(14, isr14, 0x8E);
    isr_set_gate(15, isr15, 0x8E);
    isr_set_gate(16, isr16, 0x8E);
    isr_set_gate(17, isr17, 0x8E);
    isr_set_gate(18, isr18, 0x8E);
    isr_set_gate(19, isr19, 0x8E);
    isr_set_gate(20, isr20, 0x8E);
    isr_set_gate(21, isr21, 0x8E);
    isr_set_gate(22, isr22, 0x8E);
    isr_set_gate(23, isr23, 0x8E);
    isr_set_gate(24, isr24, 0x8E);
    isr_set_gate(25, isr25, 0x8E);
    isr_set_gate(26, isr26, 0x8E);
    isr_set_gate(27, isr27, 0x8E);
    isr_set_gate(28, isr28, 0x8E);
    isr_set_gate(29, isr29, 0x8E);
    isr_set_gate(30, isr30, 0x8E);
    isr_set_gate(31, isr31, 0x8E);

    /* Hardware IRQs (ISR 32-47) */
    isr_set_gate(32, irq0, 0x8E);
    isr_set_gate(33, irq1, 0x8E);
    isr_set_gate(34, irq2, 0x8E);
    isr_set_gate(35, irq3, 0x8E);
    isr_set_gate(36, irq4, 0x8E);
    isr_set_gate(37, irq5, 0x8E);
    isr_set_gate(38, irq6, 0x8E);
    isr_set_gate(39, irq7, 0x8E);
    isr_set_gate(40, irq8, 0x8E);
    isr_set_gate(41, irq9, 0x8E);
    isr_set_gate(42, irq10, 0x8E);
    isr_set_gate(43, irq11, 0x8E);
    isr_set_gate(44, irq12, 0x8E);
    isr_set_gate(45, irq13, 0x8E);
    isr_set_gate(46, irq14, 0x8E);
    isr_set_gate(47, irq15, 0x8E);

    /* System call interrupt (0x80) - callable from user mode (DPL=3) */
    extern void syscall_entry();
    idt_set_gate(0x80, (uint64_t)syscall_entry, 0x08, 0xEE);

    /* Initialize syscall table */
    syscall_init();
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
