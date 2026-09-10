#include "core/interrupts.h"
#include "drivers/io.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define PIC_EOI      0x20

static uint64_t apic_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void apic_wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val),
                     "d"((uint32_t)(val >> 32)));
}

/* 8259 IRQs only reach the CPU through the LAPIC when LVT LINT0 is in
 * ExtINT delivery mode (virtual wire).  UEFI firmware enables the LAPIC
 * (xAPIC or x2APIC) and masks LINT0 once its own drivers move to the I/O
 * APIC - after that NO 8259 interrupt is ever delivered no matter how the
 * PIC is programmed (2.17.1 symptom: shell up, keyboard dead on the
 * parked-firmware VMware path; the 8042 was fine, the IRQ just never
 * arrived).  ExtINT delivery bypasses the LAPIC IRR/ISR, so the plain
 * 8259 EOI in pic_send_eoi() stays sufficient.  Classic BIOS runs pure
 * virtual wire (LAPIC disabled or LINT0 already ExtINT): unaffected. */
static void pic_lapic_virtual_wire(void) {
    uint64_t base = apic_rdmsr(0x1B);
    if (!(base & (1ULL << 11))) return;    /* LAPIC globally disabled */
    const uint32_t extint = 7u << 8;       /* delivery=ExtINT, unmasked */
    if (base & (1ULL << 10)) {             /* EXT bit: x2APIC -> MSR */
        apic_wrmsr(0x835, extint);         /* 0x800 + 0x350/0x10 = LINT0 */
        klog("[pic] lint0=ExtINT via x2APIC msr\n");
    } else {                               /* xAPIC -> MMIO */
        volatile uint32_t* lapic =
            (volatile uint32_t*)(unsigned long)(base & 0xFFFFFFFF000ULL);
        lapic[0x350 / 4] = extint;
        klog("[pic] lint0=ExtINT via xAPIC mmio\n");
    }
}

void pic_init() {
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    
    outb(PIC1_DATA, IRQ0);
    outb(PIC2_DATA, IRQ8);
    
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    pic_lapic_virtual_wire();
}

void pic_enable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq >= 16) return; /* invalid/unknown PCI line: leave masked */
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
        uint8_t master = inb(PIC1_DATA) & ~(1 << 2);
        outb(PIC1_DATA, master);
    }

    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

void pic_disable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = inb(port) | (1 << irq);
    outb(port, value);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void interrupts_init() {
    pic_init();
}

void enable_interrupts() {
    __asm__ volatile("sti");
}

void disable_interrupts() {
    __asm__ volatile("cli");
}