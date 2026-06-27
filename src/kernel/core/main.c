#include "core/gdt.h"
#include "core/idt.h"
#include "core/isr.h"
#include "core/interrupts.h"
#include "core/smp.h"
#include "mm/memory.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/power.h"
#include "drivers/pci.h"
#include "drivers/speaker.h"
#include "shell/shell.h"
#include "fs/fs.h"
#include "drivers/device.h"
#include "timer/pit.h"
#include "sched/scheduler.h"

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

void kernel_main(uint64_t mb_info_phys) {
    serial_init();
    serial_puts("kernel_main entered\n");

    vga_init();

    vga_set_color(vga_entry_color(COLOR_LIGHT_CYAN, COLOR_BLACK));
    vga_puts("Kil0yOS v2.3.0\n");
    vga_puts("==================================\n");
    vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));

    vga_puts("[GDT] Initializing\n");
    serial_puts("[GDT] Initializing\n");
    gdt_init();
    vga_puts("[GDT] OK\n");
    serial_puts("[GDT] OK\n");

    vga_puts("[IDT] Initializing\n");
    serial_puts("[IDT] Initializing\n");
    idt_init();
    vga_puts("[IDT] OK\n");
    serial_puts("[IDT] OK\n");

    vga_puts("[ISRs] Initializing\n");
    serial_puts("[ISRs] Initializing\n");
    isr_init();
    vga_puts("[ISRs] OK\n");
    serial_puts("[ISRs] OK\n");

    vga_puts("[PIC] Initializing\n");
    serial_puts("[PIC] Initializing\n");
    interrupts_init();
    vga_puts("[PIC] OK\n");
    serial_puts("[PIC] OK\n");

    vga_puts("[PMM] Initializing\n");
    serial_puts("[PMM] Initializing\n");
    pmm_init(mb_info_phys);
    vga_puts("[PMM] OK\n");
    serial_puts("[PMM] OK\n");

    vga_puts("[VMM] Initializing\n");
    serial_puts("[VMM] Initializing\n");
    vmm_init();
    vga_puts("[VMM] OK\n");
    serial_puts("[VMM] OK\n");

    vga_puts("[Memory] Initializing heap\n");
    serial_puts("[Memory] Initializing heap\n");
    memory_map_t map = {0};
    memory_init(&map, 1);
    vga_puts("[Memory] OK\n");
    serial_puts("[Memory] OK\n");

    vga_puts("[DeviceManager] Initializing\n");
    serial_puts("[DeviceManager] Initializing\n");
    device_init();
    vga_puts("[DeviceManager] OK\n");
    serial_puts("[DeviceManager] OK\n");

    vga_puts("[Filesystem] Initializing\n");
    serial_puts("[Filesystem] Initializing\n");
    fs_init();
    vga_puts("[Filesystem] OK\n");
    serial_puts("[Filesystem] OK\n");

    vga_puts("[Shell] Initializing\n");
    serial_puts("[Shell] Initializing\n");
    shell_init();
    vga_puts("[Shell] OK\n");
    serial_puts("[Shell] OK\n");

    vga_puts("[Keyboard] Initializing\n");
    serial_puts("[Keyboard] Initializing\n");
    keyboard_init();
    vga_puts("[Keyboard] OK\n");
    serial_puts("[Keyboard] OK\n");

    vga_puts("[Mouse] Initializing\n");
    serial_puts("[Mouse] Initializing\n");
    mouse_init();
    vga_puts("[Mouse] OK\n");
    serial_puts("[Mouse] OK\n");

    vga_puts("[Speaker] Initializing\n");
    serial_puts("[Speaker] Initializing\n");
    speaker_init();
    vga_puts("[Speaker] OK\n");
    serial_puts("[Speaker] OK\n");

    vga_puts("[Scheduler] Initializing\n");
    serial_puts("[Scheduler] Initializing\n");
    scheduler_init();
    vga_puts("[Scheduler] OK\n");
    serial_puts("[Scheduler] OK\n");

    vga_puts("[PIT] Initializing\n");
    serial_puts("[PIT] Initializing\n");
    pit_init(100);
    vga_puts("[PIT] OK\n");
    serial_puts("[PIT] OK\n");

    vga_puts("[Power] Initializing ACPI...\n");
    serial_puts("[Power] Initializing ACPI...\n");
    power_init();
    vga_puts("[Power] OK\n");
    serial_puts("[Power] OK\n");

    vga_puts("[PCI] Initializing\n");
    serial_puts("[PCI] Initializing\n");
    pci_init();
    vga_puts("[PCI] OK\n");
    serial_puts("[PCI] OK\n");

    vga_puts("[SMP] Initializing\n");
    serial_puts("[SMP] Initializing\n");
    smp_init();
    vga_puts("[SMP] OK\n");
    serial_puts("[SMP] OK\n");

    vga_puts("\nWelcome!\n");
    serial_puts("\nWelcome!\n");
    vga_puts("Type 'help' for available commands.\n\n");
    serial_puts("Type 'help' for available commands.\n\n");

    enable_interrupts();

    shell_run();
}