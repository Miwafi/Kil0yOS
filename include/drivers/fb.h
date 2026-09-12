#ifndef FB_H
#define FB_H

#include "lib/types.h"

/* Linear 32bpp framebuffer text terminal (UEFI GOP path).
 *
 * Filled in by efi_gop_init -> fb_set_info before any consumer runs; when
 * inactive (BIOS boot) every entry point is a no-op and the legacy VGA text
 * terminal keeps working untouched.  Mirrors the vga_putchar semantics
 * (\n \r \t \b, scroll at the last row) on a 8x8 glyph grid. */

void fb_set_info(uint64_t base, uint32_t pitch, uint32_t w, uint32_t h,
                 int rgbx);

int  fb_is_active(void);
/* Desktop mode: silence the plain fb text console so klog diagnostics do
 * not paint over the desktop UI (serial output continues).  The kernel
 * exception dump path calls fb_puts directly and stays unaffected. */
void fb_console_mute(int on);
int  fb_console_muted(void);
int  fb_cols(void);          /* glyph columns (= w / 8)  */
int  fb_rows(void);          /* glyph rows    (= h / 8)  */
int  fb_width(void);         /* pixels                   */
int  fb_height(void);        /* pixels                   */

/* GOP desktop primitives: EGA-16 color indexing (same numbering as
 * vga_color) so the mode13h desktop code maps 1:1 onto the framebuffer.
 * draw_char/draw_string paint fg pixels only (transparent background),
 * mirroring the vga_draw_* semantics. */
void fb_gfx_fill_rect(int x, int y, int w, int h, uint8_t ega_index);
void fb_gfx_draw_rect(int x, int y, int w, int h, uint8_t ega_index);
void fb_gfx_draw_char(int x, int y, char c, uint8_t ega_index);
void fb_gfx_draw_string(int x, int y, const char* s, uint8_t ega_index);

/* solid white arrow pointer with background save/restore; draw erases the
 * previous position itself (mouse_draw_cursor semantics) */
void fb_gfx_cursor_draw(int x, int y);
void fb_gfx_cursor_erase(int x, int y);

void fb_clear(void);
void fb_putchar(char c);
void fb_puts(const char* s);
/* solid color rectangle for boot-time diagnostics (visible pre-terminal);
 * rgb is 0x00RRGGBB, mapped through the active pixel format */
void fb_debug_block(int x, int y, int w, int h, uint32_t rgb);
/* EGA 16-color palette index (same numbering as vga_color) -> 24-bit RGB */
void fb_set_color(uint8_t ega_index);
void fb_scroll(void);

#endif /* FB_H */
