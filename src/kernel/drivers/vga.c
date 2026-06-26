#include "drivers/vga.h"
#include "drivers/io.h"
#include "gfx/88front.h"

uint16_t* vga_buffer;
uint8_t* vga_gfx_buffer;
static int vga_x = 0;
static int vga_y = 0;
uint8_t vga_color = 0x07;

static void vga_write_reg(uint16_t port, uint8_t idx, uint8_t val) {
    outb(port, idx);
    outb(port + 1, val);
}

static uint8_t vga_read_reg(uint16_t port, uint8_t idx) {
    outb(port, idx);
    return inb(port + 1);
}

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

void vga_set_mode_13h() {
    vga_gfx_buffer = (uint8_t*)VGA_GFX_ADDR;

    outb(0x3C2, 0x63);

    vga_write_reg(0x3C4, 0x00, 0x03);
    vga_write_reg(0x3C4, 0x01, 0x01);
    vga_write_reg(0x3C4, 0x02, 0x0F);
    vga_write_reg(0x3C4, 0x03, 0x00);
    vga_write_reg(0x3C4, 0x04, 0x0E);

    uint8_t crtc_unlock = vga_read_reg(0x3D4, 0x11);
    vga_write_reg(0x3D4, 0x11, crtc_unlock & 0x7F);

    vga_write_reg(0x3D4, 0x00, 0x5F);
    vga_write_reg(0x3D4, 0x01, 0x4F);
    vga_write_reg(0x3D4, 0x02, 0x50);
    vga_write_reg(0x3D4, 0x03, 0x82);
    vga_write_reg(0x3D4, 0x04, 0x54);
    vga_write_reg(0x3D4, 0x05, 0x80);
    vga_write_reg(0x3D4, 0x06, 0xBF);
    vga_write_reg(0x3D4, 0x07, 0x1F);
    vga_write_reg(0x3D4, 0x08, 0x00);
    vga_write_reg(0x3D4, 0x09, 0x41);
    vga_write_reg(0x3D4, 0x0A, 0x00);
    vga_write_reg(0x3D4, 0x0B, 0x00);
    vga_write_reg(0x3D4, 0x0C, 0x00);
    vga_write_reg(0x3D4, 0x0D, 0x00);
    vga_write_reg(0x3D4, 0x0E, 0x00);
    vga_write_reg(0x3D4, 0x0F, 0x00);
    vga_write_reg(0x3D4, 0x10, 0x9C);
    vga_write_reg(0x3D4, 0x11, 0x8E);
    vga_write_reg(0x3D4, 0x12, 0x8F);
    vga_write_reg(0x3D4, 0x13, 0x28);
    vga_write_reg(0x3D4, 0x14, 0x40);
    vga_write_reg(0x3D4, 0x15, 0x96);
    vga_write_reg(0x3D4, 0x16, 0xB9);
    vga_write_reg(0x3D4, 0x17, 0xA3);
    vga_write_reg(0x3D4, 0x18, 0xFF);

    vga_write_reg(0x3CE, 0x00, 0x00);
    vga_write_reg(0x3CE, 0x01, 0x00);
    vga_write_reg(0x3CE, 0x02, 0x00);
    vga_write_reg(0x3CE, 0x03, 0x00);
    vga_write_reg(0x3CE, 0x04, 0x00);
    vga_write_reg(0x3CE, 0x05, 0x40);
    vga_write_reg(0x3CE, 0x06, 0x05);
    vga_write_reg(0x3CE, 0x07, 0x0F);
    vga_write_reg(0x3CE, 0x08, 0xFF);

    for (int i = 0; i < 16; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        outb(0x3C0, (uint8_t)i);
    }
    outb(0x3C0, 0x20);

    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) {
        vga_gfx_buffer[i] = 0;
    }
}

void vga_set_text_mode() {
    vga_buffer = (uint16_t*)VGA_ADDR;

    outb(0x3C2, 0x67);

    vga_write_reg(0x3C4, 0x00, 0x03);
    vga_write_reg(0x3C4, 0x01, 0x00);
    vga_write_reg(0x3C4, 0x02, 0x03);
    vga_write_reg(0x3C4, 0x03, 0x00);
    vga_write_reg(0x3C4, 0x04, 0x02);

    uint8_t crtc_unlock = vga_read_reg(0x3D4, 0x11);
    vga_write_reg(0x3D4, 0x11, crtc_unlock & 0x7F);

    vga_write_reg(0x3D4, 0x00, 0x5F);
    vga_write_reg(0x3D4, 0x01, 0x4F);
    vga_write_reg(0x3D4, 0x02, 0x50);
    vga_write_reg(0x3D4, 0x03, 0x82);
    vga_write_reg(0x3D4, 0x04, 0x55);
    vga_write_reg(0x3D4, 0x05, 0x81);
    vga_write_reg(0x3D4, 0x06, 0xBF);
    vga_write_reg(0x3D4, 0x07, 0x1F);
    vga_write_reg(0x3D4, 0x08, 0x00);
    vga_write_reg(0x3D4, 0x09, 0x4F);
    vga_write_reg(0x3D4, 0x0A, 0x0E);
    vga_write_reg(0x3D4, 0x0B, 0x0F);
    vga_write_reg(0x3D4, 0x0C, 0x00);
    vga_write_reg(0x3D4, 0x0D, 0x00);
    vga_write_reg(0x3D4, 0x0E, 0x00);
    vga_write_reg(0x3D4, 0x0F, 0x00);
    vga_write_reg(0x3D4, 0x10, 0x9C);
    vga_write_reg(0x3D4, 0x11, 0x8E);
    vga_write_reg(0x3D4, 0x12, 0x8F);
    vga_write_reg(0x3D4, 0x13, 0x28);
    vga_write_reg(0x3D4, 0x14, 0x1F);
    vga_write_reg(0x3D4, 0x15, 0x96);
    vga_write_reg(0x3D4, 0x16, 0xB9);
    vga_write_reg(0x3D4, 0x17, 0xA3);
    vga_write_reg(0x3D4, 0x18, 0xFF);

    vga_write_reg(0x3CE, 0x00, 0x00);
    vga_write_reg(0x3CE, 0x01, 0x00);
    vga_write_reg(0x3CE, 0x02, 0x00);
    vga_write_reg(0x3CE, 0x03, 0x00);
    vga_write_reg(0x3CE, 0x04, 0x00);
    vga_write_reg(0x3CE, 0x05, 0x10);
    vga_write_reg(0x3CE, 0x06, 0x0E);
    vga_write_reg(0x3CE, 0x07, 0x00);
    vga_write_reg(0x3CE, 0x08, 0xFF);

    static const uint8_t text_palette[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
    };
    for (int i = 0; i < 16; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        outb(0x3C0, text_palette[i]);
    }
    outb(0x3C0, 0x20);

    vga_clear();
}

void vga_plot_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return;
    vga_gfx_buffer[y * GFX_WIDTH + x] = color;
}

void vga_draw_color_bars() {
    static const uint8_t bar_colors[8] = {
        0x00,
        0x01,
        0x04,
        0x02,
        0x03,
        0x05,
        0x06,
        0x07
    };

    int bar_width = GFX_WIDTH / 8;

    for (int y = 0; y < GFX_HEIGHT; y++) {
        for (int bar = 0; bar < 8; bar++) {
            uint8_t color = bar_colors[bar];
            int start_x = bar * bar_width;
            int end_x = (bar == 7) ? GFX_WIDTH : start_x + bar_width;

            for (int x = start_x; x < end_x; x++) {
                vga_gfx_buffer[y * GFX_WIDTH + x] = color;
            }
        }
    }
}

void vga_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_WIDTH)  w = GFX_WIDTH - x;
    if (y + h > GFX_HEIGHT) h = GFX_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            vga_gfx_buffer[row * GFX_WIDTH + col] = color;
        }
    }
}

void vga_draw_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    vga_fill_rect(x, y, w, 1, color);
    vga_fill_rect(x, y + h - 1, w, 1, color);
    vga_fill_rect(x, y, 1, h, color);
    vga_fill_rect(x + w - 1, y, 1, h, color);
}

void vga_draw_char(int x, int y, char c, uint8_t color) {
    if (x < 0 || x + 8 > GFX_WIDTH || y < 0 || y + 8 > GFX_HEIGHT) return;
    
    if (c < 0x20 || c > 0x7E) c = 0x20;
    int char_idx = c - 0x20;
    
    for (int row = 0; row < 8; row++) {
        uint8_t byte = matrix_font[char_idx][row];
        for (int col = 0; col < 8; col++) {
            if (byte & (0x80 >> col)) {
                vga_gfx_buffer[(y + row) * GFX_WIDTH + (x + col)] = color;
            }
        }
    }
}

void vga_draw_string(int x, int y, const char* str, uint8_t color) {
    int current_x = x;
    while (*str) {
        if (*str == '\n') {
            current_x = x;
            y += 8;
        } else {
            vga_draw_char(current_x, y, *str, color);
            current_x += 6;
        }
        str++;
    }
}

void vga_draw_window(int x, int y, int w, int h, const char* title) {
    int title_h = 8;
    int border = 1;
    int inner_x = x + border;
    int inner_y = y + border + title_h;
    int inner_w = w - border * 2;
    int inner_h = h - border * 2 - title_h;

    if (inner_w < 0) inner_w = 0;
    if (inner_h < 0) inner_h = 0;

    /* black content background */
    vga_fill_rect(inner_x, inner_y, inner_w, inner_h, 0x00);

    /* blue title bar */
    vga_fill_rect(inner_x, y + border, inner_w, title_h, 0x01);

    /* yellow border */
    vga_draw_rect(x, y, w, h, 0x0E);

    /* title text in white */
    if (title) {
        int title_x = inner_x + 2;
        int title_y = y + border + 1;
        vga_draw_string(title_x, title_y, title, 0x0F);
    }
}