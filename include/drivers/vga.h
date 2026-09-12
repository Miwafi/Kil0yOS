#ifndef VGA_H
#define VGA_H

#include "lib/types.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define VGA_ADDR        0xB8000
#define VGA_GFX_ADDR    0xA0000

/* VGA graphics desktop mode: mode 12h, 640x480, 16 colors, planar
 * (4 bit planes at 0xA0000, one bit per pixel per plane). */
#define GFX_WIDTH       640
#define GFX_HEIGHT      480

extern uint16_t* vga_buffer;
extern uint8_t vga_color;

typedef enum {
    COLOR_BLACK         = 0,
    COLOR_BLUE          = 1,
    COLOR_GREEN         = 2,
    COLOR_CYAN          = 3,
    COLOR_RED           = 4,
    COLOR_MAGENTA       = 5,
    COLOR_BROWN         = 6,
    COLOR_GREY          = 7,
    COLOR_DARK_GREY     = 8,
    COLOR_LIGHT_BLUE    = 9,
    COLOR_LIGHT_GREEN   = 10,
    COLOR_LIGHT_CYAN    = 11,
    COLOR_LIGHT_RED     = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_LIGHT_BROWN   = 14,
    COLOR_WHITE         = 15
} vga_color_t;

static inline uint8_t vga_entry_color(vga_color_t fg, vga_color_t bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | (uint16_t) color << 8;
}

void vga_init();
void vga_clear();
void vga_putchar(char c);
void vga_puts(const char* str);
void vga_puthex(uint64_t value);
void vga_set_color(uint8_t color);
void vga_set_cursor(int x, int y);
int  vga_is_graphics(void);
void vga_wait_vsync(void);

void vga_set_gfx_mode(void);   /* mode 12h: 640x480x16, planar */
void vga_set_text_mode();
void vga_plot_pixel(int x, int y, uint8_t color);
uint8_t vga_read_pixel(int x, int y);
void vga_draw_color_bars();
void vga_fill_rect(int x, int y, int w, int h, uint8_t color);
void vga_draw_rect(int x, int y, int w, int h, uint8_t color);
void vga_draw_char(int x, int y, char c, uint8_t color);
void vga_draw_string(int x, int y, const char* str, uint8_t color);

/* Kernel log – prints timestamped message to both VGA and serial */
extern void klog(const char* s);
extern void klog_hex(const char* prefix, uint64_t v);
/* GUI log pane: ring-buffer mirror of every klog byte. klog_seq() returns
 * the total byte counter; klog_copy_from() copies bytes with sequence >=
 * *seq (advancing it), clamped to the oldest retained byte. */
extern uint32_t klog_seq(void);
extern uint32_t klog_copy_from(uint32_t* seq, char* dst, uint32_t max);
void vga_draw_window(int x, int y, int w, int h, const char* title);

#endif