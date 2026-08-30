#include "core/tty.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"
#include "drivers/io.h"

/* Line being composed; committed lines queue here for readers */
#define TTY_LINE_MAX 1024

static char line_buf[TTY_LINE_MAX];
static size_t line_len = 0;

static char ready_buf[TTY_LINE_MAX];
static size_t ready_len = 0;
static int ready_flag = 0;

/* Serial console mirror so headless QEMU runs see program I/O */
static void tty_serial_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
}

/* Raw echo to console + serial (handles \r\n expansion) */
static void tty_echo(char c) {
    if (c == '\n') {
        vga_putchar('\r');
        tty_serial_putc('\r');
    }
    vga_putchar(c);
    tty_serial_putc(c);
}

static void tty_commit(void) {
    /* Move the composed line into the ready buffer */
    for (size_t i = 0; i < line_len; i++) ready_buf[i] = line_buf[i];
    ready_len = line_len;
    ready_flag = 1;
    line_len = 0;
}

int tty_read(char* buf, size_t count) {
    if (buf == NULL || count == 0) return 0;

    /* Compose a line: echo, edit (backspace), commit on Enter.
     * Special extended-key codes (0x80-0x83 arrows) are ignored. */
    for (;;) {
        char c = keyboard_getc();   /* blocks (hlt) until a key arrives */

        if (c == '\r' || c == '\n') {
            tty_echo('\n');
            if (line_len < TTY_LINE_MAX) line_buf[line_len++] = '\n';
            tty_commit();
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (line_len > 0) {
                line_len--;
                vga_putchar('\b');
                tty_serial_putc('\b');
                vga_putchar(' ');
                tty_serial_putc(' ');
                vga_putchar('\b');
                tty_serial_putc('\b');
            }
            continue;
        }
        if (c == 3) {               /* ^C: discard the composed line */
            tty_echo('^');
            tty_echo('C');
            tty_echo('\n');
            line_len = 0;
            continue;
        }
        if (c == 4) {               /* ^D on empty line: EOF */
            if (line_len == 0) {
                ready_len = 0;
                ready_flag = 1;
                break;
            }
            /* Non-empty: behave like Enter (commit what was typed) */
            tty_echo('\n');
            if (line_len < TTY_LINE_MAX) line_buf[line_len++] = '\n';
            tty_commit();
            break;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x80 ||
            ((unsigned char)c > 0x80 && (unsigned char)c < 0xA0)) {
            continue;               /* ignore control/extended codes */
        }
        if (line_len >= TTY_LINE_MAX - 1) continue;
        line_buf[line_len++] = c;
        tty_echo(c);
    }

    /* Deliver at most one line, truncated to count */
    size_t n = (ready_len < count) ? ready_len : count;
    for (size_t i = 0; i < n; i++) buf[i] = ready_buf[i];
    ready_flag = 0;
    return (int)n;
}

int tty_write(const char* buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char c = buf[i];
        if (c == '\n') {
            vga_putchar('\r');
            tty_serial_putc('\r');
        }
        vga_putchar(c);
        tty_serial_putc(c);
    }
    return (int)count;
}
