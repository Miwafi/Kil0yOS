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
#include "linux/spinlock.h"

typedef enum irqreturn {
    IRQ_NONE  = 0,
    IRQ_HANDLED = 1,
    IRQ_WAKE_TTHREAD = 2,
} irqreturn_t;

#define IRQ_RETVAL(x) ((x) ? IRQ_HANDLED : IRQ_NONE)

#define IRQF_SHARED  0x1
#define IRQF_PROBE_SHARED 0x2

int compat_request_irq(unsigned int irq, irqreturn_t (*handler)(int, void*),
                       unsigned long flags, const char* name, void* dev);
void compat_free_irq(unsigned int irq, void* dev);

#define request_irq(irq, handler, flags, name, dev) \
    compat_request_irq((irq), (handler), (flags), (name), (dev))
#define free_irq(irq, dev) compat_free_irq((irq), (dev))

/* ---- deferred work (delayed_work) ----
 * There is no worker thread: schedule_delayed_work() queues the work
 * with a deadline and the netif main-loop poll hook executes it when
 * due (impl in compat.c). */
struct work_struct {
    void (*func)(struct work_struct* work);
};

struct delayed_work {
    struct work_struct work;
    struct list_head node;
    unsigned long deadline;   /* jiffies at which to run */
    int pending;
};

#define INIT_DELAYED_WORK(dwork, fn)                  \
    do {                                              \
        (dwork)->work.func = (fn);                    \
        (dwork)->pending = 0;                         \
        INIT_LIST_HEAD(&(dwork)->node);               \
    } while (0)
#define INIT_WORK(work, fn) ((work)->func = (fn))

int  schedule_delayed_work(struct delayed_work* dwork, unsigned long delay);
int  cancel_delayed_work_sync(struct delayed_work* dwork);
void compat_run_due_works(void);

#endif /* _COMPAT_LINUX_INTERRUPT_H */
