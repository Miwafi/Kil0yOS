#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

typedef struct terminal terminal_t;

struct terminal {
    void (*putchar)(terminal_t* t, char c);
    void (*puts)(terminal_t* t, const char* str);
    void (*set_color)(terminal_t* t, uint8_t color);
    void (*clear)(terminal_t* t);
    void* priv;
};

void term_init_text(void);
/* Desktop terminal placed at an arbitrary pane region: the caller computes
 * the glyph grid and the repaint rect (mode13h surface backend). */
void term_init_gui_at(int base_x, int base_y, int cols, int rows,
                      int clr_x, int clr_y, int clr_w, int clr_h);
/* Same, on the GOP framebuffer (fb_gfx backend). */
void term_init_gop_gui(int base_x, int base_y, int cols, int rows,
                       int clr_x, int clr_y, int clr_w, int clr_h);
void term_set(terminal_t* t);
terminal_t* term_get(void);

void term_putchar(char c);
void term_puts(const char* str);
void term_set_color(uint8_t color);
void term_clear(void);

void term_gui_render(void);
int term_gui_get_cursor_y(void);
void term_gui_type_char(char c);
void term_gui_backspace(void);

#endif
