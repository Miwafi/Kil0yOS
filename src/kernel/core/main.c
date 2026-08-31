#include "core/gdt.h"
#include "core/idt.h"
#include "core/isr.h"
#include "core/interrupts.h"
#include "core/smp.h"
#include "core/tss.h"
#include "core/process.h"
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
#include "net/tcp.h"
#include "net/dhcp.h"

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
    /* in graphics mode the text VRAM window is not mapped as text - keep logs serial-only */
    if (!vga_is_graphics()) {
        vga_puts(ts);
        vga_puts(s);
    }
    serial_puts(ts);
    serial_puts(s);
}

void klog_hex(const char* prefix, uint64_t v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        uint8_t nib = (uint8_t)(v >> (60 - i * 4)) & 0xF;
        buf[2 + i] = nib < 10 ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[18] = '\0';
    klog(prefix);
    klog(buf);
    klog("\n");
}

void kernel_main(uint64_t mb_info_phys) {
    serial_init();
    serial_puts("kernel_main entered\n");

    /* Start the timestamp clock before any klog() so early boot logs get
     * monotonic timestamps. IRQ0 delivery is unmasked separately right
     * after the PIC remap below. */
    pit_init(100);

    vga_init();

    vga_set_color(vga_entry_color(COLOR_LIGHT_CYAN, COLOR_BLACK));
    klog("Kil0yOS version 2.14.0\n");
    klog("Command line: (none)\n");
    vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));

    klog("GDT: loading GDT...\n");
    gdt_init();

    klog("TSS: initializing Task State Segment...\n");
    tss_init();

    klog("IDT: loading IDT...\n");
    idt_init();

    klog("ISRs: registering handlers...\n");
    isr_init();

    klog("PIC: initializing 8259A...\n");
    interrupts_init();

    /* pic_init() masked all IRQ lines - unmask IRQ0 now so timer ticks
     * (and timestamps) keep flowing once interrupts are enabled. */
    pic_enable_irq(0);

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
    heap_verify("fs_init");

    klog("user: installing built-in user programs...\n");
    user_programs_install();
    heap_verify("user_install");

    klog("Shell: initializing command interpreter...\n");
    shell_init();
    heap_verify("shell_init");
    klog("[init] shell_init done\n");

    klog("input: keyboard initializing...\n");
    keyboard_init();
    heap_verify("keyboard");
    klog("[init] keyboard_init done\n");

    klog("input: mouse initializing...\n");
    mouse_init();
    heap_verify("mouse");
    klog("[init] mouse_init done\n");

    klog("Speaker: initializing...\n");
    speaker_init();
    heap_verify("speaker");
    klog("[init] speaker_init done\n");

    klog("Scheduler: initializing round-robin scheduler...\n");
    scheduler_init();
    heap_verify("sched");
    klog("[init] scheduler_init done\n");

    klog("ACPI: initializing...\n");
    power_init();
    heap_verify("acpi");
    klog("[init] power_init done\n");

    klog("PCI: initializing bus...\n");
    pci_init();
    heap_verify("pci");
    klog("[init] pci_init done\n");

    klog("net: initializing network stack...\n");
    netif_init();
    arp_init();
    udp_init();
    tcp_init();
    const char* nic = netif_probe();
    if (nic) {
        klog("net: ");
        klog(nic);
        klog(" found\n");

        /* Auto-configuration: try DHCP first, fall back to the classic
         * QEMU user-network static settings when no server answers. */
        if (dhcp_autoconfig(&g_netif) == 0) {
            klog("net: DHCP ok\n");
        } else {
            g_netif.ip      = 0x0A00020F; /* 10.0.2.15 */
            g_netif.netmask = 0xFFFFFF00; /* 255.255.255.0 */
            g_netif.gateway = 0x0A000202; /* 10.0.2.2 */
            g_netif.dns     = 0x0A000203; /* 10.0.2.3 (slirp DNS proxy) */
            klog("net: DHCP failed, using static fallback\n");
        }
        klog("net: configured\n");
    } else {
        klog("net: no NIC found\n");
    }
    klog("[init] net done\n");
    heap_verify("net");

    klog("SMP: initializing multiprocessor...\n");
    smp_init();
    klog("[init] smp_init done\n");

    klog("\n");
    klog("Welcome to Kil0yOS!\n");
    klog("Type 'help' for available commands.\n\n");
    klog("[init] before enable_interrupts\n");

    enable_interrupts();
    klog("[init] interrupts enabled\n");
    /* Anchor the tick clock to the polling clock while the polling clock is
     * still accurate (last sample was milliseconds ago). Waiting until some
     * later first caller risks anchoring to a stale/wrapped poll reading. */
    (void)pit_uptime_us();

    shell_run();
}
