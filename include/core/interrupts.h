#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "lib/types.h"
#include "core/idt.h"

#define IRQ0  32
#define IRQ1  33
#define IRQ2  34
#define IRQ3  35
#define IRQ4  36
#define IRQ5  37
#define IRQ6  38
#define IRQ7  39
#define IRQ8  40
#define IRQ9  41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

void interrupts_init();
void pic_init();
void pic_enable_irq(uint8_t irq);
void pic_disable_irq(uint8_t irq);
void pic_send_eoi(uint8_t irq);
void enable_interrupts();
void disable_interrupts();

/* Short critical sections shared between ISR and poll contexts must save
 * and restore the IF flag instead of blindly sti()-ing: the same code
 * runs with interrupts already disabled when entered from an ISR. */
static inline int irq_save(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    return (int)((rflags >> 9) & 1);
}

static inline void irq_restore(int enabled) {
    if (enabled) __asm__ volatile("sti" ::: "memory");
    else         __asm__ volatile("cli" ::: "memory");
}

#endif