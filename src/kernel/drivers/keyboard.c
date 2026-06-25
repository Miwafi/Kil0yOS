#include "drivers/keyboard.h"
#include "drivers/io.h"
#include "core/isr.h"
#include "core/interrupts.h"
#include "drivers/device.h"

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
        } else if ((shift_pressed ^ caps_lock) && c >= 'a' && c <= 'z') {
            c = scancode_map_shift[scancode];
        } else if (shift_pressed && c >= '1' && c <= '9') {
            c = scancode_map_shift[scancode];
        } else if (shift_pressed && c == '0') {
            c = scancode_map_shift[scancode];
        }
        
        if (buffer_count < BUFFER_SIZE) {
            disable_interrupts();
            keyboard_buffer[buffer_head] = c;
            buffer_head = (buffer_head + 1) % BUFFER_SIZE;
            buffer_count++;
            enable_interrupts();
        }
    }
}

void keyboard_handler(interrupt_frame_t* frame) {
    uint8_t scancode = inb(KEYBOARD_PORT);
    
    if (scancode == 0xE0) {
        extended = 1;
        pic_send_eoi(KEYBOARD_IRQ);
        return;
    }
    
    if (scancode & 0x80) {
        scancode &= ~0x80;
        if (scancode == 42 || scancode == 54) {
            shift_pressed = 0;
        }
        if (scancode == 29) {
            ctrl_pressed = 0;
        }
        /* Only clear extended flag for extended key releases */
        if (scancode == 0xE0) {
            extended = 0;
        }
        pic_send_eoi(KEYBOARD_IRQ);
        return;
    }
    
    if (extended) {
        if (scancode == 0x53) {
            if (buffer_count < BUFFER_SIZE) {
                disable_interrupts();
                keyboard_buffer[buffer_head] = '\b';
                buffer_head = (buffer_head + 1) % BUFFER_SIZE;
                buffer_count++;
                enable_interrupts();
            }
        } else if (scancode == 0x48) {
            if (buffer_count < BUFFER_SIZE) {
                disable_interrupts();
                keyboard_buffer[buffer_head] = 0x80;
                buffer_head = (buffer_head + 1) % BUFFER_SIZE;
                buffer_count++;
                enable_interrupts();
            }
        } else if (scancode == 0x50) {
            if (buffer_count < BUFFER_SIZE) {
                disable_interrupts();
                keyboard_buffer[buffer_head] = 0x81;
                buffer_head = (buffer_head + 1) % BUFFER_SIZE;
                buffer_count++;
                enable_interrupts();
            }
        } else if (scancode == 0x4B) {
            if (buffer_count < BUFFER_SIZE) {
                disable_interrupts();
                keyboard_buffer[buffer_head] = 0x82;
                buffer_head = (buffer_head + 1) % BUFFER_SIZE;
                buffer_count++;
                enable_interrupts();
            }
        } else if (scancode == 0x4D) {
            if (buffer_count < BUFFER_SIZE) {
                disable_interrupts();
                keyboard_buffer[buffer_head] = 0x83;
                buffer_head = (buffer_head + 1) % BUFFER_SIZE;
                buffer_count++;
                enable_interrupts();
            }
        }
        extended = 0;
        pic_send_eoi(KEYBOARD_IRQ);
        return;
    }
    
    process_scancode(scancode);
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