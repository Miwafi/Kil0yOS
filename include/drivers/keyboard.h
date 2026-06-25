#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "lib/types.h"
#include "core/isr.h"

#define KEYBOARD_PORT 0x60
#define KEYBOARD_IRQ  1

void keyboard_init();
char keyboard_getc();
int keyboard_has_input();
void keyboard_handler(interrupt_frame_t* frame);

#endif