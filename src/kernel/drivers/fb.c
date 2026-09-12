/* 32bpp linear framebuffer text terminal for the UEFI GOP path.
 *
 * The framebuffer lives at the GOP FrameBufferBase (identity-mapped by the
 * boot page tables for the first 4 GiB).  All entry points are inert until
 * fb_set_info() runs from efi_gop_init, so BIOS boots never touch this
 * code and the legacy VGA terminal stays the only display. */
#include "drivers/fb.h"
#include "drivers/vga.h"
#include "gfx/88front.h"
#include "lib/string.h"

static uint32_t* fb   = NULL;   /* linear 32bpp surface      */
static uint32_t  pitch_ = 0;    /* bytes per scanline        */
static uint32_t  w_     = 0;    /* pixels                    */
static uint32_t  h_     = 0;    /* pixels                    */
static int       rgbx_  = 0;    /* 1: RGBX, 0: BGRX (bytes)  */
static int       active = 0;

static int cur_x = 0;           /* glyph column              */
static int cur_y = 0;           /* glyph row                 */
static int cursor_drawn = 0;    /* software cursor on screen */
static int console_muted = 0;   /* desktop mode: klog must not paint */
static uint32_t fg_rgb = 0xAAAAAA;   /* EGA 7 light grey */
static uint32_t bg_rgb = 0x000000;   /* EGA 0 black      */

/* While the GOP desktop owns the whole screen, plain console text would
 * be painted straight over the desktop chrome and stay there until some
 * UI redraw happens to cover that region.  Muting makes fb_putchar a
 * no-op so klog output goes to the serial log only; the desktop exit
 * path unmutes and repaints the terminal via fb_clear(). */
void fb_console_mute(int on) { console_muted = on; }
int  fb_console_muted(void)  { return console_muted; }

/* EGA 16-color palette (matches the vga_color numbering) -> 0xRRGGBB */
static const uint32_t ega_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

/* EGA palette index -> current fg/bg RGB (clamped input like vga_set_color) */
void fb_set_color(uint8_t ega_index) {
    fg_rgb = ega_rgb[ega_index & 0x0F];
}

void fb_set_info(uint64_t base, uint32_t pitch, uint32_t w, uint32_t h,
                 int rgbx) {
    fb     = (uint32_t*)(uint64_t)base;
    pitch_ = pitch;
    w_     = w;
    h_     = h;
    rgbx_  = rgbx;
    active = (fb != NULL && pitch_ >= w_ * 4 && w_ >= 8 && h_ >= 8);
    cur_x  = 0;
    cur_y  = 0;
    cursor_drawn = 0;
}

int fb_is_active(void) { return active; }
int fb_cols(void)      { return active ? (int)(w_ / 8) : 0; }
int fb_rows(void)      { return active ? (int)(h_ / 8) : 0; }
int fb_width(void)     { return active ? (int)w_ : 0; }
int fb_height(void)    { return active ? (int)h_ : 0; }

static inline void set_pixel(int x, int y, uint32_t rgb) {
    uint32_t word;
    if (rgbx_) {
        word = 0xFF000000u |
               ((rgb & 0x0000FF) << 16) |      /* B <- blue  */
               (rgb & 0x00FF00)        |       /* G <- green */
               ((rgb & 0xFF0000) >> 16);       /* R <- red   */
    } else {
        word = 0xFF000000u | rgb;              /* X R G B little-endian */
    }
    fb[y * (pitch_ / 4) + x] = word;
}

/* ========== GOP desktop primitives (EGA-index colored) ========== */

static inline uint32_t pack_rgb(uint32_t rgb) {
    return rgbx_ ? (0xFF000000u | ((rgb & 0xFF) << 16) |
                    (rgb & 0xFF00) | (rgb >> 16))
                 : (0xFF000000u | rgb);
}

void fb_gfx_fill_rect(int x, int y, int w, int h, uint8_t ega_index) {
    if (!active) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)w_) w = (int)w_ - x;
    if (y + h > (int)h_) h = (int)h_ - y;
    if (w <= 0 || h <= 0) return;

    uint32_t word = pack_rgb(ega_rgb[ega_index & 0x0F]);
    uint32_t stride = pitch_ / 4;
    for (int row = y; row < y + h; row++) {
        uint32_t* line = fb + row * stride + x;
        for (int col = 0; col < w; col++) line[col] = word;
    }
}

void fb_gfx_draw_rect(int x, int y, int w, int h, uint8_t ega_index) {
    if (!active || w <= 0 || h <= 0) return;
    fb_gfx_fill_rect(x, y, w, 1, ega_index);
    fb_gfx_fill_rect(x, y + h - 1, w, 1, ega_index);
    fb_gfx_fill_rect(x, y, 1, h, ega_index);
    fb_gfx_fill_rect(x + w - 1, y, 1, h, ega_index);
}

/* transparent-background glyph, matching vga_draw_char */
void fb_gfx_draw_char(int x, int y, char c, uint8_t ega_index) {
    if (!active) return;
    if (x < 0 || x + 8 > (int)w_ || y < 0 || y + 8 > (int)h_) return;
    if (c < 0x20 || c > 0x7E) c = 0x20;

    uint32_t rgb = ega_rgb[ega_index & 0x0F];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = matrix_font[c - 0x20][row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) set_pixel(x + col, y + row, rgb);
        }
    }
}

void fb_gfx_draw_string(int x, int y, const char* s, uint8_t ega_index) {
    if (!active) return;
    int cx = x;
    while (s && *s) {
        if (*s == '\n') {
            cx = x;
            y += 8;
        } else {
            fb_gfx_draw_char(cx, y, *s, ega_index);
            cx += 8;
        }
        s++;
    }
}

/* ========== desktop pointer (white arrow, save/restore) ========== */

#define DCUR_W 8
#define DCUR_H 12
static const uint8_t dcur_pat[DCUR_H][DCUR_W] = {
    {1,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0},
    {1,1,1,1,0,0,0,0},
    {1,1,1,1,1,0,0,0},
    {1,1,1,1,1,1,0,0},
    {1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1},
    {1,1,1,1,1,0,0,0},
    {1,1,0,1,1,0,0,0},
    {1,0,0,1,1,0,0,0},
    {0,0,0,1,1,0,0,0},
};
static uint32_t dcur_save[DCUR_H][DCUR_W];
static int dcur_x = -1, dcur_y = -1, dcur_visible = 0;

void fb_gfx_cursor_draw(int x, int y) {
    if (!active) return;
    if (dcur_visible) fb_gfx_cursor_erase(0, 0);

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + DCUR_W > (int)w_) x = (int)w_ - DCUR_W;
    if (y + DCUR_H > (int)h_) y = (int)h_ - DCUR_H;

    uint32_t stride = pitch_ / 4;
    for (int row = 0; row < DCUR_H; row++) {
        for (int col = 0; col < DCUR_W; col++) {
            int px = x + col, py = y + row;
            dcur_save[row][col] = fb[py * stride + px];
            if (dcur_pat[row][col]) set_pixel(px, py, 0x000000);
        }
    }
    dcur_x = x;
    dcur_y = y;
    dcur_visible = 1;
}

void fb_gfx_cursor_erase(int x, int y) {
    (void)x; (void)y;   /* position remembered internally */
    if (!dcur_visible || !active) { dcur_visible = 0; return; }

    uint32_t stride = pitch_ / 4;
    for (int row = 0; row < DCUR_H; row++)
        for (int col = 0; col < DCUR_W; col++)
            fb[(dcur_y + row) * stride + (dcur_x + col)] = dcur_save[row][col];

    dcur_visible = 0;
}

/* 8x8 glyph from matrix_font; the whole cell is repainted with the
 * background so overwrites never leave trails.  Clamps at the edges. */
static void fb_draw_char(int gx, int gy, char c) {
    if (gx < 0 || gy < 0 ||
        gx + 8 > (int)w_ || gy + 8 > (int)h_) return;
    if (c < 0x20 || c > 0x7E) c = 0x20;

    int x0 = gx, y0 = gy;
    for (int row = 0; row < 8; row++) {
        uint8_t bits = matrix_font[c - 0x20][row];
        for (int col = 0; col < 8; col++) {
            set_pixel(x0 + col, y0 + row,
                      (bits & (0x80 >> col)) ? fg_rgb : bg_rgb);
        }
    }
}

/* software cursor: 8x2 underline in the current cell */
static void cursor_erase(void) {
    if (!cursor_drawn) return;
    cursor_drawn = 0;
    int px = cur_x * 8;
    int py = cur_y * 8 + 6;
    if (px + 8 > (int)w_ || py + 2 > (int)h_) return;
    for (int y = py; y < py + 2; y++)
        for (int x = px; x < px + 8; x++)
            set_pixel(x, y, bg_rgb);
}

static void cursor_draw(void) {
    int px = cur_x * 8;
    int py = cur_y * 8 + 6;
    if (px + 8 > (int)w_ || py + 2 > (int)h_) return;
    for (int y = py; y < py + 2; y++)
        for (int x = px; x < px + 8; x++)
            set_pixel(x, y, fg_rgb);
    cursor_drawn = 1;
}

/* solid color rectangle for boot-time diagnostics (EBS bring-up markers):
 * usable before any terminal exists, survives as long as we don't clear */
void fb_debug_block(int x, int y, int w, int h, uint32_t rgb) {
    if (!active) return;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)h_) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)w_) continue;
            set_pixel(xx, yy, rgb);
        }
    }
}

void fb_scroll(void) {
    if (!active) return;
    uint32_t stride = pitch_ / 4;
    uint32_t rows_px = h_ / 8 * 8;
    for (uint32_t y = 8; y < rows_px; y++) {
        memcpy((uint8_t*)(fb + (y - 8) * stride),
               (const uint8_t*)(fb + y * stride), w_ * 4);
    }
    for (uint32_t y = rows_px - 8; y < rows_px; y++)
        for (uint32_t x = 0; x < w_; x++)
            set_pixel((int)x, (int)y, bg_rgb);
    if (cur_y > 0) cur_y--;
    cursor_drawn = 0;
}

void fb_clear(void) {
    if (!active) return;
    uint32_t stride = pitch_ / 4;
    for (uint32_t y = 0; y < h_; y++)
        for (uint32_t x = 0; x < w_; x++)
            fb[y * stride + x] =
                rgbx_ ? (0xFF000000u | (bg_rgb & 0xFF) << 16 |
                         (bg_rgb & 0xFF00) | (bg_rgb >> 16))
                      : (0xFF000000u | bg_rgb);
    cur_x = 0;
    cur_y = 0;
    cursor_drawn = 0;
    /* screen wipe invalidates the desktop pointer's save buffer */
    dcur_visible = 0;
    dcur_x = dcur_y = -1;
}

void fb_putchar(char c) {
    if (!active || console_muted) return;
    cursor_erase();

    if (c == '\n') {
        cur_x = 0;
        cur_y++;
    } else if (c == '\r') {
        cur_x = 0;
    } else if (c == '\t') {
        cur_x = (cur_x + 4) & ~3;
    } else if (c == '\b') {
        if (cur_x > 0) {
            cur_x--;
            fb_draw_char(cur_x * 8, cur_y * 8, ' ');
        }
    } else {
        fb_draw_char(cur_x * 8, cur_y * 8, c);
        cur_x++;
    }

    if (cur_x >= (int)(w_ / 8)) {
        cur_x = 0;
        cur_y++;
    }
    if (cur_y >= (int)(h_ / 8)) {
        fb_scroll();
        cur_y = (int)(h_ / 8) - 1;
    }

    cursor_draw();
}

void fb_puts(const char* s) {
    while (s && *s) fb_putchar(*s++);
}
