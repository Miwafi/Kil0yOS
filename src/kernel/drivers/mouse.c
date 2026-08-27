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

static void mouse_wait(uint8_t type) {
    if (type == 0) {
        while ((inb(MOUSE_STATUS_PORT) & 0x01) == 0) {
            __asm__ volatile("nop");
        }
    } else {
        while (inb(MOUSE_STATUS_PORT) & 0x02) {
            __asm__ volatile("nop");
        }
    }
}

static void mouse_write(uint8_t data) {
    mouse_wait(1);
    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_SEND_TO_MOUSE);
    mouse_wait(1);
    outb(MOUSE_DATA_PORT, data);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(MOUSE_DATA_PORT);
}

static void mouse_flush(void) {
    while (inb(MOUSE_STATUS_PORT) & 0x01) {
        inb(MOUSE_DATA_PORT);
    }
}

static void mouse_controller_reset(void) {
    mouse_flush();

    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_DISABLE);
    io_wait();

    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_READ_CFG);
    mouse_wait(0);
    uint8_t status = inb(MOUSE_DATA_PORT);
    status |= 0x02;
    status &= ~0x20;

    mouse_wait(1);
    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_WRITE_CFG);
    mouse_wait(1);
    outb(MOUSE_DATA_PORT, status);

    outb(MOUSE_COMMAND_PORT, MOUSE_CMD_ENABLE);
    io_wait();

    mouse_write(MOUSE_CMD_ENABLE_REPORTING);
    uint8_t ack = mouse_read();
    (void)ack;
}

void mouse_handler(interrupt_frame_t* frame) {
    (void)frame;

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

            mouse.x += dx;
            mouse.y -= dy;

            if (mouse.x < 0) mouse.x = 0;
            if (mouse.y < 0) mouse.y = 0;
            /* keep cursor fully on-screen: guard in mouse_draw_cursor uses x+CURSOR_W > GFX_WIDTH */
            if (mouse.x > GFX_WIDTH - CURSOR_W)  mouse.x = GFX_WIDTH - CURSOR_W;
            if (mouse.y > GFX_HEIGHT - CURSOR_H) mouse.y = GFX_HEIGHT - CURSOR_H;

            mouse.buttons = mouse_packet[0] & 0x07;
            mouse_cycle = 0;
            break;
    }

    pic_send_eoi(MOUSE_IRQ);
}

void mouse_init() {
    mouse_controller_reset();
    register_irq_handler(MOUSE_IRQ, mouse_handler);
    pic_enable_irq(MOUSE_IRQ);
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
