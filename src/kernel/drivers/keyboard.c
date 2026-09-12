#include "drivers/keyboard.h"
#include "drivers/io.h"
#include "core/isr.h"
#include "core/interrupts.h"
#include "drivers/device.h"
#include "drivers/vga.h"

#define BUFFER_SIZE 256
#define KEYBOARD_STATUS_PORT 0x64

static char keyboard_buffer[BUFFER_SIZE];
static int buffer_head = 0;
static int buffer_tail = 0;
static int buffer_count = 0;

static int shift_pressed = 0;
static int caps_lock = 0;
static int ctrl_pressed = 0;
static int extended = 0;

static const char scancode_map[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,  ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  '7', '8', '9', '-', '4', '5', '6', '+',
    '1', '2', '3', '0', '.'
};

static const char scancode_map_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,  ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  '7', '8', '9', '-', '4', '5', '6', '+',
    '1', '2', '3', '0', '.'
};

static void buf_push(char c) {
    /* irq_save/restore (NOT disable/enable): this runs from the PS/2 IRQ,
     * the UHCI IRQ and the IRQ0 tick - all of which must keep IF=0. */
    int irqon = irq_save();
    disable_interrupts();
    if (buffer_count < BUFFER_SIZE) {
        keyboard_buffer[buffer_head] = c;
        buffer_head = (buffer_head + 1) % BUFFER_SIZE;
        buffer_count++;
    }
    irq_restore(irqon);
}

static void process_scancode(uint8_t scancode) {
    if (scancode == 0x58) {            /* F12: menu key too. Special-cased
                                        * before the char map (headless QEMU
                                        * cannot inject the E0 5D menu key,
                                        * but f12 is a valid sendkey qcode) */
        buf_push(KEY_WIN);
        return;
    }
    if (scancode == 42 || scancode == 54) {
        shift_pressed = 1;
        return;
    }
    if (scancode == 58) {
        caps_lock ^= 1;
        return;
    }
    if (scancode == 29) {
        ctrl_pressed = 1;
        return;
    }

    if (scancode >= sizeof(scancode_map)) {
        return;
    }

    char c = scancode_map[scancode];
    if (c != 0) {
        if (ctrl_pressed && c >= 'a' && c <= 'z') {
            c = c - 'a' + 1;
        } else if (c >= 'a' && c <= 'z') {
            /* Letter case: shift XOR caps lock (classic PC behaviour). */
            if (shift_pressed ^ caps_lock) c = scancode_map_shift[scancode];
        } else if (shift_pressed) {
            /* Punctuation/number shifted forms (: " < > ? _ ! ...) - the
             * driver previously shifted only letters/digits, so URLs with
             * ':' typed over sendkey came out as ';'. */
            char sc = scancode_map_shift[scancode];
            if (sc != 0) c = sc;
        }

        buf_push(c);
    }
}

void keyboard_feed_scancode(uint8_t sc, int ext) {
    if (ext) {
        /* extended make/release block (0xE0-prefixed on PS/2; the USB HID
         * driver feeds these directly with ext=1) */
        if (sc & 0x80) return;             /* extended releases are unused */
        if (sc == 0x53)      buf_push('\b');
        else if (sc == 0x48) buf_push(0x80);
        else if (sc == 0x50) buf_push(0x81);
        else if (sc == 0x4B) buf_push(0x82);
        else if (sc == 0x4D) buf_push(0x83);
        else if (sc == 0x5B || sc == 0x5C || sc == 0x5D) buf_push(KEY_WIN);
        /* 0x5B/0x5C = Win keys, 0x5D = Menu/Application key: both open the
         * desktop start-menu popup (headless QEMU can only inject `menu`). */
        return;
    }
    if (sc & 0x80) {
        sc &= ~0x80;
        if (sc == 42 || sc == 54) shift_pressed = 0;
        if (sc == 29)             ctrl_pressed = 0;
        return;
    }
    process_scancode(sc);
}

static int ps2_enabled = 1;

void keyboard_set_ps2_enabled(int enabled) {
    ps2_enabled = enabled;
}

void keyboard_handler(interrupt_frame_t* frame) {
    (void)frame;
    static int dbg_n = 0;
    uint8_t scancode = inb(KEYBOARD_PORT);

    /* remote-debug: proves IRQ1 delivery through the LAPIC virtual wire
     * (UEFI firmware masks LINT0 and silently kills all 8259 IRQs) */
    if (dbg_n < 12) {
        dbg_n++;
        klog_hex("[kbd] irq1 sc=", scancode);
    }

    if (!ps2_enabled) {
        /* USB keyboard owns the console: drain the 8042 output buffer and
         * discard, but keep the IRQ serviced */
        while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
            (void)inb(KEYBOARD_PORT);
        }
        pic_send_eoi(KEYBOARD_IRQ);
        return;
    }

    if (scancode == 0xE0) {
        extended = 1;
        pic_send_eoi(KEYBOARD_IRQ);
        return;
    }

    if (extended) {
        extended = 0;
        keyboard_feed_scancode(scancode, 1);
        pic_send_eoi(KEYBOARD_IRQ);
        return;
    }

    keyboard_feed_scancode(scancode, 0);
    pic_send_eoi(KEYBOARD_IRQ);
}

/* Bounded waits + explicit 8042 controller programming, mirroring the
 * mouse bring-up rule.  A parked UEFI firmware (e.g. VMware) may leave
 * the keyboard port with OBF interrupts disabled (CDB bit0 = 0) or the
 * keyboard clock gated (bit4 = 1) because its own driver was poll-based;
 * the 8042 then buffers bytes but IRQ1 never fires - the shell comes up
 * and typing does nothing.  Threshold >= 1000000 (ISA port reads ~1us
 * each, 2M is ~2s worst case). */
#define KBD_WAIT_LOOPS 2000000
/* Device-command ACKs: a real keyboard answers in well under 1ms.  On
 * VMware every port read is a VM-exit (~10x slower than QEMU), so a 2M
 * loop poll for an ACK that never comes stalls the boot for tens of
 * seconds - that was the "press any key to continue boot" hang.  100k
 * loops is ~100ms on QEMU / ~1s on VMware: still 100x over a real ACK. */
#define KBD_ACK_LOOPS  100000

static int kbd_wait_output(void) {
    uint32_t n = KBD_WAIT_LOOPS;
    while ((inb(KEYBOARD_STATUS_PORT) & 0x01) == 0) {
        if (--n == 0) return 0;
        __asm__ volatile("nop");
    }
    return 1;
}

/* Wait for a device response byte (ACK / self-test result). */
static int kbd_wait_ack(void) {
    uint32_t n = KBD_ACK_LOOPS;
    while ((inb(KEYBOARD_STATUS_PORT) & 0x01) == 0) {
        if (--n == 0) return 0;
        __asm__ volatile("nop");
    }
    return 1;
}

static int kbd_wait_input(void) {
    uint32_t n = KBD_WAIT_LOOPS;
    while (inb(KEYBOARD_STATUS_PORT) & 0x02) {
        if (--n == 0) return 0;
        __asm__ volatile("nop");
    }
    return 1;
}

static void kbd_flush(void) {
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        (void)inb(KEYBOARD_PORT);
    }
}

/* Program the controller and the device: CDB (OBF int on, kbd clock on),
 * device reset FF -> FA AA 00, enable scanning F4 -> FA.  Returns 1 when
 * the device ACKed enable-scanning; 0 logs a degraded path (the handler
 * still gets registered and USB HID may claim the console later). */
static int keyboard_controller_reset(void) {
    uint8_t cdb, b;

    /* IRQs OFF for the whole 8042 command sequence: with KIE set, the CDB
     * reply and every device ACK raise IRQ1 and would be consumed by
     * whatever handler is live before our polled read sees the byte. */
    int irqon = irq_save();
    disable_interrupts();

    kbd_flush();

    /* controller config byte: set keyboard OBF-int (bit0), clear the
     * keyboard-clock disable (bit4).  Leave translation (bit6) and the
     * aux bits (1/5) exactly as found - mouse.c rewrites those itself. */
    outb(KEYBOARD_STATUS_PORT, 0x20);              /* read CDB */
    if (!kbd_wait_output()) {
        klog("[kbd] 8042 config read timeout\n");
        irq_restore(irqon);
        return 0;
    }
    cdb = inb(KEYBOARD_PORT);
    klog_hex("[kbd] cdb in =", cdb);
    klog("\n");
    cdb |= 0x01;
    cdb &= ~0x10;

    if (!kbd_wait_input()) {
        klog("[kbd] 8042 ibf stuck\n");
        irq_restore(irqon);
        return 0;
    }
    outb(KEYBOARD_STATUS_PORT, 0x60);              /* write CDB */
    if (!kbd_wait_input()) {
        klog("[kbd] 8042 cfg write stuck\n");
        irq_restore(irqon);
        return 0;
    }
    outb(KEYBOARD_PORT, cdb);

    /* device-level reset: FF -> ACK(FA) -> self-test AA 00.  Recovers a
     * device left in the firmware's own mode. */
    if (!kbd_wait_input()) {
        irq_restore(irqon);
        return 0;
    }
    outb(KEYBOARD_PORT, 0xFF);
    if (!kbd_wait_ack()) {
        klog("[kbd] device reset timeout\n");
    } else if (inb(KEYBOARD_PORT) != 0xFA) {
        klog("[kbd] reset ack unexpected\n");
        kbd_flush();
    } else {
        if (kbd_wait_ack() && inb(KEYBOARD_PORT) == 0xAA) {
            if (kbd_wait_ack()) (void)inb(KEYBOARD_PORT);   /* 0x00 */
        }
    }

    kbd_flush();
    if (!kbd_wait_input()) {
        irq_restore(irqon);
        return 0;
    }
    outb(KEYBOARD_PORT, 0xF4);                     /* enable scanning */
    if (!kbd_wait_ack()) {
        klog("[kbd] enable-scanning timeout\n");
        irq_restore(irqon);
        return 0;
    }
    b = inb(KEYBOARD_PORT);
    if (b != 0xFA) {
        klog_hex("[kbd] enable ack=0x", b);
        klog("\n");
        irq_restore(irqon);
        return 0;
    }
    irq_restore(irqon);
    return 1;
}

static int keyboard_device_open(device_t* dev) {
    return 0;
}

static int keyboard_device_close(device_t* dev) {
    return 0;
}

static int keyboard_device_read(device_t* dev, void* buffer, size_t size) {
    if (size == 0 || buffer == NULL) return -1;
    char* buf = (char*)buffer;
    int count = 0;
    
    disable_interrupts();
    while (count < (int)size && buffer_count > 0) {
        buf[count++] = keyboard_buffer[buffer_tail];
        buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
        buffer_count--;
    }
    enable_interrupts();
    
    return count;
}

static int keyboard_device_write(device_t* dev, const void* buffer, size_t size) {
    return -1;
}

static int keyboard_device_ioctl(device_t* dev, int cmd, void* arg) {
    return -1;
}

static device_t keyboard_device = {
    .name = "keyboard",
    .type = DEVICE_TYPE_KEYBOARD,
    .open = keyboard_device_open,
    .close = keyboard_device_close,
    .read = keyboard_device_read,
    .write = keyboard_device_write,
    .ioctl = keyboard_device_ioctl
};

void keyboard_init() {
    if (keyboard_controller_reset()) {
        klog("[kbd] init ok\n");
    } else {
        /* degraded: no ACK.  Keep going - the handler still drains stray
         * bytes and a USB HID keyboard may claim the console later. */
        klog("[kbd] init degraded\n");
    }
    register_irq_handler(KEYBOARD_IRQ, keyboard_handler);
    pic_enable_irq(KEYBOARD_IRQ);
    device_register(&keyboard_device);
}

/* Rewrite the 8042 command byte with the keyboard interrupt bits FORCED
 * on (OBF-int bit0=1, clock-disable bit4=0).  Called from kernel_main
 * right after mouse_init(): mouse.c rewrites the CDB itself, and its
 * reply read does not check the AUX flag - a stray keystroke consumed as
 * the CDB reply once poisoned the byte (e.g. 0x92 -> bit0=0 kbd int off),
 * after which the shell receives no IRQ1 at all.  Controller-level read,
 * fast on both QEMU and VMware. */
void keyboard_rearm_interrupt(void) {
    uint8_t cdb;

    /* IRQs OFF: the CDB reply raises IRQ1 and would be stolen before the
     * polled read below sees it (same race as the init path). */
    int irqon = irq_save();
    disable_interrupts();

    kbd_flush();
    outb(KEYBOARD_STATUS_PORT, 0x20);              /* read CDB */
    if (!kbd_wait_output()) {
        irq_restore(irqon);
        return;
    }
    cdb = inb(KEYBOARD_PORT);
    klog_hex("[kbd] rearm cdb read =", cdb);

    if ((cdb & 0x01) && !(cdb & 0x10)) {
        irq_restore(irqon);
        return;                                    /* already correct */
    }

    cdb |= 0x01;
    cdb &= ~0x10;
    if (!kbd_wait_input()) {
        irq_restore(irqon);
        return;
    }
    outb(KEYBOARD_STATUS_PORT, 0x60);              /* write CDB */
    if (!kbd_wait_input()) {
        irq_restore(irqon);
        return;
    }
    outb(KEYBOARD_PORT, cdb);
    klog_hex("[kbd] rearm cdb written =", cdb);
    irq_restore(irqon);
}

char keyboard_getc() {
    while (1) {
        disable_interrupts();
        int count = buffer_count;
        if (count > 0) {
            char c = keyboard_buffer[buffer_tail];
            buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
            buffer_count = count - 1;
            enable_interrupts();
            return c;
        }
        enable_interrupts();
        __asm__ volatile("hlt");
    }
}

int keyboard_has_input() {
    disable_interrupts();
    int count = buffer_count;
    enable_interrupts();
    return count > 0;
}