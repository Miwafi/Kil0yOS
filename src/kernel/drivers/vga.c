#include "drivers/vga.h"
#include "drivers/fb.h"
#include "drivers/io.h"
#include "gfx/88front.h"

uint16_t* vga_buffer;
static int vga_x = 0;
static int vga_y = 0;
uint8_t vga_color = 0x07;
static int vga_in_gfx_mode = 0;

/* Snapshot of text-mode state destroyed by mode 13h:
   - plane 2 holds the BIOS character generator (font). Mode 13h chained
     addressing spreads writes across all four planes, so any full repaint
     destroys the font -> garbage "dot matrix" screen after returning to text.
   - the DAC palette carries BIOS text colours; reprogram it deterministically
     for graphics and restore the original on return. */
static uint8_t vga_font_plane[16384];
static uint8_t vga_dac_state[256 * 3];
/* sequencer/graphics regs we touch during plane access; restoring the exact
   BIOS values keeps text-mode addressing (odd/even etc.) intact */
static uint8_t vga_reg_snap[7];   /* SR2 SR4 GR3 GR5 GR6 GR4 GR8 */
static int vga_snapshot_valid = 0;

/* Wait for start of vertical retrace before large repaints (anti-tear).
   Bounded so a non-toggling status register can never hang the caller. */
void vga_wait_vsync(void) {
    if (fb_is_active()) return;      /* no legacy VGA regs under GOP */
    int guard = 200000;
    while ((inb(0x3DA) & 0x08)) {
        if (--guard == 0) return;
    }
    guard = 200000;
    while (!(inb(0x3DA) & 0x08)) {
        if (--guard == 0) return;
    }
}

int vga_is_graphics(void) {
    return vga_in_gfx_mode;
}

static void vga_write_reg(uint16_t port, uint8_t idx, uint8_t val) {
    outb(port, idx);
    outb(port + 1, val);
}

static uint8_t vga_read_reg(uint16_t port, uint8_t idx) {
    outb(port, idx);
    return inb(port + 1);
}

void vga_init() {
    /* UEFI GOP path: the firmware owns the display and 0xB8000 may not be
     * mapped at all - leave the sentinel NULL so every text op becomes a
     * no-op and the fb terminal takes over.  BIOS boots are unchanged. */
    if (fb_is_active()) {
        vga_buffer = 0;
        return;
    }
    vga_buffer = (uint16_t*)VGA_ADDR;
    vga_clear();
}

void vga_clear() {
    if (!vga_buffer) return;
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
    if (!vga_buffer) return;
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

void vga_puthex(uint64_t value) {
    if (!vga_buffer) return;
    const char hex_chars[] = "0123456789ABCDEF";
    char buffer[17];
    buffer[16] = '\0';

    for (int i = 15; i >= 0; i--) {
        buffer[i] = hex_chars[value & 0xF];
        value >>= 4;
    }

    vga_puts(buffer);
}

void vga_set_cursor(int x, int y) {
    if (!vga_buffer) return;
    uint16_t pos = y * VGA_WIDTH + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* --- text-mode state snapshot (font plane 2 + DAC palette) --- */

static void vga_snapshot_save(void) {
    if (vga_snapshot_valid) return;

    /* remember the registers the plane access will overwrite */
    vga_reg_snap[0] = vga_read_reg(0x3C4, 0x02);
    vga_reg_snap[1] = vga_read_reg(0x3C4, 0x04);
    vga_reg_snap[2] = vga_read_reg(0x3CE, 0x03);
    vga_reg_snap[3] = vga_read_reg(0x3CE, 0x05);
    vga_reg_snap[4] = vga_read_reg(0x3CE, 0x06);
    vga_reg_snap[5] = vga_read_reg(0x3CE, 0x04);
    vga_reg_snap[6] = vga_read_reg(0x3CE, 0x08);

    /* sequential access to plane 2: reset sequencer, disable odd/even,
       map A000 64K window (text-mode GR6 points at B800!), select plane 2 */
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);                 /* synchronous reset */
    vga_write_reg(0x3C4, 0x04, 0x06);  /* odd/even off, ext. memory */
    vga_write_reg(0x3C4, 0x02, 0x04);  /* write plane 2 only */
    vga_write_reg(0x3CE, 0x03, 0x00);  /* no data rotate */
    vga_write_reg(0x3CE, 0x05, 0x00);  /* read mode 0, write mode 0 */
    vga_write_reg(0x3CE, 0x06, 0x05);  /* map A000-BFFF, graphics layout */
    vga_write_reg(0x3CE, 0x04, 0x02);  /* read plane 2 */
    vga_write_reg(0x3CE, 0x08, 0xFF);  /* all bitmask bits */
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);                 /* restart sequencer */

    {
        volatile const uint8_t* src = (volatile const uint8_t*)VGA_GFX_ADDR;
        for (int i = 0; i < 16384; i++) {
            vga_font_plane[i] = src[i];
        }
    }

    for (int i = 0; i < 256; i++) {
        outb(0x3C7, (uint8_t)i);       /* PEL read index */
        for (int j = 0; j < 3; j++) {
            vga_dac_state[i * 3 + j] = inb(0x3C9);
        }
    }

    vga_snapshot_valid = 1;
}

static void vga_snapshot_restore(void) {
    if (!vga_snapshot_valid) return;

    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);
    vga_write_reg(0x3C4, 0x04, 0x06);
    vga_write_reg(0x3C4, 0x02, 0x04);
    vga_write_reg(0x3CE, 0x03, 0x00);
    vga_write_reg(0x3CE, 0x05, 0x00);
    vga_write_reg(0x3CE, 0x06, 0x05);
    vga_write_reg(0x3CE, 0x04, 0x02);
    vga_write_reg(0x3CE, 0x08, 0xFF);
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);

    {
        volatile uint8_t* dst = (volatile uint8_t*)VGA_GFX_ADDR;
        for (int i = 0; i < 16384; i++) {
            dst[i] = vga_font_plane[i];
        }
    }

    /* put back exactly the register values we saved (BIOS text state) */
    vga_write_reg(0x3C4, 0x02, vga_reg_snap[0]);
    vga_write_reg(0x3C4, 0x04, vga_reg_snap[1]);
    vga_write_reg(0x3CE, 0x03, vga_reg_snap[2]);
    vga_write_reg(0x3CE, 0x05, vga_reg_snap[3]);
    vga_write_reg(0x3CE, 0x06, vga_reg_snap[4]);
    vga_write_reg(0x3CE, 0x04, vga_reg_snap[5]);
    vga_write_reg(0x3CE, 0x08, vga_reg_snap[6]);

    for (int i = 0; i < 256; i++) {
        outb(0x3C8, (uint8_t)i);       /* PEL write index */
        for (int j = 0; j < 3; j++) {
            outb(0x3C9, vga_dac_state[i * 3 + j]);
        }
    }

    vga_snapshot_valid = 0;
}

/* --- mode 12h planar (640x480x16) ---------------------------------- */

/* Byte offset of the byte containing pixel (x,y) inside each plane. */
static inline int gfx_byte_offset(int x, int y) {
    return y * (GFX_WIDTH / 8) + (x >> 3);
}

/* Graphics controller: write mode 2 (plane values from CPU data low
 * nibble), read mode 0, no rotation.  GR8 (bitmask) is set per op. */
static void gfx_wmode2(void) {
    vga_write_reg(0x3CE, 0x03, 0x00);
    vga_write_reg(0x3CE, 0x05, 0x02);
}

/* Read-modify-write one display byte: the IN loads the latches (so the
 * bits outside GR8 keep their color), the OUT stores color into the
 * masked-in bits. */
static inline void gfx_rmw_byte(int off, uint8_t mask, uint8_t color) {
    volatile uint8_t* p = (volatile uint8_t*)VGA_GFX_ADDR + off;
    vga_write_reg(0x3CE, 0x08, mask);
    (void)*p;
    *p = color;
}

void vga_set_gfx_mode(void) {
    if (fb_is_active()) return;      /* gfx desktop is VGA-only; GOP path keeps fb */
    /* save font plane + DAC while still in text mode; graphics repaints
       would otherwise destroy the character generator */
    vga_snapshot_save();

    vga_in_gfx_mode = 1;

    /* stop the sequencer while retiming: real chips (unlike QEMU/VMware)
       can latch an inconsistent state when Misc clock bits change while
       the character counter is running */
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);               /* synchronous reset */

    outb(0x3C2, 0xE3);               /* 25.175MHz clock, -H/-V sync, 0x3Dx */

    vga_write_reg(0x3C4, 0x00, 0x03);
    vga_write_reg(0x3C4, 0x01, 0x01);
    vga_write_reg(0x3C4, 0x02, 0x0F);
    vga_write_reg(0x3C4, 0x03, 0x00);
    vga_write_reg(0x3C4, 0x04, 0x06);

    uint8_t crtc_unlock = vga_read_reg(0x3D4, 0x11);
    vga_write_reg(0x3D4, 0x11, crtc_unlock & 0x7F);

    /* 640x480 @ 60Hz: 100 char clocks/line, 525 lines/frame */
    vga_write_reg(0x3D4, 0x00, 0x5F);  /* horizontal total - 5 (100 chars) */
    vga_write_reg(0x3D4, 0x01, 0x4F);  /* display end: 640/8 - 1           */
    vga_write_reg(0x3D4, 0x02, 0x52);  /* blank start (656 px)             */
    vga_write_reg(0x3D4, 0x03, 0x82);  /* blank end                        */
    vga_write_reg(0x3D4, 0x04, 0x54);  /* retrace start (672 px)           */
    vga_write_reg(0x3D4, 0x05, 0x80);  /* retrace end                      */
    vga_write_reg(0x3D4, 0x06, 0x0B);  /* vertical total - 2 (525 lines)   */
    vga_write_reg(0x3D4, 0x07, 0x3E);  /* overflow: VT9,VBS8,VRS8,VDE8,LC8 */
    vga_write_reg(0x3D4, 0x08, 0x00);
    vga_write_reg(0x3D4, 0x09, 0x40);  /* max scan line 0 (480 lines), LC9 */
    vga_write_reg(0x3D4, 0x0A, 0x00);  /* cursor off in graphics           */
    vga_write_reg(0x3D4, 0x0B, 0x00);
    vga_write_reg(0x3D4, 0x0C, 0x00);  /* start address = 0                */
    vga_write_reg(0x3D4, 0x0D, 0x00);
    vga_write_reg(0x3D4, 0x0E, 0x00);
    vga_write_reg(0x3D4, 0x0F, 0x00);
    vga_write_reg(0x3D4, 0x10, 0xEA);  /* retrace start (line 490)         */
    vga_write_reg(0x3D4, 0x11, 0x8C);  /* retrace end + lock               */
    vga_write_reg(0x3D4, 0x12, 0xDF);  /* display end: 480 - 1             */
    vga_write_reg(0x3D4, 0x13, 0x28);  /* offset: 80 bytes per scanline    */
    vga_write_reg(0x3D4, 0x14, 0x00);  /* BYTE addressing (no dword shift!) */
    vga_write_reg(0x3D4, 0x15, 0xEA);  /* blank start (line 490)           */
    vga_write_reg(0x3D4, 0x16, 0x0B);  /* blank end                        */
    vga_write_reg(0x3D4, 0x17, 0xE3);  /* CRTC mode: byte addressing       */
    vga_write_reg(0x3D4, 0x18, 0xFF);  /* line compare (never reached)     */

    vga_write_reg(0x3CE, 0x00, 0x00);
    vga_write_reg(0x3CE, 0x01, 0x00);
    vga_write_reg(0x3CE, 0x02, 0x00);
    vga_write_reg(0x3CE, 0x03, 0x00);
    vga_write_reg(0x3CE, 0x04, 0x00);
    vga_write_reg(0x3CE, 0x05, 0x02);  /* write mode 2 (planar RMW path)   */
    vga_write_reg(0x3CE, 0x06, 0x01);  /* A0000 window, NO odd/even chain  */
    vga_write_reg(0x3CE, 0x07, 0x0F);
    vga_write_reg(0x3CE, 0x08, 0xFF);

    for (int i = 0; i < 16; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        outb(0x3C0, (uint8_t)i);
    }
    /* attribute controller mode registers: real hardware (VMware/bochs)
       requires the graphics-mode attribute path to be selected explicitly */
    inb(0x3DA);
    outb(0x3C0, 0x10);
    outb(0x3C0, 0x01);                 /* AC mode: graphics on, no blink   */
    inb(0x3DA);
    outb(0x3C0, 0x11);
    outb(0x3C0, 0x00);                 /* overscan black */
    inb(0x3DA);
    outb(0x3C0, 0x12);
    outb(0x3C0, 0x0F);                 /* color plane enable: all planes */
    inb(0x3DA);
    outb(0x3C0, 0x13);
    outb(0x3C0, 0x00);                 /* no pixel panning */
    outb(0x3C0, 0x20);
    static const uint8_t gfx_dac[16][3] = {
        { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x2A }, { 0x00, 0x2A, 0x00 }, { 0x00, 0x2A, 0x2A },
        { 0x2A, 0x00, 0x00 }, { 0x2A, 0x00, 0x2A }, { 0x2A, 0x15, 0x00 }, { 0x2A, 0x2A, 0x2A },
        { 0x15, 0x15, 0x15 }, { 0x15, 0x15, 0x3F }, { 0x15, 0x3F, 0x15 }, { 0x15, 0x3F, 0x3F },
        { 0x3F, 0x15, 0x15 }, { 0x3F, 0x15, 0x3F }, { 0x3F, 0x3F, 0x15 }, { 0x3F, 0x3F, 0x3F }
    };
    outb(0x3C8, 0);
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, gfx_dac[i][0]);
        outb(0x3C9, gfx_dac[i][1]);
        outb(0x3C9, gfx_dac[i][2]);
    }

    /* restart the sequencer now that all timing/addressing regs are set */
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);

    vga_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, 0x00);
}

void vga_set_text_mode() {
    if (fb_is_active()) return;
    vga_buffer = (uint16_t*)VGA_ADDR;
    vga_in_gfx_mode = 0;

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
    /* attribute controller mode registers back to text values */
    inb(0x3DA);
    outb(0x3C0, 0x10);
    outb(0x3C0, 0x0C);                 /* AC mode: text, blink on */
    inb(0x3DA);
    outb(0x3C0, 0x11);
    outb(0x3C0, 0x00);                 /* overscan black */
    inb(0x3DA);
    outb(0x3C0, 0x12);
    outb(0x3C0, 0x0F);                 /* color plane enable: all planes */
    outb(0x3C0, 0x20);

    /* bring back the BIOS character generator (plane 2) and DAC palette
       destroyed by the mode 13h repaints */
    vga_snapshot_restore();

    vga_clear();
}

void vga_plot_pixel(int x, int y, uint8_t color) {
    if (!vga_in_gfx_mode) return;
    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return;
    gfx_wmode2();
    gfx_rmw_byte(gfx_byte_offset(x, y), 0x80 >> (x & 7), color & 0x0F);
}

uint8_t vga_read_pixel(int x, int y) {
    if (!vga_in_gfx_mode) return 0;
    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return 0;
    vga_write_reg(0x3CE, 0x05, 0x00);   /* read mode 0 */
    int off = gfx_byte_offset(x, y);
    volatile const uint8_t* p =
        (volatile const uint8_t*)VGA_GFX_ADDR + off;
    uint8_t bit = 7 - (x & 7);
    uint8_t color = 0;
    for (int plane = 0; plane < 4; plane++) {
        vga_write_reg(0x3CE, 0x04, (uint8_t)plane);
        if ((*p >> bit) & 1) color |= (uint8_t)(1 << plane);
    }
    return color;
}

void vga_draw_color_bars() {
    if (!vga_in_gfx_mode) return;

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
    for (int bar = 0; bar < 8; bar++) {
        int bx = bar * bar_width;
        vga_fill_rect(bx, 0, GFX_WIDTH - bx, GFX_HEIGHT, bar_colors[bar]);
    }
}

void vga_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (!vga_in_gfx_mode) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_WIDTH)  w = GFX_WIDTH - x;
    if (y + h > GFX_HEIGHT) h = GFX_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    color &= 0x0F;
    int b0 = x >> 3;
    int b1 = (x + w - 1) >> 3;
    gfx_wmode2();

    for (int row = y; row < y + h; row++) {
        int off = row * (GFX_WIDTH / 8);
        if (b0 == b1) {
            uint8_t mask = (uint8_t)(0xFF >> (x & 7)) &
                           (uint8_t)(0xFF << (7 - ((x + w - 1) & 7)));
            gfx_rmw_byte(off + b0, mask, color);
        } else {
            if (x & 7) {
                gfx_rmw_byte(off + b0, (uint8_t)(0xFF >> (x & 7)), color);
            }
            vga_write_reg(0x3CE, 0x08, 0xFF);
            for (int b = b0 + (x & 7 ? 1 : 0); b < b1; b++) {
                volatile uint8_t* p =
                    (volatile uint8_t*)VGA_GFX_ADDR + off + b;
                *p = color;
            }
            if ((x + w - 1) & 7) {
                gfx_rmw_byte(off + b1,
                             (uint8_t)(0xFF << (7 - ((x + w - 1) & 7))),
                             color);
            }
        }
    }
}

void vga_draw_rect(int x, int y, int w, int h, uint8_t color) {
    if (!vga_in_gfx_mode) return;
    if (w <= 0 || h <= 0) return;
    vga_fill_rect(x, y, w, 1, color);
    vga_fill_rect(x, y + h - 1, w, 1, color);
    vga_fill_rect(x, y, 1, h, color);
    vga_fill_rect(x + w - 1, y, 1, h, color);
}

void vga_draw_char(int x, int y, char c, uint8_t color) {
    if (!vga_in_gfx_mode) return;
    if (x < 0 || x + 8 > GFX_WIDTH || y < 0 || y + 8 > GFX_HEIGHT) return;
    
    if (c < 0x20 || c > 0x7E) c = 0x20;
    int char_idx = c - 0x20;
    
    for (int row = 0; row < 8; row++) {
        uint8_t byte = matrix_font[char_idx][row];
        if (!byte) continue;
        for (int col = 0; col < 8; col++) {
            if (byte & (0x80 >> col)) {
                vga_plot_pixel(x + col, y + row, color);
            }
        }
    }
}

void vga_draw_string(int x, int y, const char* str, uint8_t color) {
    if (!vga_in_gfx_mode) return;
    int current_x = x;
    while (*str) {
        if (*str == '\n') {
            current_x = x;
            y += 8;
        } else {
            vga_draw_char(current_x, y, *str, color);
            current_x += 8;   /* 8x8 VGA ROM font: full cell advance */
        }
        str++;
    }
}

void vga_draw_window(int x, int y, int w, int h, const char* title) {
    if (!vga_in_gfx_mode) return;
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