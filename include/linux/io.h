/*
 * linux/io.h - MMIO and port I/O accessors
 *
 * The kernel identity-maps the first 4 GiB, so ioremap() is a physical
 * pass-through: MMIO regions below 4 GiB are directly dereferenceable.
 * Port I/O forwards to the project's inline in/out helpers.
 */
#ifndef _COMPAT_LINUX_IO_H
#define _COMPAT_LINUX_IO_H

#include "drivers/io.h"
#include "lib/types.h"

/* sparse-style address-space annotation; a no-op for the compiler */
#define __iomem

static inline void __iomem* ioremap(unsigned long phys, unsigned long size) {
    (void)size;
    return (void __iomem*)phys;
}
#define ioremap_nocache ioremap
#define ioremap_wc ioremap
static inline void iounmap(void __iomem* addr) { (void)addr; }

static inline __u8  readb(const volatile void __iomem* addr) { return *(__u8*)addr; }
static inline __u16 readw(const volatile void __iomem* addr) { return *(__u16*)addr; }
static inline __u32 readl(const volatile void __iomem* addr) { return *(__u32*)addr; }
static inline void  writeb(__u8 v, volatile void __iomem* addr) { *(__u8*)addr = v; }
static inline void  writew(__u16 v, volatile void __iomem* addr) { *(__u16*)addr = v; }
static inline void  writel(__u32 v, volatile void __iomem* addr) { *(__u32*)addr = v; }

static inline void ioread8(const volatile void __iomem* a)  { (void)readb(a); }
static inline __u8  ioread8v(const volatile void __iomem* a)  { return readb(a); }
static inline __u16 ioread16v(const volatile void __iomem* a) { return readw(a); }
static inline __u32 ioread32v(const volatile void __iomem* a) { return readl(a); }
#define ioread8(a)   readb(a)
#define ioread16(a)  readw(a)
#define ioread32(a)  readl(a)
#define iowrite8(v, a)   writeb((v), (a))
#define iowrite16(v, a)  writew((v), (a))
#define iowrite32(v, a)  writel((v), (a))

/* string variants (subset) */
static inline void ioread32_rep(const volatile void __iomem* addr, void* buf, int len) {
    __u32* d = (__u32*)buf;
    while (len--) *d++ = readl(addr);
}
static inline void iowrite32_rep(volatile void __iomem* addr, const void* buf, int len) {
    const __u32* s = (const __u32*)buf;
    while (len--) writel(*s++, addr);
}

#endif /* _COMPAT_LINUX_IO_H */
