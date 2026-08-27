#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "lib/types.h"
#include "core/isr.h"

#define KEYBOARD_PORT 0x60
#define KEYBOARD_IRQ  1

/* Special key codes produced by the keyboard driver */
#define KEY_ESC    27
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83

void keyboard_init();
char keyboard_getc();
int keyboard_has_input();
void keyboard_handler(interrupt_frame_t* frame);

#endif