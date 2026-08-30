#ifndef TTY_H
#define TTY_H

#include "lib/types.h"

/* TTY line discipline (Phase 1.2): canonical-mode stdin over the keyboard
 * and line-oriented stdout over VGA+COM1. User processes see a Linux-like
 * console: input is echoed and editable (backspace) until Enter commits a
 * line; output \n expands to \r\n. */

/* Blocking canonical read from the console. Returns at most one committed
 * line per call (like Linux tty canonical mode). count bytes are copied
 * into buf; returns the number of bytes delivered. */
int tty_read(char* buf, size_t count);

/* Console write with \n -> \r\n expansion. Returns count. */
int tty_write(const char* buf, size_t count);

#endif
