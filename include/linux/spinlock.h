/*
 * linux/spinlock.h - uniprocessor no-op locking
 *
 * Kil0yOS drivers run either in the single main loop or in an ISR; the
 * PIC will not re-deliver the same line before EOI, so lock operations
 * degrade to compiler barriers. _irqsave variants still disable the
 * interrupt flag around the critical section for ISR safety.
 */
#ifndef _COMPAT_LINUX_SPINLOCK_H
#define _COMPAT_LINUX_SPINLOCK_H

#include "linux/compiler.h"

typedef struct { unsigned int s; } spinlock_t;
typedef struct { unsigned int s; } raw_spinlock_t;

static inline void spin_lock_init(spinlock_t* l)      { l->s = 0; }
static inline void spin_lock(spinlock_t* l)           { (void)l; barrier(); }
static inline void spin_unlock(spinlock_t* l)         { (void)l; barrier(); }
static inline void spin_lock_bh(spinlock_t* l)        { spin_lock(l); }
static inline void spin_unlock_bh(spinlock_t* l)      { spin_unlock(l); }

static inline void spin_lock_irq(spinlock_t* l) {
    __asm__ __volatile__("cli" ::: "memory");
    spin_lock(l);
}
static inline void spin_unlock_irq(spinlock_t* l) {
    spin_unlock(l);
    __asm__ __volatile__("sti" ::: "memory");
}
static inline unsigned long __spin_lock_irqsave(spinlock_t* l) {
    unsigned long flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory", "cc");
    spin_lock(l);
    return flags;
}
static inline void __spin_unlock_irqrestore(spinlock_t* l, unsigned long flags) {
    spin_unlock(l);
    __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}
#define spin_lock_irqsave(lock, flags)  ((flags) = __spin_lock_irqsave(lock))
#define spin_unlock_irqrestore(lock, flags) __spin_unlock_irqrestore((lock), (flags))

/* RCU / netdev-wide locks: uniprocessor no-ops */
static inline void synchronize_rcu(void) { barrier(); }
static inline void rcu_read_lock(void)   { barrier(); }
static inline void rcu_read_unlock(void) { barrier(); }

#endif /* _COMPAT_LINUX_SPINLOCK_H */
