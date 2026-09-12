#include "shell/terminal.h"
#include "drivers/vga.h"
#include "drivers/fb.h"
#include "drivers/io.h"
#include "lib/string.h"

/* Serial console mirror (COM1): headless QEMU acceptance tests read shell
 * I/O from the serial log. Mirrors the same approach used by tty.c for
 * program output; without it builtin ls/touch output is GUI-only. */
static void term_serial_putc(char c) {
    /* Bounded THR wait - same rationale as klog's serial_putc: a stalled
     * serial back-end must never hang the shell. */
    int guard = 100000;
    while ((inb(0x3F8 + 5) & 0x20) == 0) {
        if (--guard == 0) return;
    }
    outb(0x3F8, (uint8_t)c);
}

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

/* ========== Framebuffer terminal implementation (UEFI GOP) ========== */

static void fbterm_putchar(terminal_t* t, char c) {
    (void)t;
    fb_putchar(c);
}

static void fbterm_puts(terminal_t* t, const char* str) {
    (void)t;
    fb_puts(str);
}

static void fbterm_set_color(terminal_t* t, uint8_t color) {
    (void)t;
    fb_set_color(color);
}

static void fbterm_clear(terminal_t* t) {
    (void)t;
    fb_clear();
}

static terminal_t g_fb_term = {
    .putchar = fbterm_putchar,
    .puts    = fbterm_puts,
    .set_color = fbterm_set_color,
    .clear   = fbterm_clear,
    .priv    = NULL
};

/* ========== GUI terminal implementation ========== */
/* Cells grid used by both desktops. The mode13h desktop uses a 26x19 grid
 * inside its 216px content panel; the GOP desktop computes its grid from
 * the framebuffer size. MAX bounds size the static cells buffer. */
#define GUI_TERM_MAX_COLS 192
#define GUI_TERM_MAX_ROWS 96

typedef struct {
    uint16_t cells[GUI_TERM_MAX_ROWS][GUI_TERM_MAX_COLS];
    int cursor_x;
    int cursor_y;
    uint8_t color;
    int cols;               /* active grid size (<= MAX)          */
    int rows;
    int base_x;             /* glyph origin on the active surface */
    int base_y;
    int clr_x, clr_y;       /* repaint region (cells area)        */
    int clr_w, clr_h;
    int use_fb;             /* render backend: 0=vga mode13h, 1=gop fb */
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
        if (p->cursor_x >= p->cols) {
            p->cursor_x = 0;
            p->cursor_y++;
        }
    } else {
        p->cells[p->cursor_y][p->cursor_x] = (uint16_t)c | ((uint16_t)p->color << 8);
        p->cursor_x++;
    }

    if (p->cursor_x >= p->cols) {
        p->cursor_x = 0;
        p->cursor_y++;
    }

    if (p->cursor_y >= p->rows) {
        for (int i = 1; i < p->rows; i++) {
            for (int j = 0; j < p->cols; j++) {
                p->cells[i - 1][j] = p->cells[i][j];
            }
        }
        for (int j = 0; j < p->cols; j++) {
            p->cells[p->rows - 1][j] = ' ' | ((uint16_t)p->color << 8);
        }
        p->cursor_y = p->rows - 1;
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
    for (int i = 0; i < p->rows; i++) {
        for (int j = 0; j < p->cols; j++) {
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
    /* GOP path: the fb terminal replaces the VGA text terminal (vga.c
     * already neutered itself via the vga_buffer sentinel). */
    g_current_term = fb_is_active() ? &g_fb_term : &g_text_term;
}

/* Shared GUI-terminal geometry setup (both desktops / panes). */
static void gui_term_setup(int use_fb, int base_x, int base_y, int cols, int rows,
                           int clr_x, int clr_y, int clr_w, int clr_h) {
    gui_term_priv_t* p = &g_gui_priv;
    p->use_fb   = use_fb;
    p->cols     = (cols > GUI_TERM_MAX_COLS) ? GUI_TERM_MAX_COLS : cols;
    p->rows     = (rows > GUI_TERM_MAX_ROWS) ? GUI_TERM_MAX_ROWS : rows;
    if (p->cols < 1) p->cols = 1;
    if (p->rows < 1) p->rows = 1;
    p->base_x   = base_x;
    p->base_y   = base_y;
    p->clr_x    = clr_x;
    p->clr_y    = clr_y;
    p->clr_w    = clr_w;
    p->clr_h    = clr_h;
    p->color    = 0x00;   /* black text on the white panel */
    p->cursor_x = 0;
    p->cursor_y = 0;

    for (int i = 0; i < p->rows; i++) {
        for (int j = 0; j < p->cols; j++) {
            p->cells[i][j] = ' ' | (0x00 << 8);
        }
    }

    g_current_term = &g_gui_term;
}

/* Desktop shell terminal on the mode13h surface: caller computes grid and
 * repaint region for the pane it wants the terminal in. */
void term_init_gui_at(int base_x, int base_y, int cols, int rows,
                      int clr_x, int clr_y, int clr_w, int clr_h) {
    gui_term_setup(0, base_x, base_y, cols, rows, clr_x, clr_y, clr_w, clr_h);
}

/* GOP desktop shell terminal: grid computed from the framebuffer size by
 * the caller; rendered through the fb_gfx primitives instead of the
 * mode13h surface. */
void term_init_gop_gui(int base_x, int base_y, int cols, int rows,
                       int clr_x, int clr_y, int clr_w, int clr_h) {
    gui_term_setup(1, base_x, base_y, cols, rows, clr_x, clr_y, clr_w, clr_h);
}

void term_set(terminal_t* t) {
    g_current_term = t;
}

terminal_t* term_get(void) {
    return g_current_term;
}

void term_putchar(char c) {
    /* Serial console mirror: headless QEMU acceptance tests read terminal
     * I/O from the serial log (same approach as tty.c for program output).
     * Applied at the term level so both text and GUI terminals mirror. */
    if (c == '\n') term_serial_putc('\r');
    if (c == '\n' || c == '\r' || (c >= 32 && c <= 126)) term_serial_putc(c);
    if (g_current_term && g_current_term->putchar) {
        g_current_term->putchar(g_current_term, c);
    }
}

void term_puts(const char* str) {
    /* Route through term_putchar so every char hits the serial mirror
     * exactly once, regardless of the active terminal implementation. */
    while (str && *str) {
        term_putchar(*str++);
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

    /* clear cells area only - keeps the "Shell Terminal" title above base_y intact */
    if (p->use_fb) {
        fb_gfx_fill_rect(p->clr_x, p->clr_y, p->clr_w, p->clr_h, 0x0F);
    } else {
        vga_fill_rect(p->clr_x, p->clr_y, p->clr_w, p->clr_h, 0x0F);
    }

    for (int row = 0; row < p->rows; row++) {
        int y = p->base_y + row * 8;
        if (y >= p->clr_y + p->clr_h) break;

        for (int col = 0; col < p->cols; col++) {
            uint16_t cell = p->cells[row][col];
            char c = (char)(cell & 0xFF);
            uint8_t color = (uint8_t)(cell >> 8);
            if (c != ' ') {
                int x = p->base_x + col * 8;
                if (p->use_fb) fb_gfx_draw_char(x, y, c, color);
                else           vga_draw_char(x, y, c, color);
            }
        }
    }
}

int term_gui_get_cursor_y(void) {
    if (g_current_term != &g_gui_term) return 0;
    gui_term_priv_t* p = &g_gui_priv;
    return p->base_y + p->cursor_y * 8;
}

/* Echo one printable input character into the GUI terminal cells and on-screen.
   Keeps the typed line in cells so full re-renders do not erase user input. */
void term_gui_type_char(char c) {
    if (g_current_term != &g_gui_term) return;
    gui_term_priv_t* p = &g_gui_priv;

    int px = p->base_x + p->cursor_x * 8;
    int py = p->base_y + p->cursor_y * 8;

    /* stay inside the content panel and the visible terminal region */
    if (px + 8 > p->clr_x + p->clr_w - 2) return;
    if (py >= p->clr_y + p->clr_h) return;

    gui_putchar(&g_gui_term, c);          /* update cells + cursor */
    if (p->use_fb) fb_gfx_draw_char(px, py, c, p->color);
    else           vga_draw_char(px, py, c, p->color);
}

/* Backspace for interactive input: clear previous cell and move cursor back. */
void term_gui_backspace(void) {
    if (g_current_term != &g_gui_term) return;
    gui_term_priv_t* p = &g_gui_priv;

    if (p->cursor_x == 0 && p->cursor_y == 0) return;

    if (p->cursor_x > 0) {
        p->cursor_x--;
    } else {
        p->cursor_y--;
        p->cursor_x = p->cols - 1;
    }

    p->cells[p->cursor_y][p->cursor_x] = ' ' | ((uint16_t)p->color << 8);
    if (p->use_fb) {
        fb_gfx_fill_rect(p->base_x + p->cursor_x * 8,
                         p->base_y + p->cursor_y * 8, 8, 8, 0x0F);
    } else {
        vga_fill_rect(p->base_x + p->cursor_x * 8,
                      p->base_y + p->cursor_y * 8, 8, 8, 0x0F);
    }
}
