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
    if (dbg_n < 2) {
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

static void keyboard_controller_reset() {
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        inb(KEYBOARD_PORT);
    }
    
    outb(KEYBOARD_PORT, 0xFF);
    io_wait();
    
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        inb(KEYBOARD_PORT);
    }
    
    outb(KEYBOARD_PORT, 0xF4);
    io_wait();
    
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        inb(KEYBOARD_PORT);
    }
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
    keyboard_controller_reset();
    register_irq_handler(KEYBOARD_IRQ, keyboard_handler);
    pic_enable_irq(KEYBOARD_IRQ);
    device_register(&keyboard_device);
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