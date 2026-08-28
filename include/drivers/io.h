#ifndef IO_H
#define IO_H

#include "lib/types.h"

/* All port I/O accessors carry a "memory" clobber: they are ordering
 * barriers for the compiler. Without it, GCC is free to sink ordinary
 * stores (e.g. memcpy into a DMA TX buffer) BELOW the device kick
 * (outb/outl), so the hardware DMAs a half-written or zeroed buffer.
 * This exact bug produced all-zero RTL8139 transmit frames under -O2. */

static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    __asm__ volatile("inb %1, %0" : "=a"(data) : "dN"(port) : "memory");
    return data;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t data;
    __asm__ volatile("inw %1, %0" : "=a"(data) : "dN"(port) : "memory");
    return data;
}

static inline uint32_t ind(uint16_t port) {
    uint32_t data;
    __asm__ volatile("inl %1, %0" : "=a"(data) : "dN"(port) : "memory");
    return data;
}

static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "dN"(port) : "memory");
}

static inline void outw(uint16_t port, uint16_t data) {
    __asm__ volatile("outw %0, %1" : : "a"(data), "dN"(port) : "memory");
}

static inline void outd(uint16_t port, uint32_t data) {
    __asm__ volatile("outl %0, %1" : : "a"(data), "dN"(port) : "memory");
}

static inline void io_wait() {
    outb(0x80, 0);
}

#endif
