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

/* Inject a set-1 scancode from a non-PS/2 source (USB HID boot keyboard).
 * ext=1 selects the extended block (arrows/Delete); release codes carry
 * the 0x80 bit.  Callable with IF=0 (IRQ/tick context). */
void keyboard_feed_scancode(uint8_t sc, int ext);

/* While a USB keyboard is bound the PS/2 IRQ handler only drains the
 * 8042 buffer (sendkey would otherwise deliver every key twice). */
void keyboard_set_ps2_enabled(int enabled);

#endif