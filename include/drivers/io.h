#ifndef IO_H
#define IO_H

#include "lib/types.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    __asm__ volatile("inb %1, %0" : "=a"(data) : "dN"(port));
    return data;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t data;
    __asm__ volatile("inw %1, %0" : "=a"(data) : "dN"(port));
    return data;
}

static inline uint32_t ind(uint16_t port) {
    uint32_t data;
    __asm__ volatile("inl %1, %0" : "=a"(data) : "dN"(port));
    return data;
}

static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "dN"(port));
}

static inline void outw(uint16_t port, uint16_t data) {
    __asm__ volatile("outw %0, %1" : : "a"(data), "dN"(port));
}

static inline void outd(uint16_t port, uint32_t data) {
    __asm__ volatile("outl %0, %1" : : "a"(data), "dN"(port));
}

static inline void io_wait() {
    outb(0x80, 0);
}

#endif