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

/* pr_fmt: drivers may #define it before including kernel.h (8139too
 * uses KBUILD_MODNAME). pr_debug is compiled out like netdev_dbg. */
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif
#define pr_emerg(fmt, ...)  printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...)  printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)   printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)    printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)   printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning(fmt, ...) printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)   printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...)   printk(KERN_CONT fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)  do { } while (0)
#define pr_devel(fmt, ...)  do { } while (0)

/* device printk helpers: the struct device argument is dropped, the
 * message text carries the context already */
#define dev_emerg(dev, fmt, ...)  printk(KERN_ERR fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...)   printk(KERN_ERR fmt, ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...)  printk(KERN_ERR fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)    printk(KERN_ERR fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)   printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)   printk(KERN_INFO fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)    do { (void)(dev); } while (0)

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

/* error-pointer plumbing (errno values stay negative in a pointer) */
#define ERR_PTR(err)      ((void*)(long)(err))
#define PTR_ERR(ptr)      ((long)(ptr))
#define IS_ERR(ptr)       ((unsigned long)(ptr) > (unsigned long)-MAX_ERRNO)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define MAX_ERRNO 4096L

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))

#define BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2 * !!(cond)]))
#define BUG_ON(cond) do { if (cond) panic("BUG_ON " #cond, __FILE__, __LINE__, 0); } while (0)
#define WARN_ON(cond) ((cond) ? 1 : 0)

#endif /* _COMPAT_LINUX_KERNEL_H */
