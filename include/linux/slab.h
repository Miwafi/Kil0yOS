/*
 * linux/slab.h + dma-mapping.h + string.h + errno.h + delay.h
 *
 * One consolidated header covering the allocator, DMA, string and
 * error-code surface that NIC drivers actually touch. On Kil0yOS the
 * first 4 GiB are identity-mapped, so coherent DMA is simply kernel
 * memory: bus == physical == virtual address.
 */
#ifndef _COMPAT_LINUX_SLAB_H
#define _COMPAT_LINUX_SLAB_H

#include "mm/memory.h"
#include "lib/string.h"
#include "timer/pit.h"

#define GFP_KERNEL  0x01
#define GFP_ATOMIC  0x02
#define GFP_DMA     0x04
#define GFP_NOWAIT  GFP_ATOMIC
#define __GFP_ZERO  0x10
#define __GFP_DMA   GFP_DMA

static inline void* kmalloc_compat(size_t size, unsigned int flags);
void* compat_kmalloc(size_t size, unsigned int flags);
void* compat_kzalloc(size_t size, unsigned int flags);
void  compat_kfree(void* ptr);

#define kmalloc(size, flags)    compat_kmalloc((size), (flags))
#define kzalloc(size, flags)    compat_kzalloc((size), (flags))
#define kfree(ptr)              compat_kfree((ptr))
#define kcalloc(n, size, flags) compat_kzalloc((n) * (size), (flags))
#define vmalloc(size)           compat_kmalloc((size), GFP_KERNEL)
#define vfree(ptr)              compat_kfree((ptr))

/* ---- dma-mapping.h ----
 * Heap VA is not the bus address on this kernel: translate through
 * vmm_get_phys() exactly like the native drivers do. */
static inline void* dma_alloc_coherent(void* dev, size_t size,
                                       dma_addr_t* dma_handle, unsigned int flags) {
    (void)dev; (void)flags;
    void* v = compat_kzalloc(size, GFP_KERNEL);
    if (v && dma_handle) *dma_handle = (dma_addr_t)vmm_get_phys((uint64_t)v);
    return v;
}
static inline void dma_free_coherent(void* dev, size_t size,
                                     void* vaddr, dma_addr_t dma_handle) {
    (void)dev; (void)size; (void)dma_handle;
    compat_kfree(vaddr);
}
static inline dma_addr_t dma_map_single(void* dev, void* ptr, size_t size, int dir) {
    (void)dev; (void)size; (void)dir;
    return (dma_addr_t)vmm_get_phys((uint64_t)ptr);
}
static inline void dma_unmap_single(void* dev, dma_addr_t a, size_t size, int dir) {
    (void)dev; (void)a; (void)size; (void)dir;
}
static inline int dma_mapping_error(void* dev, dma_addr_t a) { (void)dev; return a == 0; }

#define DMA_TO_DEVICE 1
#define DMA_FROM_DEVICE 2
#define DMA_BIDIRECTIONAL 0

/* ---- errno.h ---- */
#define EINVAL 22
#define ENOMEM 12
#define EBUSY  16
#define EAGAIN 11
#define ENODEV 19
#define EIO    5
#define ENOSPC 28
#define ETIMEDOUT 110
#define EOPNOTSUPP 95
#define EPERM  1
#define EFAULT 14

/* ---- delay.h (busy-wait; IRQ contexts in a panic-safe kernel) ---- */
static inline void udelay(unsigned long usec) {
    uint64_t end = pit_uptime_us() + usec;
    while (pit_uptime_us() < end) __asm__ volatile("pause");
}
static inline void mdelay(unsigned long msec) {
    uint64_t end = pit_uptime_us() + (uint64_t)msec * 1000;
    while (pit_uptime_us() < end) __asm__ volatile("pause");
}
static inline void msleep(unsigned long msec) { mdelay(msec); }
static inline void ssleep(unsigned long sec) { mdelay(sec * 1000); }
static inline void ndelay(unsigned long nsec) { (void)nsec; }

/* linux/ioport.h essentials */
static inline int request_mem_region(unsigned long s, unsigned long l, const char* n) { (void)s;(void)l;(void)n; return 0; }
static inline void release_mem_region(unsigned long s, unsigned long l) { (void)s;(void)l; }

#endif /* _COMPAT_LINUX_SLAB_H */
