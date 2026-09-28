/*
 * linux/interrupt.h - IRQ registration bridging to Kil0yOS ISR core
 *
 * request_irq(irq, handler, flags, name, dev) registers a compat
 * wrapper that calls the driver handler and then transmits the PIC
 * EOI, so drivers only need to return IRQ_HANDLED.
 */
#ifndef _COMPAT_LINUX_INTERRUPT_H
#define _COMPAT_LINUX_INTERRUPT_H

#include "core/isr.h"
#include "linux/types.h"

typedef enum irqreturn {
    IRQ_NONE  = 0,
    IRQ_HANDLED = 1,
    IRQ_WAKE_TTHREAD = 2,
} irqreturn_t;

#define IRQF_SHARED  0x1
#define IRQF_PROBE_SHARED 0x2

int compat_request_irq(unsigned int irq, irqreturn_t (*handler)(int, void*),
                       unsigned long flags, const char* name, void* dev);
void compat_free_irq(unsigned int irq, void* dev);

#define request_irq(irq, handler, flags, name, dev) \
    compat_request_irq((irq), (handler), (flags), (name), (dev))
#define free_irq(irq, dev) compat_free_irq((irq), (dev))

#endif /* _COMPAT_LINUX_INTERRUPT_H */
