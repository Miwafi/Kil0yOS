#include "vga.h"
#include "io.h"

static uint16_t* vga_buffer;
static int vga_x = 0;
static int vga_y = 0;
static uint8_t vga_color = 0x07;

void vga_init() {
    vga_buffer = (uint16_t*)VGA_ADDR;
    vga_clear();
}

void vga_clear() {
    for (int i = 0; i < VGA_HEIGHT; i++) {
        for (int j = 0; j < VGA_WIDTH; j++) {
            vga_buffer[i * VGA_WIDTH + j] = vga_entry(' ', vga_color);
        }
    }
    vga_x = 0;
    vga_y = 0;
}

void vga_set_color(uint8_t color) {
    vga_color = color;
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_x = 0;
        vga_y++;
    } else if (c == '\r') {
        vga_x = 0;
    } else if (c == '\t') {
        vga_x = (vga_x + 4) & ~3;
    } else if (c == '\b') {
        if (vga_x > 0) {
            vga_x--;
            vga_buffer[vga_y * VGA_WIDTH + vga_x] = vga_entry(' ', vga_color);
        }
    } else {
        vga_buffer[vga_y * VGA_WIDTH + vga_x] = vga_entry(c, vga_color);
        vga_x++;
    }
    
    if (vga_x >= VGA_WIDTH) {
        vga_x = 0;
        vga_y++;
    }
    
    if (vga_y >= VGA_HEIGHT) {
        for (int i = 1; i < VGA_HEIGHT; i++) {
            for (int j = 0; j < VGA_WIDTH; j++) {
                vga_buffer[(i - 1) * VGA_WIDTH + j] = vga_buffer[i * VGA_WIDTH + j];
            }
        }
        for (int j = 0; j < VGA_WIDTH; j++) {
            vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + j] = vga_entry(' ', vga_color);
        }
        vga_y = VGA_HEIGHT - 1;
    }
    
    vga_set_cursor(vga_x, vga_y);
}

void vga_puts(const char* str) {
    while (*str) {
        vga_putchar(*str++);
    }
}

void vga_set_cursor(int x, int y) {
    uint16_t pos = y * VGA_WIDTH + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}