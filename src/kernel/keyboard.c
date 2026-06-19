#include "keyboard.h"
#include "io.h"
#include "isr.h"
#include "interrupts.h"

#define BUFFER_SIZE 256
#define KEYBOARD_STATUS_PORT 0x64

static char keyboard_buffer[BUFFER_SIZE];
static int buffer_head = 0;
static int buffer_tail = 0;
static int buffer_count = 0;

static int shift_pressed = 0;
static int caps_lock = 0;
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
    
    if (scancode >= sizeof(scancode_map)) {
        return;
    }
    
    char c = scancode_map[scancode];
    if (c != 0) {
        if ((shift_pressed ^ caps_lock) && c >= 'a' && c <= 'z') {
            c = scancode_map_shift[scancode];
        } else if (shift_pressed && c >= '1' && c <= '9') {
            c = scancode_map_shift[scancode];
        } else if (shift_pressed && c == '0') {
            c = scancode_map_shift[scancode];
        }
        
        if (buffer_count < BUFFER_SIZE) {
            keyboard_buffer[buffer_head] = c;
            buffer_head = (buffer_head + 1) % BUFFER_SIZE;
            buffer_count++;
        }
    }
}

void keyboard_handler(interrupt_frame_t* frame) {
    uint8_t scancode = inb(KEYBOARD_PORT);
    
    if (scancode == 0xE0) {
        extended = 1;
        return;
    }
    
    if (scancode & 0x80) {
        scancode &= ~0x80;
        if (scancode == 42 || scancode == 54) {
            shift_pressed = 0;
        }
        extended = 0;
        return;
    }
    
    if (extended) {
        if (scancode == 0x53) {
            if (buffer_count < BUFFER_SIZE) {
                keyboard_buffer[buffer_head] = '\b';
                buffer_head = (buffer_head + 1) % BUFFER_SIZE;
                buffer_count++;
            }
        }
        extended = 0;
        return;
    }
    
    process_scancode(scancode);
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

void keyboard_init() {
    keyboard_controller_reset();
    register_irq_handler(KEYBOARD_IRQ, keyboard_handler);
    pic_enable_irq(KEYBOARD_IRQ);
}

char keyboard_getc() {
    while (buffer_count == 0) {
        __asm__ volatile("hlt");
    }
    
    char c = keyboard_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
    buffer_count--;
    return c;
}