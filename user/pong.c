/*
 * Pong - a ring 3 game for Kil0yOS (VGA mode 13h, 320x200x256)
 *
 * Controls:  UP / W  - move paddle up
 *            DOWN / S - move paddle down
 *            ESC       - quit to shell
 *
 * First to 5 wins. Renders exclusively through kernel syscalls;
 * the identity-mapped framebuffer (0xA0000) is kernel-only.
 */

/* Syscall numbers - must match include/core/syscall.h:
 * EXIT=0 READ=1 WRITE=2 OPEN=3 CLOSE=4 GETPID=5 YIELD=6 PUTS=7
 * GETCHAR=8 PUTCHAR=9 GFX_MODE=10 GFX_CLEAR=11 GFX_RECT=12
 * GFX_TEXT=13 KEY_POLL=14 */
#define SYS_EXIT      0
#define SYS_YIELD     6
#define SYS_GFX_MODE  10
#define SYS_GFX_CLEAR 11
#define SYS_GFX_RECT  12
#define SYS_GFX_TEXT  13
#define SYS_KEY_POLL  14

/* Keyboard codes from the kernel driver */
#define KEY_ESC    27
#define KEY_UP     0x80
#define KEY_DOWN   0x81

/* Screen layout */
#define W          320
#define H          200
#define PAD_W      4
#define PAD_H      28
#define PLAYER_X   8
#define AI_X       (W - PLAYER_X - PAD_W)
#define BALL_S     3
#define WIN_SCORE  5

/* mode-13h palette */
#define C_BLACK    0
#define C_WHITE    15
#define C_YELLOW   14
#define C_GREY     8
#define C_GREEN    10
#define C_RED      12

static int player_y, ai_y;
static int ball_x, ball_y, ball_dx, ball_dy;
static int score_p, score_a;
static int quit;

/* Previous-frame state for incremental rendering: each frame erases only
 * what moved instead of clearing the whole screen (which flickers). */
static int prev_ball_x, prev_ball_y;
static int prev_player_y, prev_ai_y;
static int last_score_p = -1, last_score_a = -1;

/* ---- syscall wrappers (num in rax, args in rbx rcx rdx r8 r9) ---- */

static inline long sc0(long num) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline long sc1(long num, long a0) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rbx\n\t"
        "int $0x80\n"
        : "=a"(ret)
        : "m"(a0), "a"(num)
        : "rbx", "memory");
    return ret;
}

static inline long sc3(long num, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rbx\n\t"
        "movq %2, %%rcx\n\t"
        "movq %3, %%rdx\n\t"
        "int $0x80\n"
        : "=a"(ret)
        : "m"(a0), "m"(a1), "m"(a2), "a"(num)
        : "rbx", "rcx", "rdx", "memory");
    return ret;
}

static inline long sc5(long num, long a0, long a1, long a2, long a3, long a4) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rbx\n\t"
        "movq %2, %%rcx\n\t"
        "movq %3, %%rdx\n\t"
        "movq %4, %%r8\n\t"
        "movq %5, %%r9\n\t"
        "int $0x80\n"
        : "=a"(ret)
        : "m"(a0), "m"(a1), "m"(a2), "m"(a3), "m"(a4), "a"(num)
        : "rbx", "rcx", "rdx", "r8", "r9", "memory");
    return ret;
}

static void gfx_text(int x, int y, const char* s, int color) {
    sc5(SYS_GFX_TEXT, x, y, (long)s, color, 0);
}

static void gfx_rect(int x, int y, int w, int h, int color) {
    sc5(SYS_GFX_RECT, x, y, w, h, color);
}

/* Minimal unsigned-int-to-decimal (no libc in user space) */
static void fmt_int(char* buf, int v) {
    char tmp[12];
    int n = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}

static void draw_score(void) {
    char a[8], b[8], line[24];
    int i = 0;
    fmt_int(a, score_p);
    fmt_int(b, score_a);

    for (const char* p = a; *p; p++) line[i++] = *p;
    line[i++] = ' ';
    line[i++] = ':';
    line[i++] = ' ';
    for (const char* p = b; *p; p++) line[i++] = *p;
    line[i] = '\0';

    /* ~centered on the 8x8 font grid */
    gfx_text(148, 8, line, C_WHITE);
}

/* Redraw the dashed center line inside a rectangle that was just erased
 * (the ball and the score text both sit across x = W/2-1 .. W/2). */
static void redraw_center_line_over(int x, int y, int w, int h) {
    if (x >= W / 2 + 1 || x + w <= W / 2 - 1) return;
    for (int dy = 4; dy < H; dy += 16) {
        int y0 = (y > dy) ? y : dy;
        int y1 = (y + h < dy + 8) ? y + h : dy + 8;
        if (y0 < y1) gfx_rect(W / 2 - 1, y0, 2, y1 - y0, C_GREY);
    }
}

/* Full scene (re)draw - used once after entering the game or a rematch */
static void draw_static(void) {
    sc0(SYS_GFX_CLEAR);
    for (int y = 4; y < H; y += 16) {
        gfx_rect(W / 2 - 1, y, 2, 8, C_GREY);
    }
    gfx_rect(PLAYER_X, player_y, PAD_W, PAD_H, C_GREEN);
    gfx_rect(AI_X, ai_y, PAD_W, PAD_H, C_RED);
    gfx_rect(ball_x, ball_y, BALL_S, BALL_S, C_YELLOW);
    draw_score();
    prev_ball_x = ball_x;
    prev_ball_y = ball_y;
    prev_player_y = player_y;
    prev_ai_y = ai_y;
    last_score_p = score_p;
    last_score_a = score_a;
}

/* Incremental render: erase/redraw only what changed since last frame */
static void draw_field(void) {
    /* Ball: erase old square, restore the center line under it, draw new */
    gfx_rect(prev_ball_x, prev_ball_y, BALL_S, BALL_S, C_BLACK);
    redraw_center_line_over(prev_ball_x, prev_ball_y, BALL_S, BALL_S);
    gfx_rect(ball_x, ball_y, BALL_S, BALL_S, C_YELLOW);
    prev_ball_x = ball_x;
    prev_ball_y = ball_y;

    /* Paddles: only when they moved (erase covers the overlap, so no gap) */
    if (player_y != prev_player_y) {
        gfx_rect(PLAYER_X, prev_player_y, PAD_W, PAD_H, C_BLACK);
        gfx_rect(PLAYER_X, player_y, PAD_W, PAD_H, C_GREEN);
        prev_player_y = player_y;
    }
    if (ai_y != prev_ai_y) {
        gfx_rect(AI_X, prev_ai_y, PAD_W, PAD_H, C_BLACK);
        gfx_rect(AI_X, ai_y, PAD_W, PAD_H, C_RED);
        prev_ai_y = ai_y;
    }

    /* Score: only when a point was scored ("10 : 10" is 7 glyphs = 56px) */
    if (score_p != last_score_p || score_a != last_score_a) {
        gfx_rect(144, 4, 68, 12, C_BLACK);
        redraw_center_line_over(144, 4, 68, 12);
        draw_score();
        last_score_p = score_p;
        last_score_a = score_a;
    }
}

/* Drain the keyboard queue: returns 1 if ESC was seen */
static int poll_input(void) {
    int esc = 0;
    for (;;) {
        long k = sc0(SYS_KEY_POLL);
        if (k == 0) break;
        if (k == KEY_ESC) {
            esc = 1;
        } else if (k == KEY_UP || k == 'w' || k == 'W') {
            player_y -= 6;
        } else if (k == KEY_DOWN || k == 's' || k == 'S') {
            player_y += 6;
        }
    }
    if (player_y < 2) player_y = 2;
    if (player_y > H - PAD_H - 2) player_y = H - PAD_H - 2;
    return esc;
}

static void serve(int toward_player) {
    ball_x = W / 2 - 1;
    ball_y = H / 2 - 1;
    ball_dx = toward_player ? -2 : 2;
    ball_dy = (ball_y & 1) ? 1 : -1;   /* deterministic pseudo-random */
}

static int clamp_dy(int dy) {
    if (dy > 3) return 3;
    if (dy < -3) return -3;
    if (dy == 0) return 1;
    return dy;
}

static void physics(void) {
    ball_x += ball_dx;
    ball_y += ball_dy;

    /* top / bottom walls */
    if (ball_y <= 0) {
        ball_y = 0;
        ball_dy = -ball_dy;
    } else if (ball_y >= H - BALL_S) {
        ball_y = H - BALL_S;
        ball_dy = -ball_dy;
    }

    /* left paddle */
    if (ball_dx < 0 &&
        ball_x <= PLAYER_X + PAD_W && ball_x + BALL_S >= PLAYER_X &&
        ball_y + BALL_S >= player_y && ball_y <= player_y + PAD_H) {
        ball_x = PLAYER_X + PAD_W + 1;
        ball_dx = 2;
        ball_dy = clamp_dy((ball_y + BALL_S / 2 - (player_y + PAD_H / 2)) / 8);
    }

    /* right paddle */
    if (ball_dx > 0 &&
        ball_x + BALL_S >= AI_X && ball_x <= AI_X + PAD_W &&
        ball_y + BALL_S >= ai_y && ball_y <= ai_y + PAD_H) {
        ball_x = AI_X - BALL_S - 1;
        ball_dx = -2;
        ball_dy = clamp_dy((ball_y + BALL_S / 2 - (ai_y + PAD_H / 2)) / 8);
    }

    /* scoring: serve toward the point loser */
    if (ball_x < -BALL_S) {
        score_a++;
        serve(0);
    } else if (ball_x > W) {
        score_p++;
        serve(1);
    }
}

static void ai_move(void) {
    int ball_c = ball_y + BALL_S / 2;
    int ai_c = ai_y + PAD_H / 2;
    if (ball_c > ai_c + 2) {
        ai_y += 1;
    } else if (ball_c < ai_c - 2) {
        ai_y -= 1;
    }
    if (ai_y < 2) ai_y = 2;
    if (ai_y > H - PAD_H - 2) ai_y = H - PAD_H - 2;
}

/* Block (via yield) until any key arrives; returns that key */
static long wait_key(void) {
    for (;;) {
        long k = sc0(SYS_KEY_POLL);
        if (k != 0) return k;
        sc0(SYS_YIELD);
    }
}

static void center_text(int y, const char* s, int color) {
    /* 8 px per glyph */
    int x = W / 2 - 4;
    const char* p = s;
    while (*p) {
        x -= 4;
        p++;
    }
    gfx_text(x, y, s, color);
}

static int show_banner(const char* title, const char* sub) {
    /* returns 0 = continue, 1 = quit */
    sc0(SYS_GFX_CLEAR);
    center_text(70, title, C_WHITE);
    center_text(95, sub, C_GREY);
    center_text(120, "UP/DOWN or W/S - ESC quits", C_GREY);
    long k = wait_key();
    return k == KEY_ESC;
}

static int show_winner(void) {
    /* returns 0 = play again, 1 = quit */
    sc0(SYS_GFX_CLEAR);
    draw_score();
    if (score_p > score_a) {
        center_text(85, "YOU WIN!", C_GREEN);
    } else {
        center_text(85, "AI WINS!", C_RED);
    }
    center_text(110, "any key = rematch, ESC = quit", C_GREY);
    long k = wait_key();
    return k == KEY_ESC;
}

static void reset_match(void) {
    player_y = H / 2 - PAD_H / 2;
    ai_y = player_y;
    score_p = 0;
    score_a = 0;
    quit = 0;
    serve(1);
    /* Sync incremental-render state so the first frame erases nothing */
    prev_ball_x = ball_x;
    prev_ball_y = ball_y;
    prev_player_y = player_y;
    prev_ai_y = ai_y;
    last_score_p = -1;   /* force one score repaint */
    last_score_a = -1;
}

/* .text.entry: linked first in the raw binary - the kernel's raw exec
 * enters at image base (OUTPUT_FORMAT(binary) drops ELF entry info). */
__attribute__((section(".text.entry"), used))
void _start(void) {
    /* Enter VGA mode 13h first: every gfx_* syscall is a no-op while
     * the display is still in text mode. */
    sc1(SYS_GFX_MODE, 1);

    if (show_banner("P O N G", "first to 5 wins")) {
        sc1(SYS_GFX_MODE, 0);
        sc1(SYS_EXIT, 0);
    }

    reset_match();
    draw_static();

    while (!quit) {
        if (poll_input()) break;

        physics();
        ai_move();
        draw_field();
        sc0(SYS_YIELD);

        if (score_p >= WIN_SCORE || score_a >= WIN_SCORE) {
            if (show_winner()) {
                quit = 1;
            } else {
                reset_match();
                draw_static();
            }
        }
    }

    /* Restore text mode so the shell comes back intact */
    sc1(SYS_GFX_MODE, 0);
    sc1(SYS_EXIT, 0);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
