#include "drivers/mouse.h"
#include "drivers/io.h"
#include "core/interrupts.h"
#include "drivers/vga.h"

#define MOUSE_DATA_PORT    0x60
#define MOUSE_STATUS_PORT  0x64
#define MOUSE_COMMAND_PORT 0x64

#define MOUSE_CMD_ENABLE        0xA8
#define MOUSE_CMD_DISABLE       0xA7
#define MOUSE_CMD_READ_CFG      0x20
#define MOUSE_CMD_WRITE_CFG     0x60
#define MOUSE_CMD_SEND_TO_MOUSE 0xD4
#define MOUSE_CMD_ENABLE_REPORTING 0xF4
#define MOUSE_ACK               0xFA

static mouse_state_t mouse = { .x = 160, .y = 100, .buttons = 0, .ready = 0 };

static uint8_t mouse_packet[3];
static int mouse_cycle = 0;

#define CURSOR_W 3
#define CURSOR_H 3

static uint8_t cursor_pattern[CURSOR_H][CURSOR_W] = {
    { 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x0F },
    { 0x00, 0x0F, 0x00 }
};

static uint8_t saved_pixels[CURSOR_H][CURSOR_W];
static int saved_x = -1;
static int saved_y = -1;
static int cursor_visible = 0;

/* Bounded waits: an EFI firmware may leave the aux channel owned by its
 * own (vmmouse) driver in a state that never ACKs, so every poll must
 * time out instead of hanging the boot.  Threshold >= 1000000 per the
 * mouse bring-up rule (ISA port reads ~1us each, 2M is ~2s worst case). */
#define MOUSE_WAIT_LOOPS 2000000

static int mouse_wait_timeout(uint8_t type) {
    uint32_t n = MOUSE_WAIT_LOOPS;
    if (type == 0) {
        while ((inb(MOUSE_STATUS_PORT) & 0x01) == 0) {
            if (--n == 0) return 0;
            __asm__ volatile("nop");
        }
    } else {
        while (inb(MOUSE_STATUS_PORT) & 0x02) {
            if (--n == 0) return 0;
            __asm__ volatile("nop");
        }
    }
    return 1;
}

static int mouse_write(uint8_t data) {
    if (!mouse_wait_timeout(1)) return 0;
    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_SEND_TO_MOUSE);
    if (!mouse_wait_timeout(1)) return 0;
    outb(MOUSE_DATA_PORT, data);
    return 1;
}

static int mouse_read(uint8_t* out) {
    if (!mouse_wait_timeout(0)) return 0;
    *out = inb(MOUSE_DATA_PORT);
    return 1;
}

static void mouse_flush(void) {
    while (inb(MOUSE_STATUS_PORT) & 0x01) {
        inb(MOUSE_DATA_PORT);
    }
}

/* Device-level reset: FF -> ACK(FA) -> self-test AA 00.  This is what
 * recovers a device that a firmware driver left in its own mode. */
static int mouse_reset_device(void) {
    uint8_t b;
    if (!mouse_write(0xFF)) return 0;
    if (!mouse_read(&b) || b != MOUSE_ACK) return 0;
    if (!mouse_read(&b) || b != 0xAA) return 0;
    if (!mouse_read(&b) || b != 0x00) return 0;
    return 1;
}

/* Returns 1 when the aux channel is live and reporting; 0 leaves the
 * channel disabled (IRQ12 masked by the caller) so the boot continues. */
static int mouse_controller_reset(void) {
    uint8_t status, ack;

    mouse_flush();

    /* start from a known controller state: disable aux, enable the aux
     * interrupt (IRQ12 bit) and the aux clock in the config byte */
    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_DISABLE);
    io_wait();

    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_READ_CFG);
    if (!mouse_read(&status)) {
        klog("[mouse] 8042 config read timeout\n");
        return 0;
    }
    status |= 0x02;
    status &= ~0x20;

    if (!mouse_wait_timeout(1)) {
        klog("[mouse] 8042 ibf stuck\n");
        return 0;
    }
    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_WRITE_CFG);
    if (!mouse_wait_timeout(1)) {
        klog("[mouse] 8042 cfg write stuck\n");
        return 0;
    }
    outb(MOUSE_DATA_PORT, status);

    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_ENABLE);
    io_wait();
    mouse_flush();

    if (!mouse_reset_device()) {
        klog("[mouse] device reset failed (firmware-owned aux?)\n");
        goto fail;
    }
    if (!mouse_write(MOUSE_CMD_ENABLE_REPORTING)) {
        klog("[mouse] enable write timeout\n");
        goto fail;
    }
    if (!mouse_read(&ack)) {
        klog("[mouse] enable ack timeout\n");
        goto fail;
    }
    if (ack != MOUSE_ACK) {
        klog_hex("[mouse] enable ack=0x", ack);
        klog("\n");
        goto fail;
    }
    return 1;
fail:
    /* park the aux channel: no IRQ12 storm from stray firmware bytes */
    mouse_flush();
    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_DISABLE);
    io_wait();
    return 0;
}

/* Integrate one motion sample (shared by the PS/2 and USB HID paths).
 * USB convention: positive dy = down = increasing y; the PS/2 handler
 * negates dy before calling in. */
void mouse_inject_delta(int dx, int dy, int buttons) {
    mouse.x += dx;
    mouse.y += dy;

    if (mouse.x < 0) mouse.x = 0;
    if (mouse.y < 0) mouse.y = 0;
    /* keep cursor fully on-screen: guard in mouse_draw_cursor uses x+CURSOR_W > GFX_WIDTH */
    if (mouse.x > GFX_WIDTH - CURSOR_W)  mouse.x = GFX_WIDTH - CURSOR_W;
    if (mouse.y > GFX_HEIGHT - CURSOR_H) mouse.y = GFX_HEIGHT - CURSOR_H;

    mouse.buttons = (uint8_t)(buttons & 0x07);
    mouse.ready = 1;
}

static int ps2_enabled = 1;

void mouse_set_ps2_enabled(int enabled) {
    ps2_enabled = enabled;
}

void mouse_handler(interrupt_frame_t* frame) {
    (void)frame;

    if (!ps2_enabled) {
        /* USB mouse owns the pointer: drain any pending aux bytes */
        while (inb(MOUSE_STATUS_PORT) & 0x01) {
            (void)inb(MOUSE_DATA_PORT);
        }
        pic_send_eoi(MOUSE_IRQ);
        return;
    }

    uint8_t status = inb(MOUSE_STATUS_PORT);
    if ((status & 0x01) == 0) {
        pic_send_eoi(MOUSE_IRQ);
        return;
    }
    if ((status & 0x20) == 0) {
        pic_send_eoi(MOUSE_IRQ);
        return;
    }

    uint8_t data = inb(MOUSE_DATA_PORT);

    switch (mouse_cycle) {
        case 0:
            if ((data & 0x08) == 0) {
                pic_send_eoi(MOUSE_IRQ);
                return;
            }
            mouse_packet[0] = data;
            mouse_cycle = 1;
            break;
        case 1:
            mouse_packet[1] = data;
            mouse_cycle = 2;
            break;
        case 2:
            mouse_packet[2] = data;
            mouse.ready = 1;

            int dx = (int)mouse_packet[1];
            int dy = (int)mouse_packet[2];

            if (mouse_packet[0] & 0x40) dx = 0;
            if (mouse_packet[0] & 0x80) dy = 0;

            if (mouse_packet[0] & 0x10) dx -= 256;
            if (mouse_packet[0] & 0x20) dy -= 256;

            /* PS/2: positive dy = up -> negate for the shared integrator */
            mouse_inject_delta(dx, -dy, mouse_packet[0] & 0x07);
            mouse_cycle = 0;
            break;
    }

    pic_send_eoi(MOUSE_IRQ);
}

void mouse_init() {
    if (mouse_controller_reset()) {
        register_irq_handler(MOUSE_IRQ, mouse_handler);
        pic_enable_irq(MOUSE_IRQ);
    } else {
        /* aux channel unusable (e.g. firmware vmmouse left it wedged):
         * IRQ12 stays masked, the boot continues without a PS/2 mouse
         * (USB HID can still claim the pointer via mouse_set_ps2_enabled) */
        mouse_set_ps2_enabled(0);
    }
}

void mouse_get_state(mouse_state_t* out) {
    disable_interrupts();
    *out = mouse;
    enable_interrupts();
}

void mouse_draw_cursor(int x, int y) {
    /* graphics mode only, avoid writing into text-mode VRAM window */
    if (!vga_is_graphics()) return;

    /* redraw while visible: restore old background first to avoid stale-pixel artifacts */
    if (cursor_visible) {
        mouse_erase_cursor(x, y);
    }

    if (x < 0 || y < 0 || x + CURSOR_W > GFX_WIDTH || y + CURSOR_H > GFX_HEIGHT) return;

    saved_x = x;
    saved_y = y;

    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            int px = x + col;
            int py = y + row;
            saved_pixels[row][col] = vga_gfx_buffer[py * GFX_WIDTH + px];
            vga_gfx_buffer[py * GFX_WIDTH + px] = cursor_pattern[row][col];
        }
    }
    cursor_visible = 1;
}

void mouse_erase_cursor(int x, int y) {
    (void)x; (void)y; /* position kept for API compat; actual pos is remembered internally */

    if (!cursor_visible || saved_x < 0 || saved_y < 0) return;
    if (!vga_is_graphics()) { cursor_visible = 0; return; }

    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            int px = saved_x + col;
            int py = saved_y + row;
            if (px >= 0 && py >= 0 && px < GFX_WIDTH && py < GFX_HEIGHT) {
                vga_gfx_buffer[py * GFX_WIDTH + px] = saved_pixels[row][col];
            }
        }
    }

    saved_x = -1;
    saved_y = -1;
    cursor_visible = 0;
}
