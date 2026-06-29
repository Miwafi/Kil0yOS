#include "core/gdt.h"
#include "core/idt.h"
#include "core/isr.h"
#include "core/interrupts.h"
#include "core/smp.h"
#include "mm/memory.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/power.h"
#include "drivers/speaker.h"
#include "shell/shell.h"
#include "fs/fs.h"
#include "drivers/device.h"
#include "timer/pit.h"
#include "sched/scheduler.h"
#include "net/netif.h"
#include "net/rtl8139.h"
#include "net/e1000.h"
#include "net/arp.h"
#include "net/udp.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init() {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static void serial_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0);
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void klog(const char* s) {
    char ts[24];
    pit_format_time(ts, sizeof(ts));
    vga_puts(ts);
    vga_puts(s);
    serial_puts(ts);
    serial_puts(s);
}

void kernel_main(uint64_t mb_info_phys) {
    serial_init();
    serial_puts("kernel_main entered\n");

    vga_init();

    vga_set_color(vga_entry_color(COLOR_LIGHT_CYAN, COLOR_BLACK));
    klog("Kil0yOS version 2.4.1\n");
    klog("Command line: (none)\n");
    vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));

    klog("GDT: loading GDT...\n");
    gdt_init();

    klog("IDT: loading IDT...\n");
    idt_init();

    klog("ISRs: registering handlers...\n");
    isr_init();

    klog("PIC: initializing 8259A...\n");
    interrupts_init();

    klog("PMM: initializing physical memory...\n");
    pmm_init(mb_info_phys);

    klog("VMM: initializing virtual memory...\n");
    vmm_init();

    klog("Memory: initializing heap...\n");
    memory_map_t map = {0};
    memory_init(&map, 1);

    klog("devtmpfs: initializing device manager...\n");
    device_init();

    klog("VFS: initializing filesystem...\n");
    fs_init();

    klog("Shell: initializing command interpreter...\n");
    shell_init();

    klog("input: keyboard initializing...\n");
    keyboard_init();

    klog("input: mouse initializing...\n");
    mouse_init();

    klog("Speaker: initializing...\n");
    speaker_init();

    klog("Scheduler: initializing round-robin scheduler...\n");
    scheduler_init();

    klog("PIT: initializing timer (100 Hz)...\n");
    pit_init(100);

    klog("ACPI: initializing...\n");
    power_init();

    klog("PCI: initializing bus...\n");
    pci_init();

    klog("net: initializing network stack...\n");
    netif_init();
    arp_init();
    udp_init();
    const char* nic = netif_probe();
    if (nic) {
        g_netif.ip      = 0x0A00020F; /* 10.0.2.15 */
        g_netif.netmask = 0xFFFFFF00; /* 255.255.255.0 */
        g_netif.gateway = 0x0A000202; /* 10.0.2.2 */
        klog("net: ");
        klog(nic);
        klog(" found\n");
        klog("net: IP=10.0.2.15 mask=255.255.255.0 gw=10.0.2.2\n");
    } else {
        klog("net: no NIC found\n");
    }

    klog("SMP: initializing multiprocessor...\n");
    smp_init();

    klog("\n");
    klog("Welcome to Kil0yOS!\n");
    klog("Type 'help' for available commands.\n\n");

    enable_interrupts();

    shell_run();
}
