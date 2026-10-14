#include "core/gdt.h"
#include "core/idt.h"
#include "core/isr.h"
#include "core/interrupts.h"
#include "core/smp.h"
#include "core/nmi_wdt.h"
#include "core/tss.h"
#include "core/process.h"
#include "mm/memory.h"
#include "drivers/video/vga.h"
#include "drivers/video/fb.h"
#include "drivers/video/efi_gop.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "drivers/pci.h"
#include "drivers/power.h"
#include "drivers/speaker.h"
#include "shell/shell.h"
#include "fs/fs.h"
#include "drivers/device.h"
#include "timer/pit.h"
#include "sched/scheduler.h"
#include "net/netif.h"
#include "net/realtek/rtl8139.h"
#include "net/intel/e1000.h"
#include "net/arp.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "net/dhcp.h"
#include "usb/usb.h"

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
    /* Bounded THR wait: if the VMware/QEMU serial back-end ever stops
     * draining (locked log file, full pipe), an unbounded spin here would
     * freeze the whole kernel at the next klog - silently, with no log
     * line to explain it.  On stall: drop the char, never hang. */
    int guard = 100000;
    while ((inb(0x3F8 + 5) & 0x20) == 0) {
        if (--guard == 0) return;
    }
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

/* Kernel-log mirror ring: every klog() byte lands here
 * so GUI panes can render recent output without touching the serial UART.
 * Power-of-two size, masked indexing; guarded by irq_save because klog is
 * legal from IRQ context (exception dump, device handlers). */
#define KLOG_RING_SIZE 8192   /* power of two */
static char klog_ring[KLOG_RING_SIZE];
static volatile uint32_t klog_wr = 0;   /* total bytes ever written */

static void klog_ring_write(const char* s) {
    int irqon = irq_save();
    disable_interrupts();
    while (*s) {
        klog_ring[klog_wr & (KLOG_RING_SIZE - 1)] = *s++;
        klog_wr++;
    }
    irq_restore(irqon);
}

/* Reader side: how many bytes have ever been logged. */
uint32_t klog_seq(void) {
    return klog_wr;
}

/* Copy log bytes with sequence >= *seq (max `max`) into dst; *seq advances
 * by the copied count. Clamps to the oldest byte still in the ring (a
 * reader that fell behind gets a mid-line start - acceptable for a view). */
uint32_t klog_copy_from(uint32_t* seq, char* dst, uint32_t max) {
    uint32_t wr = klog_wr;
    uint32_t oldest = (wr > KLOG_RING_SIZE) ? wr - KLOG_RING_SIZE : 0;
    uint32_t s = *seq;
    if (s < oldest) s = oldest;
    if (s >= wr) return 0;
    uint32_t n = wr - s;
    if (n > max) {
        n = max;
        s = wr - n;   /* keep the most recent bytes */
    }
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = klog_ring[(s + i) & (KLOG_RING_SIZE - 1)];
    }
    *seq = s + n;
    return n;
}

/* Boot console quiet flag: while set, subsystem diagnostics stay off the
 * screen (systemd-style clean boot) but still reach the serial log and the
 * GUI log ring for debugging. Cleared once the status-line boot finishes. */
static int klog_muted = 0;

/* Unconditional emit to every sink: console + GUI ring + serial. Used by
 * the boot status helpers, which must print even while diagnostics are
 * muted. */
static void klog_out(const char* s) {
    /* fb terminal takes priority when GOP grabbed the display; in VGA
     * graphics mode the text VRAM window is not mapped as text - keep
     * those logs serial-only (BIOS path behaviour unchanged).  The GOP
     * desktop additionally mutes the fb console so diagnostics never
     * paint over the desktop UI (isr.c's exception dump bypasses this). */
    if (fb_is_active() && !fb_console_muted()) {
        fb_puts(s);
    } else if (!vga_is_graphics()) {
        vga_puts(s);
    }
    klog_ring_write(s);
    serial_puts(s);
}

void klog(const char* s) {
    if (klog_muted) {
        klog_ring_write(s);
        serial_puts(s);
        return;
    }
    klog_out(s);
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

/* Boot status lines, systemd style: boot_start() prints the step label and
 * pads it to the status column; boot_ok()/boot_fail() close the line with a
 * right-aligned verdict tag, green for OK / red for FAILED. Timestamp-free
 * and console-unconditional (they bypass klog_muted). */
#define BOOT_STATUS_COL 60   /* column where the verdict tag starts */

static void boot_start(const char* label) {
    klog_out(label);
    uint32_t len = 0;
    while (label[len]) len++;
    if (len + 9 > BOOT_STATUS_COL) {   /* 9 = one space + 8-char tag */
        klog_out(" ");
        return;
    }
    for (uint32_t i = len; i < BOOT_STATUS_COL - 9; i++) klog_out(" ");
}

/* Verdict tag color on whichever console is active; boot_tag_color_reset()
 * restores the boot-screen default (fb light grey / VGA white-on-black). */
static void boot_tag_color(int ok) {
    uint8_t ega = ok ? (uint8_t)COLOR_LIGHT_GREEN : (uint8_t)COLOR_LIGHT_RED;
    if (fb_is_active() && !fb_console_muted()) {
        fb_set_color(ega);
    } else if (!vga_is_graphics()) {
        vga_set_color(vga_entry_color(ega, COLOR_BLACK));
    }
}

static void boot_tag_color_reset(void) {
    if (fb_is_active() && !fb_console_muted()) {
        fb_set_color(COLOR_GREY);   /* fb default fg 0xAAAAAA = EGA 7 */
    } else if (!vga_is_graphics()) {
        vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));
    }
}

static void boot_ok(void) {
    boot_tag_color(1);
    klog_out("[  OK  ]\n");
    boot_tag_color_reset();
}

static void boot_fail(void) {
    boot_tag_color(0);
    klog_out("[FAILED]\n");
    boot_tag_color_reset();
}

void kernel_main(uint64_t mb_info_phys) {
    serial_init();
    serial_puts("kernel_main entered\n");

    /* Clean boot screen: mute subsystem diagnostics from the very first
     * klog so the console shows only the banner + status lines. Serial and
     * the GUI log ring keep capturing everything for debugging. */
    klog_muted = 1;

    /* UEFI boots: locate GOP, pick 1024x768x32, snapshot the EFI memory
     * map and ExitBootServices - all before PMM needs the memory map.
     * BIOS boots bail out here with a single serial line, VGA untouched. */
    efi_gop_init(mb_info_phys);

    /* Start the uptime clock before any driver init so early timeout
     * decisions have a monotonic time source. IRQ0 delivery is unmasked
     * separately right after the PIC remap below. */
    pit_init(100);

    vga_init();

    vga_set_color(vga_entry_color(COLOR_LIGHT_CYAN, COLOR_BLACK));
    klog_out("Kil0yOS version 3.10.0\n");
    klog_out("Command line: (none)\n");
    vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));

    boot_start("GDT: loading GDT");
    gdt_init();
    boot_ok();

    boot_start("TSS: initializing Task State Segment");
    tss_init();
    boot_ok();

    boot_start("IDT: loading IDT");
    idt_init();
    boot_ok();

    boot_start("ISRs: registering handlers");
    isr_init();
    boot_ok();

    boot_start("PIC: initializing 8259A");
    interrupts_init();
    boot_ok();

    /* pic_init() masked all IRQ lines - unmask IRQ0 now so timer ticks
     * keep flowing once interrupts are enabled. */
    pic_enable_irq(0);

    boot_start("PMM: initializing physical memory");
    pmm_init(mb_info_phys);
    boot_ok();

    boot_start("VMM: initializing virtual memory");
    vmm_init();
    boot_ok();

    boot_start("Memory: initializing heap");
    memory_map_t map = {0};
    memory_init(&map, 1);
    boot_ok();

    boot_start("devtmpfs: initializing device manager");
    device_init();
    boot_ok();

    boot_start("VFS: initializing filesystem");
    fs_init();
    heap_verify("fs_init");
    boot_ok();

    boot_start("user: installing built-in user programs");
    user_programs_install();
    heap_verify("user_install");
    boot_ok();

    boot_start("Shell: initializing command interpreter");
    shell_init();
    heap_verify("shell_init");
    boot_ok();

    boot_start("input: keyboard initializing");
    keyboard_init();
    heap_verify("keyboard");
    boot_ok();

    boot_start("input: mouse initializing");
    mouse_init();
    heap_verify("mouse");
    boot_ok();

    /* mouse.c rewrites the 8042 CDB; force the keyboard interrupt bits
     * back on in case its CDB read consumed a stray keystroke byte */
    keyboard_rearm_interrupt();

    boot_start("Speaker: initializing");
    speaker_init();
    heap_verify("speaker");
    boot_ok();

    boot_start("Scheduler: initializing round-robin scheduler");
    scheduler_init();
    heap_verify("sched");
    boot_ok();

    boot_start("ACPI: initializing");
    power_init();
    heap_verify("acpi");
    boot_ok();

    boot_start("PCI: initializing bus");
    pci_init();
    heap_verify("pci");
    boot_ok();

    boot_start("net: initializing network stack");
    netif_init();
    arp_init();
    udp_init();
    tcp_init();
    boot_ok();

    boot_start("net: probing network interface");
    const char* nic = netif_probe();
    if (nic) {
        boot_ok();
        klog("net: ");
        klog(nic);
        klog(" found\n");

        /* Auto-configuration: try DHCP first, fall back to the classic
         * QEMU user-network static settings when no server answers. */
        boot_start("net: requesting DHCP lease");
        if (dhcp_autoconfig(&g_netif) == 0) {
            boot_ok();
        } else {
            boot_fail();
            g_netif.ip      = 0x0A00020F; /* 10.0.2.15 */
            g_netif.netmask = 0xFFFFFF00; /* 255.255.255.0 */
            g_netif.gateway = 0x0A000202; /* 10.0.2.2 */
            g_netif.dns     = 0x0A000203; /* 10.0.2.3 (slirp DNS proxy) */
            klog("net: DHCP failed, using static fallback\n");
        }
    } else {
        boot_fail();
        klog("net: no NIC found\n");
    }
    heap_verify("net");

    boot_start("USB: initializing (UHCI + HID)");
    usb_init();
    heap_verify("usb");
    boot_ok();

    boot_start("SMP: initializing multiprocessor");
    smp_init();
    boot_ok();

    boot_start("watchdog: starting kernel heartbeat thread (10s poll)");
    heartbeat_watchdog_init();
    boot_ok();

    boot_start("watchdog: arming NMI watchdog (cli-wedge coverage)");
    nmi_wdt_init();
    boot_ok();

    /* Boot finished: diagnostics back on the console for runtime messages. */
    klog_muted = 0;

    klog("\n");
    klog("Welcome to Kil0yOS!\n");
    klog("Type 'help' for available commands.\n\n");

    enable_interrupts();
    /* Anchor the tick clock to the polling clock while the polling clock is
     * still accurate (last sample was milliseconds ago). Waiting until some
     * later first caller risks anchoring to a stale/wrapped poll reading. */
    (void)pit_uptime_us();

    shell_run();
}
