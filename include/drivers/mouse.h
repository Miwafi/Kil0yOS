#ifndef MOUSE_H
#define MOUSE_H

#include "lib/types.h"
#include "core/isr.h"

#define MOUSE_IRQ 12

typedef struct mouse_state {
    int x;
    int y;
    uint8_t buttons;
    int ready;
} mouse_state_t;

void mouse_init();
void mouse_handler(interrupt_frame_t* frame);
void mouse_get_state(mouse_state_t* out);
void mouse_draw_cursor(int x, int y);
void mouse_erase_cursor(int x, int y);

/* Inject one relative motion sample from a non-PS/2 source (USB HID).
 * Convention: positive dy = down = increasing y. Callable with IF=0. */
void mouse_inject_delta(int dx, int dy, int buttons);

/* While a USB mouse is bound the PS/2 IRQ handler only drains aux bytes
 * (sendkey/mouse_move would otherwise deliver every event twice). */
void mouse_set_ps2_enabled(int enabled);

#endif
