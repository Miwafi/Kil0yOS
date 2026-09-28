/*
 * linux/compiler.h - branch hints, barriers and attributes
 *
 * Real memory barriers (not just compiler barriers) guard the MMIO
 * paths: mfence orders the coherent ring writes the drivers do with
 * plain loads/stores before descriptor kicks.
 */
#ifndef _COMPAT_LINUX_COMPILER_H
#define _COMPAT_LINUX_COMPILER_H

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define barrier() __asm__ __volatile__("" ::: "memory")
#define mb()  __asm__ __volatile__("mfence" ::: "memory")
#define rmb() __asm__ __volatile__("lfence" ::: "memory")
#define wmb() __asm__ __volatile__("sfence" ::: "memory")

#define __maybe_unused  __attribute__((unused))
#define __always_inline inline __attribute__((always_inline))
#define __packed        __attribute__((packed))
#define __aligned(x)    __attribute__((aligned(x)))
#define __cold          __attribute__((cold))
#define __force
#define __user
#define __rcu
#define __must_check
#define __printf(a, b)  __attribute__((format(printf, a, b)))

#endif /* _COMPAT_LINUX_COMPILER_H */
