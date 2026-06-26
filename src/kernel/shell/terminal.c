#include "shell/terminal.h"
#include "drivers/vga.h"
#include "lib/string.h"

static terminal_t* g_current_term = NULL;

/* ========== Text terminal implementation ========== */

static void text_putchar(terminal_t* t, char c) {
    (void)t;
    vga_putchar(c);
}

static void text_puts(terminal_t* t, const char* str) {
    (void)t;
    vga_puts(str);
}

static void text_set_color(terminal_t* t, uint8_t color) {
    (void)t;
    vga_set_color(color);
}

static void text_clear(terminal_t* t) {
    (void)t;
    vga_clear();
}

static terminal_t g_text_term = {
    .putchar = text_putchar,
    .puts    = text_puts,
    .set_color = text_set_color,
    .clear   = text_clear,
    .priv    = NULL
};

/* ========== GUI terminal implementation ========== */

#define GUI_TERM_COLS 35
#define GUI_TERM_ROWS 19

typedef struct {
    uint16_t cells[GUI_TERM_ROWS][GUI_TERM_COLS];
    int cursor_x;
    int cursor_y;
    uint8_t color;
    int base_x;
    int base_y;
    int left_w;
    int header_h;
    int content_h;
} gui_term_priv_t;

static gui_term_priv_t g_gui_priv;

static void gui_putchar(terminal_t* t, char c) {
    gui_term_priv_t* p = (gui_term_priv_t*)t->priv;
    if (!p) return;

    if (c == '\n') {
        p->cursor_x = 0;
        p->cursor_y++;
    } else if (c == '\r') {
        p->cursor_x = 0;
    } else if (c == '\b') {
        if (p->cursor_x > 0) {
            p->cursor_x--;
            p->cells[p->cursor_y][p->cursor_x] = ' ' | ((uint16_t)p->color << 8);
        }
    } else if (c == '\t') {
        p->cursor_x = (p->cursor_x + 4) & ~3;
        if (p->cursor_x >= GUI_TERM_COLS) {
            p->cursor_x = 0;
            p->cursor_y++;
        }
    } else {
        p->cells[p->cursor_y][p->cursor_x] = (uint16_t)c | ((uint16_t)p->color << 8);
        p->cursor_x++;
    }

    if (p->cursor_x >= GUI_TERM_COLS) {
        p->cursor_x = 0;
        p->cursor_y++;
    }

    if (p->cursor_y >= GUI_TERM_ROWS) {
        for (int i = 1; i < GUI_TERM_ROWS; i++) {
            for (int j = 0; j < GUI_TERM_COLS; j++) {
                p->cells[i - 1][j] = p->cells[i][j];
            }
        }
        for (int j = 0; j < GUI_TERM_COLS; j++) {
            p->cells[GUI_TERM_ROWS - 1][j] = ' ' | ((uint16_t)p->color << 8);
        }
        p->cursor_y = GUI_TERM_ROWS - 1;
    }
}

static void gui_puts(terminal_t* t, const char* str) {
    while (*str) {
        gui_putchar(t, *str++);
    }
}

static void gui_set_color(terminal_t* t, uint8_t color) {
    gui_term_priv_t* p = (gui_term_priv_t*)t->priv;
    if (p) p->color = color;
}

static void gui_clear(terminal_t* t) {
    gui_term_priv_t* p = (gui_term_priv_t*)t->priv;
    if (!p) return;
    for (int i = 0; i < GUI_TERM_ROWS; i++) {
        for (int j = 0; j < GUI_TERM_COLS; j++) {
            p->cells[i][j] = ' ' | ((uint16_t)p->color << 8);
        }
    }
    p->cursor_x = 0;
    p->cursor_y = 0;
}

static terminal_t g_gui_term = {
    .putchar = gui_putchar,
    .puts    = gui_puts,
    .set_color = gui_set_color,
    .clear   = gui_clear,
    .priv    = &g_gui_priv
};

/* ========== Public API ========== */

void term_init_text(void) {
    g_current_term = &g_text_term;
}

void term_init_gui(int left_w, int header_h, int content_h) {
    gui_term_priv_t* p = &g_gui_priv;
    p->left_w   = left_w;
    p->header_h = header_h;
    p->content_h = content_h;
    p->base_x   = left_w + 4;
    p->base_y   = header_h + 14;
    p->color    = 0x0F;
    p->cursor_x = 0;
    p->cursor_y = 0;

    for (int i = 0; i < GUI_TERM_ROWS; i++) {
        for (int j = 0; j < GUI_TERM_COLS; j++) {
            p->cells[i][j] = ' ' | (0x0F << 8);
        }
    }

    g_current_term = &g_gui_term;
}

void term_set(terminal_t* t) {
    g_current_term = t;
}

terminal_t* term_get(void) {
    return g_current_term;
}

void term_putchar(char c) {
    if (g_current_term && g_current_term->putchar) {
        g_current_term->putchar(g_current_term, c);
    }
}

void term_puts(const char* str) {
    if (g_current_term && g_current_term->puts) {
        g_current_term->puts(g_current_term, str);
    }
}

void term_set_color(uint8_t color) {
    if (g_current_term && g_current_term->set_color) {
        g_current_term->set_color(g_current_term, color);
    }
}

void term_clear(void) {
    if (g_current_term && g_current_term->clear) {
        g_current_term->clear(g_current_term);
    }
}

void term_gui_render(void) {
    if (g_current_term != &g_gui_term) return;
    gui_term_priv_t* p = &g_gui_priv;

    vga_fill_rect(p->left_w + 1, p->header_h + 1,
                  GFX_WIDTH - p->left_w - 2, p->content_h - 2, 0x00);

    for (int row = 0; row < GUI_TERM_ROWS; row++) {
        int y = p->base_y + row * 8;
        if (y >= GFX_HEIGHT - 20) break;

        for (int col = 0; col < GUI_TERM_COLS; col++) {
            uint16_t cell = p->cells[row][col];
            char c = (char)(cell & 0xFF);
            uint8_t color = (uint8_t)(cell >> 8);
            if (c != ' ') {
                vga_draw_char(p->base_x + col * 6, y, c, color);
            }
        }
    }
}

int term_gui_get_cursor_y(void) {
    if (g_current_term != &g_gui_term) return 0;
    gui_term_priv_t* p = &g_gui_priv;
    return p->base_y + p->cursor_y * 8;
}
