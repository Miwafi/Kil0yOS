/*
 * linux/kernel.h - printk and misc helpers on Kil0yOS
 *
 * printk() goes through the kernel log (klog) using the project's
 * kvsnprintf formatter. jiffies is derived from the PIT uptime so that
 * drivers get a monotonic 100 Hz tick without touching the timer code.
 */
#ifndef _COMPAT_LINUX_KERNEL_H
#define _COMPAT_LINUX_KERNEL_H

#include <stdarg.h>
#include "lib/types.h"
#include "linux/types.h"

#define KERN_INFO     ""
#define KERN_NOTICE   ""
#define KERN_WARNING  ""
#define KERN_ERR      ""
#define KERN_DEBUG    ""
#define KERN_CONT     ""

int printk(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void panic(const char* msg, const char* file, int line, uint64_t code);

unsigned long compat_jiffies(void);
#define HZ 100
#define jiffies compat_jiffies()
#define msecs_to_jiffies(m) ((unsigned long)(m) / (1000 / HZ))
#define jiffies_to_msecs(j) ((unsigned long)(j) * (1000 / HZ))
#define time_after(a, b)   ((long)(b) - (long)(a) < 0)
#define time_before(a, b)  time_after(b, a)
#define time_after_eq(a, b) ((long)(a) - (long)(b) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)

#define container_of(ptr, type, member) \
    ((type*)((char*)(ptr) - __builtin_offsetof(type, member)))

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))

#define BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2 * !!(cond)]))
#define BUG_ON(cond) do { if (cond) panic("BUG_ON " #cond, __FILE__, __LINE__, 0); } while (0)
#define WARN_ON(cond) ((cond) ? 1 : 0)

#endif /* _COMPAT_LINUX_KERNEL_H */
