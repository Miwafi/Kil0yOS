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

#endif
