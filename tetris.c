/*
 * ultimate-terminal-tetris
 *
 * A guideline tetris for the terminal: SRS with wall kicks and 180 degree
 * spins, 7-bag randomizer, hold, ghost piece, lock delay, T-spins,
 * back-to-back, combos, perfect clears, four game modes and configurable
 * DAS/ARR/SDF handling.
 *
 * Compilation: clang -O2 -o tetris tetris.c
 *
 * By Alextout.
 *
 * It grew out of the C port in Kirill Timofeev's tetris, a recreation of the
 * tetris that ran on soviet DVK machines (PDP-11 clones). That project is at
 * https://github.com/kt97679/tetris and is under the WTFPL; none of its code
 * is left here, but it is where this one started and it deserves the credit.
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * Constants
 * ------------------------------------------------------------------ */

#define BOARD_W 10
#define BOARD_H 40              /* 20 visible rows plus 20 rows of buffer */
#define VISIBLE_H 20
#define TOP_ROW (BOARD_H - VISIBLE_H)
#define NEXT_COUNT 5

#define MAX_SCREEN_W 200
#define MAX_SCREEN_H 60
#define MIN_SCREEN_W 62
#define MIN_SCREEN_H 24

#define LOCK_DELAY_US 500000L
#define LOCK_RESET_LIMIT 15
#define LINE_CLEAR_ANIM_US 150000L
#define RESTART_HOLD_US 700000L /* hold r this long to throw the run away */
#define NAME_MAX 16
#define BOARD_MAX 200
#define BOARD_PER_MODE 50
#define BOARD_SHOWN 10
#define FRAME_US 16666L         /* ~60 Hz */

enum { PIECE_I, PIECE_J, PIECE_L, PIECE_O, PIECE_S, PIECE_T, PIECE_Z, PIECE_COUNT };

enum { ROT_0, ROT_R, ROT_2, ROT_L };

enum { MODE_MARATHON, MODE_SPRINT, MODE_ULTRA, MODE_ZEN, MODE_COUNT };

enum { STYLE_SOLID, STYLE_CLASSIC, STYLE_COUNT };

enum { STATE_MENU, STATE_PLAYING, STATE_PAUSED, STATE_CLEARING, STATE_GAMEOVER };

enum { MENU_NAME, MENU_MARATHON, MENU_SPRINT, MENU_ULTRA, MENU_ZEN,
       MENU_BOARD, MENU_COUNT };

enum { SPIN_NONE, SPIN_MINI, SPIN_FULL };

#define MARATHON_GOAL 150       /* lines */
#define SPRINT_GOAL 40          /* lines */
#define ULTRA_TIME_US 120000000L

/* ------------------------------------------------------------------ *
 * Piece geometry
 *
 * Every rotation state is stored explicitly as four (x, y) cells inside the
 * piece's bounding box, with y growing downwards. Rotation is a table lookup;
 * the wall kicks below then nudge the result into place.
 * ------------------------------------------------------------------ */

static const int8_t SHAPES[PIECE_COUNT][4][4][2] = {
    [PIECE_I] = {
        {{0, 1}, {1, 1}, {2, 1}, {3, 1}},
        {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
        {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
        {{1, 0}, {1, 1}, {1, 2}, {1, 3}},
    },
    [PIECE_J] = {
        {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 0}, {1, 1}, {0, 2}, {1, 2}},
    },
    [PIECE_L] = {
        {{2, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
        {{0, 0}, {1, 0}, {1, 1}, {1, 2}},
    },
    [PIECE_O] = {
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
    },
    [PIECE_S] = {
        {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
        {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
    },
    [PIECE_T] = {
        {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {1, 2}},
    },
    [PIECE_Z] = {
        {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
        {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {0, 2}},
    },
};

/* Spawn position of the bounding box, chosen so the piece appears in the top
 * two visible rows in the guideline's spawn columns. */
static const int8_t SPAWN_X[PIECE_COUNT] = {
    [PIECE_I] = 3, [PIECE_J] = 3, [PIECE_L] = 3, [PIECE_O] = 3,
    [PIECE_S] = 3, [PIECE_T] = 3, [PIECE_Z] = 3,
};
static const int8_t SPAWN_Y[PIECE_COUNT] = {
    [PIECE_I] = TOP_ROW - 1, [PIECE_J] = TOP_ROW, [PIECE_L] = TOP_ROW,
    [PIECE_O] = TOP_ROW, [PIECE_S] = TOP_ROW, [PIECE_T] = TOP_ROW,
    [PIECE_Z] = TOP_ROW,
};

/* SRS wall kicks, transcribed with y growing downwards (the published tables
 * use y up, so every y is negated here). Indexed [from state][0 = CW, 1 = CCW]. */
static const int8_t KICKS_JLSTZ[4][2][5][2] = {
    [ROT_0] = {
        {{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}},
        {{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}},
    },
    [ROT_R] = {
        {{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}},
        {{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}},
    },
    [ROT_2] = {
        {{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}},
        {{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}},
    },
    [ROT_L] = {
        {{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}},
        {{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}},
    },
};

static const int8_t KICKS_I[4][2][5][2] = {
    [ROT_0] = {
        {{0, 0}, {-2, 0}, {1, 0}, {-2, 1}, {1, -2}},
        {{0, 0}, {-1, 0}, {2, 0}, {-1, -2}, {2, 1}},
    },
    [ROT_R] = {
        {{0, 0}, {-1, 0}, {2, 0}, {-1, -2}, {2, 1}},
        {{0, 0}, {2, 0}, {-1, 0}, {2, -1}, {-1, 2}},
    },
    [ROT_2] = {
        {{0, 0}, {2, 0}, {-1, 0}, {2, -1}, {-1, 2}},
        {{0, 0}, {1, 0}, {-2, 0}, {1, 2}, {-2, -1}},
    },
    [ROT_L] = {
        {{0, 0}, {1, 0}, {-2, 0}, {1, 2}, {-2, -1}},
        {{0, 0}, {-2, 0}, {1, 0}, {-2, 1}, {1, -2}},
    },
};

/* 180 degree spins are not part of classic SRS. This is a small symmetric
 * kick set in the spirit of SRS+, enough to make the spin feel natural
 * against walls and floors without inventing exotic twists. */
static const int8_t KICKS_180[9][2] = {
    {0, 0}, {0, -1}, {0, 1}, {1, 0}, {-1, 0},
    {1, -1}, {-1, -1}, {2, 0}, {-2, 0},
};

/* ------------------------------------------------------------------ *
 * Colours
 *
 * Every colour the game draws is a palette index. The palette knows how to
 * render itself in truecolour, in 256 colours and in the 8 ANSI colours, so
 * the renderer never has to care which terminal it is talking to.
 * ------------------------------------------------------------------ */

enum {
    C_DEFAULT, C_I, C_J, C_L, C_O, C_S, C_T, C_Z,
    C_GHOST, C_BORDER, C_TEXT, C_DIM, C_ACCENT, C_WARN, C_GOOD, C_COUNT
};

typedef struct {
    uint8_t r, g, b;
    uint8_t idx256;
    uint8_t ansi;               /* 30..37, or 39 for default */
    uint8_t bold;
} palette_entry;

static const palette_entry PALETTE[C_COUNT] = {
    [C_DEFAULT] = {200, 200, 200, 250, 39, 0},
    [C_I]       = { 32, 214, 214,  44, 36, 1},
    [C_J]       = { 60,  90, 220,  27, 34, 1},
    [C_L]       = {230, 130,  30, 208, 33, 1},
    [C_O]       = {230, 200,  40, 220, 33, 1},
    [C_S]       = { 60, 200,  80,  40, 32, 1},
    [C_T]       = {180,  70, 210,  99, 35, 1},
    [C_Z]       = {225,  55,  55, 196, 31, 1},
    [C_GHOST]   = {110, 110, 120, 244, 37, 0},
    [C_BORDER]  = {120, 120, 140, 245, 33, 0},
    [C_TEXT]    = {225, 225, 230, 253, 37, 0},
    [C_DIM]     = {120, 120, 130, 243, 30, 1},
    [C_ACCENT]  = { 90, 200, 250,  81, 36, 1},
    [C_WARN]    = {240, 180,  60, 214, 33, 1},
    [C_GOOD]    = { 90, 220, 120,  84, 32, 1},
};

static const uint8_t PIECE_COLOR[PIECE_COUNT] = {
    [PIECE_I] = C_I, [PIECE_J] = C_J, [PIECE_L] = C_L, [PIECE_O] = C_O,
    [PIECE_S] = C_S, [PIECE_T] = C_T, [PIECE_Z] = C_Z,
};

/* ------------------------------------------------------------------ *
 * Configuration
 * ------------------------------------------------------------------ */

/* Logical actions the player can bind keys to. */
enum {
    ACT_LEFT, ACT_RIGHT, ACT_SOFT, ACT_HARD, ACT_CW, ACT_CCW, ACT_180,
    ACT_HOLD, ACT_PAUSE, ACT_RESTART, ACT_QUIT, ACT_STYLE, ACT_HELP, ACT_COUNT
};

#define MAX_BINDS 4

typedef struct {
    long das_us;                /* delayed auto shift */
    long arr_us;                /* auto repeat rate, 0 = instant to the wall */
    int sdf;                    /* soft drop speed in rows per second, 0 = instant */
    int style;
    int mode;
    int ghost;
    int color_depth;            /* 0 = autodetect */
    int binds[ACT_COUNT][MAX_BINDS];
} config_t;

/* Key ids: printable ASCII maps to itself, special keys live above 0x100 so
 * they never collide with a character. */
enum {
    K_LEFT = 0x100, K_RIGHT, K_UP, K_DOWN, K_ESC, K_ENTER, K_SPACE, K_TAB,
    K_LSHIFT, K_RSHIFT, K_LCTRL, K_RCTRL, K_BACKSPACE, K_MAX
};

static config_t g_cfg = {
    .das_us = 266000,   /* NES: 16 frames before the auto shift starts */
    .arr_us = 100000,   /* NES: one cell every 6 frames after that */
    .sdf = 30,          /* NES: one row every two frames at 60 Hz */
    .style = STYLE_SOLID,
    .mode = MODE_MARATHON,
    .ghost = 1,
    .color_depth = 0,
    .binds = {
        [ACT_LEFT]    = {K_LEFT, 0, 0, 0},
        [ACT_RIGHT]   = {K_RIGHT, 0, 0, 0},
        [ACT_SOFT]    = {K_DOWN, 0, 0, 0},
        [ACT_HARD]    = {K_SPACE, 0, 0, 0},
        [ACT_CW]      = {K_UP, 'x', 0, 0},
        [ACT_CCW]     = {'z', K_LCTRL, K_RCTRL, 0},
        [ACT_180]     = {'a', 0, 0, 0},
        [ACT_HOLD]    = {'c', K_LSHIFT, K_RSHIFT, K_TAB},
        [ACT_PAUSE]   = {K_ESC, 'p', 0, 0},
        [ACT_RESTART] = {'r', 0, 0, 0},
        [ACT_QUIT]    = {'q', 0, 0, 0},
        [ACT_STYLE]   = {'v', 0, 0, 0},
        [ACT_HELP]    = {'h', 0, 0, 0},
    },
};

/* ------------------------------------------------------------------ *
 * Terminal
 * ------------------------------------------------------------------ */

static struct termios g_termios_saved;
static int g_termios_valid = 0;
static int g_alt_screen = 0;
static int g_kitty_active = 0;
static volatile sig_atomic_t g_resized = 1;
static volatile sig_atomic_t g_interrupted = 0;
static int g_color_depth = 24;

static void out_raw(const char *s)
{
    size_t len = strlen(s);
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(STDOUT_FILENO, s + done, len - done);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        done += (size_t)n;
    }
}

static void term_restore(void)
{
    if (g_kitty_active) {
        out_raw("\033[<u");
        g_kitty_active = 0;
    }
    out_raw("\033[?25h\033[0m");
    if (g_alt_screen) {
        out_raw("\033[?1049l");
        g_alt_screen = 0;
    }
    if (g_termios_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_termios_saved);
        g_termios_valid = 0;
    }
}

static void on_fatal_signal(int sig)
{
    term_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_interrupt(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

static void on_winch(int sig)
{
    (void)sig;
    g_resized = 1;
}

static void term_init(void)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &g_termios_saved) == 0) {
        g_termios_valid = 1;
        raw = g_termios_saved;
        raw.c_lflag &= ~(unsigned long)(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(unsigned long)(IXON | ICRNL | INLCR | BRKINT);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    atexit(term_restore);
    signal(SIGSEGV, on_fatal_signal);
    signal(SIGBUS, on_fatal_signal);
    signal(SIGABRT, on_fatal_signal);
    signal(SIGTERM, on_fatal_signal);
    signal(SIGHUP, on_fatal_signal);
    signal(SIGINT, on_interrupt);
    signal(SIGWINCH, on_winch);

    out_raw("\033[?1049h\033[2J\033[?25l");
    g_alt_screen = 1;
}

static void detect_color_depth(void)
{
    const char *ct = getenv("COLORTERM");
    const char *term = getenv("TERM");

    if (g_cfg.color_depth) {
        g_color_depth = g_cfg.color_depth;
        return;
    }
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) {
        g_color_depth = 24;
    } else if (term && strstr(term, "256")) {
        g_color_depth = 256;
    } else if (term && strcmp(term, "dumb") != 0) {
        g_color_depth = 8;
    } else {
        g_color_depth = 0;
    }
}

/* ------------------------------------------------------------------ *
 * Screen buffer
 *
 * The game draws into a back buffer of cells; every frame only the cells that
 * actually changed are serialised and handed to the terminal in a single
 * write. Keeping a frame small and atomic is what stops the terminal from
 * ever seeing a half-drawn board.
 * ------------------------------------------------------------------ */

typedef struct {
    char ch[4];
    uint8_t fg;
    uint8_t bg;
    uint8_t bold;
} cell_t;

static cell_t g_front[MAX_SCREEN_H][MAX_SCREEN_W];
static cell_t g_back[MAX_SCREEN_H][MAX_SCREEN_W];
static int g_scr_w = 80;
static int g_scr_h = 24;

static char g_out[512 * 1024];
static size_t g_out_len;
static size_t g_frame_bytes;    /* size of the last frame, for --selftest */

static void out_reset(void)
{
    g_out_len = 0;
}

static void out_add(const char *s, size_t n)
{
    if (g_out_len + n >= sizeof(g_out)) {
        return;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
}

static void out_str(const char *s)
{
    out_add(s, strlen(s));
}

static void out_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        out_add(buf, (size_t)n);
    }
}

static void term_size(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        g_scr_w = ws.ws_col;
        g_scr_h = ws.ws_row;
    } else {
        g_scr_w = 80;
        g_scr_h = 24;
    }
    if (g_scr_w > MAX_SCREEN_W) {
        g_scr_w = MAX_SCREEN_W;
    }
    if (g_scr_h > MAX_SCREEN_H) {
        g_scr_h = MAX_SCREEN_H;
    }
}

static void scr_clear(void)
{
    int y, x;

    for (y = 0; y < g_scr_h; y++) {
        for (x = 0; x < g_scr_w; x++) {
            g_back[y][x].ch[0] = ' ';
            g_back[y][x].ch[1] = '\0';
            g_back[y][x].fg = C_DEFAULT;
            g_back[y][x].bg = C_DEFAULT;
            g_back[y][x].bold = 0;
        }
    }
}

/* Force the next flush to repaint everything. */
static void scr_invalidate(void)
{
    memset(g_front, 0xff, sizeof(g_front));
}

static void scr_put(int x, int y, const char *ch, int fg, int bg, int bold)
{
    cell_t *c;

    if (x < 0 || y < 0 || x >= g_scr_w || y >= g_scr_h) {
        return;
    }
    c = &g_back[y][x];
    snprintf(c->ch, sizeof(c->ch), "%s", ch);
    c->fg = (uint8_t)fg;
    c->bg = (uint8_t)bg;
    c->bold = (uint8_t)bold;
}

/* Draw a two-column cell, the unit the board is made of. */
static void scr_put2(int x, int y, const char *a, const char *b, int fg, int bg, int bold)
{
    scr_put(x, y, a, fg, bg, bold);
    scr_put(x + 1, y, b, fg, bg, bold);
}

static void scr_text(int x, int y, int fg, int bold, const char *fmt, ...)
{
    char buf[256];
    char ch[2] = {0, 0};
    va_list ap;
    int i;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (i = 0; buf[i]; i++) {
        ch[0] = buf[i];
        scr_put(x + i, y, ch, fg, C_DEFAULT, bold);
    }
}

/* Text whose first `filled` columns sit on a coloured background: a progress
 * bar drawn behind the label rather than next to it. */
static void scr_text_bar(int x, int y, int fg, int bold, int filled,
                         int bar_color, const char *text)
{
    char ch[2] = {0, 0};
    int i;

    for (i = 0; text[i]; i++) {
        ch[0] = text[i];
        scr_put(x + i, y, ch, i < filled ? C_TEXT : fg,
                i < filled ? bar_color : C_DEFAULT, bold);
    }
}

static void sgr_color(int idx, int background)
{
    const palette_entry *p = &PALETTE[idx];

    if (idx == C_DEFAULT || g_color_depth == 0) {
        out_str(background ? "\033[49m" : "\033[39m");
        return;
    }
    if (g_color_depth >= 24) {
        out_fmt("\033[%d;2;%d;%d;%dm", background ? 48 : 38, p->r, p->g, p->b);
    } else if (g_color_depth >= 256) {
        out_fmt("\033[%d;5;%dm", background ? 48 : 38, p->idx256);
    } else {
        out_fmt("\033[%dm", background ? p->ansi + 10 : p->ansi);
    }
}

static void scr_flush(void)
{
    int y, x, run;
    int cur_fg = -1, cur_bg = -1, cur_bold = -1;
    int cursor_x = -1, cursor_y = -1;

    out_reset();
    out_str("\033[0m");

    for (y = 0; y < g_scr_h; y++) {
        for (x = 0; x < g_scr_w; x++) {
            cell_t *b = &g_back[y][x];
            cell_t *f = &g_front[y][x];

            if (b->fg == f->fg && b->bg == f->bg && b->bold == f->bold &&
                strcmp(b->ch, f->ch) == 0) {
                continue;
            }
            if (cursor_y != y || cursor_x != x) {
                out_fmt("\033[%d;%dH", y + 1, x + 1);
                cursor_y = y;
                cursor_x = x;
            }
            /* Emit this cell and any directly following changed cells; tolerate
             * short unchanged gaps, which is cheaper than a cursor jump. */
            run = 0;
            while (x < g_scr_w && run < 4) {
                b = &g_back[y][x];
                f = &g_front[y][x];
                if (b->fg == f->fg && b->bg == f->bg && b->bold == f->bold &&
                    strcmp(b->ch, f->ch) == 0) {
                    run++;
                } else {
                    run = 0;
                }
                if (b->bold != cur_bold) {
                    out_str(b->bold ? "\033[1m" : "\033[22m");
                    cur_bold = b->bold;
                }
                if (b->fg != cur_fg) {
                    sgr_color(b->fg, 0);
                    cur_fg = b->fg;
                }
                if (b->bg != cur_bg) {
                    sgr_color(b->bg, 1);
                    cur_bg = b->bg;
                }
                out_str(b->ch);
                *f = *b;
                cursor_x++;
                x++;
            }
            x--;
        }
    }

    out_str("\033[0m");
    g_frame_bytes = g_out_len;
    if (g_out_len > 2) {
        out_add("", 1);
        g_out[g_out_len - 1] = '\0';
        out_raw(g_out);
    }
}

/* ------------------------------------------------------------------ *
 * Game state
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t board[BOARD_H][BOARD_W];

    int type;                   /* active piece, -1 when none */
    int rot;
    int x, y;                   /* bounding box position */

    int hold;
    int hold_used;

    int queue[NEXT_COUNT + 7];  /* visible previews plus a spare bag */
    int queue_len;
    int bag[PIECE_COUNT];
    int bag_pos;

    long score;
    int lines;
    int level;
    int combo;
    int b2b;
    int pieces;

    int last_was_rotation;
    int last_kick;
    int spin;                   /* SPIN_* of the move that locked the piece */

    long gravity_acc;
    long lock_acc;
    int lock_resets;
    int lowest_y;               /* deepest row the piece has reached */
    int on_ground;

    int mode;
    int state;
    long elapsed_us;
    long clear_anim_us;
    int clear_rows[4];
    int clear_count;

    /* last event, for the side panel */
    char event[48];
    long event_us;

    int topped_out;
    int finished;
    char player[NAME_MAX + 1];
} game_t;

static game_t g_game;
static uint64_t g_rng_state;

/* ------------------------------------------------------------------ *
 * Randomizer
 * ------------------------------------------------------------------ */

static void rng_seed(uint64_t seed)
{
    g_rng_state = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

static uint32_t rng_next(void)
{
    /* xorshift64*, so a fixed seed always replays the same game */
    uint64_t x = g_rng_state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng_state = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

static uint32_t rng_below(uint32_t bound)
{
    return rng_next() % bound;
}

static void bag_refill(game_t *g)
{
    int i;

    for (i = 0; i < PIECE_COUNT; i++) {
        g->bag[i] = i;
    }
    for (i = PIECE_COUNT - 1; i > 0; i--) {
        int j = (int)rng_below((uint32_t)(i + 1));
        int t = g->bag[i];
        g->bag[i] = g->bag[j];
        g->bag[j] = t;
    }
    g->bag_pos = 0;
}

static int bag_take(game_t *g)
{
    if (g->bag_pos >= PIECE_COUNT) {
        bag_refill(g);
    }
    return g->bag[g->bag_pos++];
}

static void queue_fill(game_t *g)
{
    while (g->queue_len < NEXT_COUNT) {
        g->queue[g->queue_len++] = bag_take(g);
    }
}

static int queue_pop(game_t *g)
{
    int first;
    int i;

    queue_fill(g);
    first = g->queue[0];
    for (i = 1; i < g->queue_len; i++) {
        g->queue[i - 1] = g->queue[i];
    }
    g->queue_len--;
    queue_fill(g);
    return first;
}

/* ------------------------------------------------------------------ *
 * Board and collision
 * ------------------------------------------------------------------ */

static void piece_cells(int type, int rot, int px, int py, int out[4][2])
{
    int i;

    for (i = 0; i < 4; i++) {
        out[i][0] = px + SHAPES[type][rot][i][0];
        out[i][1] = py + SHAPES[type][rot][i][1];
    }
}

static int cell_blocked(const game_t *g, int x, int y)
{
    if (x < 0 || x >= BOARD_W || y >= BOARD_H) {
        return 1;
    }
    if (y < 0) {
        return 0;               /* above the buffer is free space */
    }
    return g->board[y][x] != 0;
}

static int collides(const game_t *g, int type, int rot, int px, int py)
{
    int cells[4][2];
    int i;

    piece_cells(type, rot, px, py, cells);
    for (i = 0; i < 4; i++) {
        if (cell_blocked(g, cells[i][0], cells[i][1])) {
            return 1;
        }
    }
    return 0;
}

static int ghost_y(const game_t *g)
{
    int y = g->y;

    while (!collides(g, g->type, g->rot, g->x, y + 1)) {
        y++;
    }
    return y;
}

/* ------------------------------------------------------------------ *
 * Gravity and levels
 * ------------------------------------------------------------------ */

static long gravity_us(int level)
{
    /* Guideline: (0.8 - (level-1) * 0.007) ^ (level-1) seconds per row.
     * Computed iteratively so the binary needs no libm. */
    double base, sec = 1.0;
    int i;

    if (level < 1) {
        level = 1;
    }
    if (level > 20) {
        level = 20;
    }
    base = 0.8 - (double)(level - 1) * 0.007;
    for (i = 0; i < level - 1; i++) {
        sec *= base;
    }
    return (long)(sec * 1000000.0);
}

static int level_for(const game_t *g)
{
    if (g->mode == MODE_SPRINT) {
        return 1;
    }
    return g->lines / 10 + 1;
}

/* ------------------------------------------------------------------ *
 * Spawning
 * ------------------------------------------------------------------ */

static void note_event(game_t *g, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g->event, sizeof(g->event), fmt, ap);
    va_end(ap);
    g->event_us = 0;
}

static void reset_lock(game_t *g)
{
    g->lock_acc = 0;
    g->lock_resets = 0;
    g->lowest_y = g->y;
    g->on_ground = 0;
}

static int spawn_piece(game_t *g, int type)
{
    g->type = type;
    g->rot = ROT_0;
    g->x = SPAWN_X[type];
    g->y = SPAWN_Y[type];
    g->last_was_rotation = 0;
    g->last_kick = 0;
    g->gravity_acc = 0;
    reset_lock(g);

    if (collides(g, g->type, g->rot, g->x, g->y)) {
        /* Block out: the piece cannot appear at all. */
        g->topped_out = 1;
        g->state = STATE_GAMEOVER;
        return 0;
    }
    return 1;
}

static int spawn_next(game_t *g)
{
    g->hold_used = 0;
    return spawn_piece(g, queue_pop(g));
}

static void do_hold(game_t *g)
{
    int prev;

    if (g->hold_used) {
        return;
    }
    prev = g->hold;
    g->hold = g->type;
    g->hold_used = 1;
    if (prev < 0) {
        spawn_piece(g, queue_pop(g));
    } else {
        spawn_piece(g, prev);
    }
    g->hold_used = 1;
}

/* ------------------------------------------------------------------ *
 * Movement and rotation
 * ------------------------------------------------------------------ */

static int try_move(game_t *g, int dx, int dy)
{
    if (collides(g, g->type, g->rot, g->x + dx, g->y + dy)) {
        return 0;
    }
    g->x += dx;
    g->y += dy;
    g->last_was_rotation = 0;
    if (g->y > g->lowest_y) {
        g->lowest_y = g->y;
        g->lock_resets = 0;
    }
    if (dx != 0 || dy != 0) {
        if (g->on_ground && g->lock_resets < LOCK_RESET_LIMIT) {
            g->lock_acc = 0;
            g->lock_resets++;
        }
    }
    return 1;
}

/* dir: 1 = clockwise, -1 = counter-clockwise, 2 = 180 degrees */
static int try_rotate(game_t *g, int dir)
{
    int from = g->rot;
    int to;
    int i, tests;
    const int8_t (*table)[2];

    if (g->type == PIECE_O) {
        return 0;               /* O has no meaningful rotation */
    }

    if (dir == 2) {
        to = (from + 2) & 3;
        table = KICKS_180;
        tests = (int)(sizeof(KICKS_180) / sizeof(KICKS_180[0]));
    } else {
        to = (from + (dir > 0 ? 1 : 3)) & 3;
        table = (g->type == PIECE_I) ? KICKS_I[from][dir > 0 ? 0 : 1]
                                     : KICKS_JLSTZ[from][dir > 0 ? 0 : 1];
        tests = 5;
    }

    for (i = 0; i < tests; i++) {
        int nx = g->x + table[i][0];
        int ny = g->y + table[i][1];

        if (!collides(g, g->type, to, nx, ny)) {
            g->rot = to;
            g->x = nx;
            g->y = ny;
            g->last_was_rotation = 1;
            g->last_kick = i;
            if (g->y > g->lowest_y) {
                g->lowest_y = g->y;
                g->lock_resets = 0;
            }
            if (g->on_ground && g->lock_resets < LOCK_RESET_LIMIT) {
                g->lock_acc = 0;
                g->lock_resets++;
            }
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * T-spin detection
 *
 * Three-corner rule: a T that locks right after a rotation with at least
 * three of its four diagonal corners occupied counts as a spin. If only one
 * of the two corners it faces is filled it is a mini, unless the rotation
 * needed the last kick offset, which is always a full spin.
 * ------------------------------------------------------------------ */

static int detect_spin(const game_t *g)
{
    static const int8_t CORNERS[4][2] = {{0, 0}, {2, 0}, {0, 2}, {2, 2}};
    /* Which two corners the T faces, per rotation state. */
    static const int8_t FRONT[4][2] = {
        [ROT_0] = {0, 1}, [ROT_R] = {1, 3}, [ROT_2] = {2, 3}, [ROT_L] = {0, 2},
    };
    int filled[4];
    int front = 0, back = 0;
    int i;

    if (g->type != PIECE_T || !g->last_was_rotation) {
        return SPIN_NONE;
    }
    for (i = 0; i < 4; i++) {
        filled[i] = cell_blocked(g, g->x + CORNERS[i][0], g->y + CORNERS[i][1]);
    }
    for (i = 0; i < 4; i++) {
        int is_front = (i == FRONT[g->rot][0] || i == FRONT[g->rot][1]);

        if (filled[i]) {
            if (is_front) {
                front++;
            } else {
                back++;
            }
        }
    }
    if (front + back < 3) {
        return SPIN_NONE;
    }
    if (front == 2) {
        return SPIN_FULL;
    }
    return (g->last_kick == 4) ? SPIN_FULL : SPIN_MINI;
}

/* ------------------------------------------------------------------ *
 * Locking, line clears and scoring
 * ------------------------------------------------------------------ */

static int board_empty(const game_t *g)
{
    int y, x;

    for (y = 0; y < BOARD_H; y++) {
        for (x = 0; x < BOARD_W; x++) {
            if (g->board[y][x]) {
                return 0;
            }
        }
    }
    return 1;
}

static int find_full_rows(game_t *g)
{
    int y, x;

    g->clear_count = 0;
    for (y = 0; y < BOARD_H && g->clear_count < 4; y++) {
        int full = 1;

        for (x = 0; x < BOARD_W; x++) {
            if (!g->board[y][x]) {
                full = 0;
                break;
            }
        }
        if (full) {
            g->clear_rows[g->clear_count++] = y;
        }
    }
    return g->clear_count;
}

static void collapse_rows(game_t *g)
{
    int i, y, x;

    for (i = 0; i < g->clear_count; i++) {
        for (y = g->clear_rows[i]; y > 0; y--) {
            for (x = 0; x < BOARD_W; x++) {
                g->board[y][x] = g->board[y - 1][x];
            }
        }
        for (x = 0; x < BOARD_W; x++) {
            g->board[0][x] = 0;
        }
    }
    g->clear_count = 0;
}

static void award(game_t *g, int cleared, int spin)
{
    static const long LINE_SCORE[5] = {0, 100, 300, 500, 800};
    static const long SPIN_SCORE[5] = {400, 800, 1200, 1600, 0};
    static const long MINI_SCORE[3] = {100, 200, 400};
    static const long PC_SCORE[5] = {0, 800, 1200, 1800, 2000};
    long base = 0;
    int level = g->level;
    int difficult = 0;
    const char *name = NULL;

    if (spin == SPIN_FULL) {
        base = SPIN_SCORE[cleared];
        difficult = (cleared > 0);
        name = cleared == 0 ? "T-SPIN" :
               cleared == 1 ? "T-SPIN SINGLE" :
               cleared == 2 ? "T-SPIN DOUBLE" : "T-SPIN TRIPLE";
    } else if (spin == SPIN_MINI) {
        base = MINI_SCORE[cleared > 2 ? 2 : cleared];
        difficult = (cleared > 0);
        name = cleared == 0 ? "T-SPIN MINI" :
               cleared == 1 ? "T-SPIN MINI SINGLE" : "T-SPIN MINI DOUBLE";
    } else {
        base = LINE_SCORE[cleared];
        difficult = (cleared == 4);
        name = cleared == 1 ? "SINGLE" : cleared == 2 ? "DOUBLE" :
               cleared == 3 ? "TRIPLE" : cleared == 4 ? "TETRIS" : NULL;
    }

    if (cleared > 0) {
        if (difficult && g->b2b > 0) {
            base = base * 3 / 2;
        }
        if (difficult) {
            g->b2b++;
        } else {
            g->b2b = 0;
        }
        g->combo++;
        if (g->combo > 1) {
            base += 50L * (g->combo - 1);
        }
    } else {
        g->combo = 0;
    }

    g->score += base * level;

    if (cleared > 0 && board_empty(g)) {
        long pc = PC_SCORE[cleared];

        if (g->b2b > 1 && cleared == 4) {
            pc = 3200;
        }
        g->score += pc * level;
        note_event(g, "PERFECT CLEAR");
    } else if (name) {
        if (g->combo > 1) {
            note_event(g, "%s  x%d", name, g->combo);
        } else {
            note_event(g, "%s", name);
        }
    }
}

static void lock_piece(game_t *g)
{
    int cells[4][2];
    int i;
    int cleared;
    int spin = detect_spin(g);
    int highest = BOARD_H;

    piece_cells(g->type, g->rot, g->x, g->y, cells);
    for (i = 0; i < 4; i++) {
        int x = cells[i][0];
        int y = cells[i][1];

        if (y >= 0 && y < BOARD_H && x >= 0 && x < BOARD_W) {
            g->board[y][x] = (uint8_t)(g->type + 1);
        }
        if (y < highest) {
            highest = y;
        }
    }
    g->pieces++;

    cleared = find_full_rows(g);
    g->lines += cleared;
    award(g, cleared, spin);
    g->level = level_for(g);
    g->spin = spin;

    /* Lock out: the whole piece came to rest above the visible field. */
    if (highest < TOP_ROW && cleared == 0) {
        int all_above = 1;

        for (i = 0; i < 4; i++) {
            if (cells[i][1] >= TOP_ROW) {
                all_above = 0;
                break;
            }
        }
        if (all_above) {
            g->topped_out = 1;
            g->state = STATE_GAMEOVER;
            return;
        }
    }

    if (cleared > 0) {
        g->state = STATE_CLEARING;
        g->clear_anim_us = 0;
        g->type = -1;
    } else {
        spawn_next(g);
    }
}

static void hard_drop(game_t *g)
{
    int target = ghost_y(g);

    g->score += 2L * (target - g->y);
    g->y = target;
    g->last_was_rotation = 0;
    lock_piece(g);
}

/* ------------------------------------------------------------------ *
 * Mode goals
 * ------------------------------------------------------------------ */

static int mode_finished(const game_t *g)
{
    switch (g->mode) {
    case MODE_MARATHON:
        return g->lines >= MARATHON_GOAL;
    case MODE_SPRINT:
        return g->lines >= SPRINT_GOAL;
    case MODE_ULTRA:
        return g->elapsed_us >= ULTRA_TIME_US;
    default:
        return 0;
    }
}

static void game_reset(game_t *g, int mode)
{
    memset(g, 0, sizeof(*g));
    g->mode = mode;
    g->hold = -1;
    g->type = -1;
    g->level = 1;
    g->state = STATE_PLAYING;
    bag_refill(g);
    queue_fill(g);
    spawn_next(g);
}

/* ------------------------------------------------------------------ *
 * Input
 *
 * With the kitty keyboard protocol the terminal reports key releases, which
 * is what makes real DAS/ARR possible: we can tell how long a key has been
 * held. Without it we only see the operating system's auto repeat and have to
 * guess, which is noticeably worse but still playable.
 * ------------------------------------------------------------------ */

#define KITTY_FLAGS 11          /* disambiguate | report event types | report all keys */
#define FALLBACK_RELEASE_US 60000L
#define ESC_TIMEOUT_US 30000L

typedef struct {
    int down;
    long pressed_at;
    int edges;                  /* unconsumed presses */
    long last_seen;             /* for the fallback release heuristic */
} key_state;

static key_state g_keys[K_MAX];
static char g_inbuf[4096];
static size_t g_inlen;
static long g_esc_since;

/* Characters as typed, case intact, for the name field. The key state below
 * lowercases everything because bindings do not care about shift. */
static char g_text[64];
static int g_text_len;

static void text_push(int c)
{
    if (c >= 32 && c < 127 && g_text_len < (int)sizeof(g_text) - 1) {
        g_text[g_text_len++] = (char)c;
    }
}

static long now_us(void)
{
    struct timeval t;

    gettimeofday(&t, NULL);
    return t.tv_sec * 1000000L + t.tv_usec;
}

static int map_kitty_code(int code)
{
    switch (code) {
    case 27: return K_ESC;
    case 13: return K_ENTER;
    case 9: return K_TAB;
    case 32: return K_SPACE;
    case 127: return K_BACKSPACE;
    case 57441: return K_LSHIFT;
    case 57442: return K_LCTRL;
    case 57447: return K_RSHIFT;
    case 57448: return K_RCTRL;
    default: break;
    }
    if (code >= 32 && code < 127) {
        return tolower(code);
    }
    return -1;
}

static void key_press(int key, long ts)
{
    if (key < 0 || key >= K_MAX) {
        return;
    }
    if (!g_keys[key].down) {
        g_keys[key].down = 1;
        g_keys[key].pressed_at = ts;
    }
    g_keys[key].edges++;
    g_keys[key].last_seen = ts;
}

static void key_repeat(int key, long ts)
{
    if (key < 0 || key >= K_MAX) {
        return;
    }
    g_keys[key].last_seen = ts;
    if (!g_keys[key].down) {
        key_press(key, ts);
    }
}

static void key_release(int key)
{
    if (key < 0 || key >= K_MAX) {
        return;
    }
    g_keys[key].down = 0;
}

static void keys_clear(void)
{
    memset(g_keys, 0, sizeof(g_keys));
}

static int key_down(int key)
{
    return (key > 0 && key < K_MAX) ? g_keys[key].down : 0;
}

static int action_down(int act)
{
    int i;

    for (i = 0; i < MAX_BINDS; i++) {
        if (g_cfg.binds[act][i] && key_down(g_cfg.binds[act][i])) {
            return 1;
        }
    }
    return 0;
}

/* Consume one press edge for an action. */
static int action_pressed(int act)
{
    int i, got = 0;

    for (i = 0; i < MAX_BINDS; i++) {
        int k = g_cfg.binds[act][i];

        if (k > 0 && k < K_MAX && g_keys[k].edges > 0) {
            g_keys[k].edges = 0;
            got = 1;
        }
    }
    return got;
}

static long action_pressed_at(int act)
{
    long best = 0;
    int i;

    for (i = 0; i < MAX_BINDS; i++) {
        int k = g_cfg.binds[act][i];

        if (k > 0 && k < K_MAX && g_keys[k].down && g_keys[k].pressed_at > best) {
            best = g_keys[k].pressed_at;
        }
    }
    return best;
}

/* Parse one CSI sequence starting at g_inbuf[i] (which is ESC). Returns the
 * number of bytes consumed, or 0 if the sequence is still incomplete. */
static size_t parse_csi(size_t i, long ts)
{
    size_t j = i + 2;
    size_t start = j;
    char final = 0;
    int params[8] = {0};
    int sub[8] = {0};
    int nparam = 0;
    int in_sub = 0;
    int have_digit = 0;

    while (j < g_inlen) {
        char c = g_inbuf[j];

        if (c >= '0' && c <= '9') {
            if (nparam < 8) {
                if (in_sub) {
                    sub[nparam] = sub[nparam] * 10 + (c - '0');
                } else {
                    params[nparam] = params[nparam] * 10 + (c - '0');
                }
            }
            have_digit = 1;
        } else if (c == ':') {
            in_sub = 1;
        } else if (c == ';') {
            if (nparam < 7) {
                nparam++;
            }
            in_sub = 0;
        } else if (c >= 0x40 && c <= 0x7e) {
            final = c;
            j++;
            break;
        } else if (c == '?' || c == '>' || c == '<' || c == '=') {
            /* private marker, ignore for key events */
        } else {
            break;
        }
        j++;
    }
    if (!final) {
        return (j >= g_inlen) ? 0 : (j - i + 1);
    }
    if (have_digit) {
        nparam++;
    }

    if (final == 'u') {
        int key = map_kitty_code(params[0]);
        int event = sub[1] ? sub[1] : 1;
        int mods = params[1] > 0 ? params[1] - 1 : 0;

        if (event != 3 && params[0] >= 32 && params[0] < 127) {
            /* the protocol reports the unshifted key, so apply shift here */
            text_push((mods & 1) ? toupper(params[0]) : params[0]);
        }
        if (event == 1) {
            key_press(key, ts);
        } else if (event == 2) {
            key_repeat(key, ts);
        } else {
            key_release(key);
        }
    } else if (final >= 'A' && final <= 'D') {
        static const int arrows[4] = {K_UP, K_DOWN, K_RIGHT, K_LEFT};
        int key = arrows[final - 'A'];
        int event = sub[1] ? sub[1] : 1;

        if (event == 1) {
            key_press(key, ts);
        } else if (event == 2) {
            key_repeat(key, ts);
        } else {
            key_release(key);
        }
    }
    (void)start;
    return j - i;
}

static void input_poll(void)
{
    char buf[512];
    ssize_t n;
    size_t i = 0;
    long ts = now_us();

    for (;;) {
        n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (g_inlen + (size_t)n < sizeof(g_inbuf)) {
            memcpy(g_inbuf + g_inlen, buf, (size_t)n);
            g_inlen += (size_t)n;
        }
        if ((size_t)n < sizeof(buf)) {
            break;
        }
    }

    while (i < g_inlen) {
        unsigned char c = (unsigned char)g_inbuf[i];

        if (c == 0x1b) {
            if (i + 1 >= g_inlen) {
                break;          /* wait for more bytes */
            }
            if (g_inbuf[i + 1] == '[') {
                size_t used = parse_csi(i, ts);

                if (used == 0) {
                    break;
                }
                i += used;
                continue;
            }
            if (g_inbuf[i + 1] == 'O') {
                if (i + 2 >= g_inlen) {
                    break;
                }
                switch (g_inbuf[i + 2]) {
                case 'A': key_press(K_UP, ts); break;
                case 'B': key_press(K_DOWN, ts); break;
                case 'C': key_press(K_RIGHT, ts); break;
                case 'D': key_press(K_LEFT, ts); break;
                default: break;
                }
                i += 3;
                continue;
            }
            key_press(K_ESC, ts);
            i++;
            continue;
        }
        if (c == '\r' || c == '\n') {
            key_press(K_ENTER, ts);
        } else if (c == '\t') {
            key_press(K_TAB, ts);
        } else if (c == ' ') {
            key_press(K_SPACE, ts);
            text_push(' ');
        } else if (c == 3) {
            g_interrupted = 1;
        } else if (c == 127 || c == 8) {
            key_press(K_BACKSPACE, ts);
        } else if (c >= 32 && c < 127) {
            text_push(c);
            key_press(tolower(c), ts);
        }
        i++;
    }

    if (i > 0) {
        memmove(g_inbuf, g_inbuf + i, g_inlen - i);
        g_inlen -= i;
    }

    /* A lone escape is indistinguishable from the start of an escape sequence
     * until the next byte arrives, so it is only released once nothing has
     * followed it for a short while. */
    if (g_inlen > 0 && (unsigned char)g_inbuf[0] == 0x1b) {
        if (g_esc_since == 0) {
            g_esc_since = ts;
        } else if (ts - g_esc_since > ESC_TIMEOUT_US) {
            key_press(K_ESC, ts);
            memmove(g_inbuf, g_inbuf + 1, g_inlen - 1);
            g_inlen--;
            g_esc_since = 0;
        }
    } else {
        g_esc_since = 0;
    }
}

/* Without the kitty protocol there are no release events, so a key counts as
 * released once the operating system's auto repeat stops arriving. */
static void fallback_expire_keys(long ts)
{
    int k;

    if (g_kitty_active) {
        return;
    }
    for (k = 0; k < K_MAX; k++) {
        if (g_keys[k].down && ts - g_keys[k].last_seen > FALLBACK_RELEASE_US) {
            g_keys[k].down = 0;
        }
    }
}

/* Ask the terminal whether it speaks the kitty keyboard protocol and switch
 * it on if it does. The query is answered with CSI ? <flags> u; terminals that
 * do not know it stay silent, so a short timeout is the whole detection. */
static void kitty_enable(void)
{
    fd_set fs;
    struct timeval tv;
    char buf[64];
    ssize_t n;

    out_raw("\033[?u");
    FD_ZERO(&fs);
    FD_SET(STDIN_FILENO, &fs);
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    if (select(STDIN_FILENO + 1, &fs, NULL, NULL, &tv) <= 0) {
        return;
    }
    n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    if (strstr(buf, "\033[?") && buf[n - 1] == 'u') {
        char cmd[32];

        snprintf(cmd, sizeof(cmd), "\033[>%du", KITTY_FLAGS);
        out_raw(cmd);
        g_kitty_active = 1;
    }
}

/* ------------------------------------------------------------------ *
 * Handling: DAS, ARR and soft drop
 * ------------------------------------------------------------------ */

static long g_restart_hold;
static int g_shift_dir;
static int g_das_charged;
static int g_soft_held;
static long g_das_acc;
static long g_arr_acc;

static void update_shift(game_t *g, long dt)
{
    int left = action_down(ACT_LEFT);
    int right = action_down(ACT_RIGHT);
    int dir = 0;

    int tap_left = action_pressed(ACT_LEFT);
    int tap_right = action_pressed(ACT_RIGHT);

    if (left && right) {
        /* The more recently pressed direction wins, as in most modern games. */
        dir = (action_pressed_at(ACT_LEFT) > action_pressed_at(ACT_RIGHT)) ? -1 : 1;
    } else if (left) {
        dir = -1;
    } else if (right) {
        dir = 1;
    }

    /* A tap so short that press and release land in the same frame still has
     * to move the piece, so the press edge triggers the initial step. */
    if (!dir && (tap_left || tap_right)) {
        try_move(g, tap_left ? -1 : 1, 0);
        g_shift_dir = 0;
        g_das_acc = 0;
        g_arr_acc = 0;
        g_das_charged = 0;
        return;
    }

    if (dir != g_shift_dir) {
        g_shift_dir = dir;
        g_das_acc = 0;
        g_arr_acc = 0;
        g_das_charged = 0;
        if (dir) {
            try_move(g, dir, 0);
        }
        return;
    }
    if (!dir) {
        return;
    }

    g_das_acc += dt;
    if (!g_das_charged) {
        if (g_das_acc < g_cfg.das_us) {
            return;
        }
        g_das_charged = 1;
        if (g_cfg.arr_us <= 0) {
            while (try_move(g, dir, 0)) {
                /* ARR 0 slides all the way to the wall */
            }
            return;
        }
        /* The first auto shift lands the moment the delay runs out, not one
         * repeat later. Whatever the frame overshot by carries into the next
         * repeat so the rate does not drift. */
        g_arr_acc = g_das_acc - g_cfg.das_us;
        if (!try_move(g, dir, 0)) {
            return;
        }
    } else {
        if (g_cfg.arr_us <= 0) {
            while (try_move(g, dir, 0)) {
                /* still held against the wall */
            }
            return;
        }
        g_arr_acc += dt;
    }

    while (g_arr_acc >= g_cfg.arr_us) {
        g_arr_acc -= g_cfg.arr_us;
        if (!try_move(g, dir, 0)) {
            break;
        }
    }
}

static void update_gravity(game_t *g, long dt)
{
    long grav = gravity_us(g->level);
    int soft = action_down(ACT_SOFT);
    int soft_tap = action_pressed(ACT_SOFT);
    /* A press the player is only starting now, as opposed to one being held.
     * The second half catches a tap so short that press and release land in
     * the same frame; without the protocol's release events the key would
     * still read as up by the time we look. */
    int soft_start = (soft && !g_soft_held) || (soft_tap && !soft);

    g_soft_held = soft;

    if (soft_start) {
        /* The gravity counter holds up to a whole second at low levels. Once
         * soft drop shortens the interval, that backlog would be cashed in as
         * several rows at once and the piece would jump. A fresh press starts
         * the counter from zero and takes exactly one step. Repeats from the
         * operating system must not land here, or they would drive the fall
         * speed instead of the configured soft drop factor. */
        g->gravity_acc = 0;
        if (try_move(g, 0, 1)) {
            g->score++;
        }
    }
    if (soft) {
        long soft_iv;

        if (g_cfg.sdf <= 0) {
            while (try_move(g, 0, 1)) {
                g->score++;
            }
            g->gravity_acc = 0;
            return;
        }
        /* A fixed rate, as on the NES, rather than a multiple of gravity: the
         * soft drop feels the same at every level. It can only ever speed the
         * piece up, so at high levels where gravity is already faster it does
         * nothing. */
        soft_iv = 1000000L / g_cfg.sdf;
        if (soft_iv < grav) {
            grav = soft_iv;
        }
        if (grav < 1) {
            grav = 1;
        }
    }

    g->gravity_acc += dt;
    while (g->gravity_acc >= grav) {
        g->gravity_acc -= grav;
        if (try_move(g, 0, 1)) {
            if (soft) {
                g->score++;
            }
        } else {
            g->gravity_acc = 0;
            break;
        }
    }
}

static void update_playing(game_t *g, long dt)
{
    if (g->type < 0) {
        return;
    }

    if (action_pressed(ACT_CW)) {
        try_rotate(g, 1);
    }
    if (action_pressed(ACT_CCW)) {
        try_rotate(g, -1);
    }
    if (action_pressed(ACT_180)) {
        try_rotate(g, 2);
    }
    if (action_pressed(ACT_HOLD)) {
        do_hold(g);
    }
    if (g->type < 0 || g->state != STATE_PLAYING) {
        return;
    }
    if (action_pressed(ACT_HARD)) {
        hard_drop(g);
        return;
    }

    update_shift(g, dt);
    if (g->state != STATE_PLAYING || g->type < 0) {
        return;
    }
    update_gravity(g, dt);
    if (g->state != STATE_PLAYING || g->type < 0) {
        return;
    }

    g->on_ground = collides(g, g->type, g->rot, g->x, g->y + 1);
    if (g->on_ground) {
        g->lock_acc += dt;
        if (g->lock_acc >= LOCK_DELAY_US) {
            lock_piece(g);
        }
    } else {
        g->lock_acc = 0;
    }
}

static void game_update(game_t *g, long dt)
{
    if (g->state == STATE_PLAYING || g->state == STATE_CLEARING) {
        g->elapsed_us += dt;
        g->event_us += dt;
    }

    switch (g->state) {
    case STATE_PLAYING:
        update_playing(g, dt);
        break;
    case STATE_CLEARING:
        g->clear_anim_us += dt;
        if (g->clear_anim_us >= LINE_CLEAR_ANIM_US) {
            collapse_rows(g);
            g->state = STATE_PLAYING;
            spawn_next(g);
        }
        break;
    default:
        break;
    }

    if (g->state == STATE_PLAYING && mode_finished(g)) {
        g->finished = 1;
        g->state = STATE_GAMEOVER;
    }
}

/* ------------------------------------------------------------------ *
 * Drawing the board
 * ------------------------------------------------------------------ */

static int L_x0, L_y0;          /* top left of the whole layout */
static int L_field_x, L_field_y;
static int L_hold_x, L_next_x;
static int g_help_visible = 1;

#define PANEL_W 12
#define FIELD_W (BOARD_W * 2)

static void layout(void)
{
    int total = PANEL_W + FIELD_W + 2 + PANEL_W;

    L_x0 = (g_scr_w - total) / 2;
    if (L_x0 < 0) {
        L_x0 = 0;
    }
    L_y0 = (g_scr_h - (VISIBLE_H + 4)) / 2;
    if (L_y0 < 0) {
        L_y0 = 0;
    }
    L_hold_x = L_x0;
    L_field_x = L_x0 + PANEL_W + 1;
    L_field_y = L_y0 + 1;
    L_next_x = L_field_x + FIELD_W + 2;
}

/* kind: 0 = empty, 1 = filled, 2 = ghost */
static void draw_cell(int sx, int sy, int kind, int color)
{
    if (g_cfg.style == STYLE_CLASSIC) {
        switch (kind) {
        case 1: scr_put2(sx, sy, "[", "]", color, C_DEFAULT, 1); break;
        case 2: scr_put2(sx, sy, ":", ":", C_GHOST, C_DEFAULT, 0); break;
        default: scr_put2(sx, sy, " ", ".", C_DIM, C_DEFAULT, 0); break;
        }
        return;
    }
    switch (kind) {
    case 1: scr_put2(sx, sy, "█", "█", color, C_DEFAULT, 0); break;
    case 2: scr_put2(sx, sy, "▓", "▓", C_GHOST, C_DEFAULT, 0); break;
    default: scr_put2(sx, sy, " ", " ", C_DIM, C_DEFAULT, 0); break;
    }
}

static void draw_border(const game_t *g)
{
    int i;
    int bottom = L_field_y + VISIBLE_H;

    (void)g;
    if (g_cfg.style == STYLE_CLASSIC) {
        for (i = 0; i < VISIBLE_H; i++) {
            scr_put2(L_field_x - 2, L_field_y + i, "<", "|", C_BORDER, C_DEFAULT, 1);
            scr_put2(L_field_x + FIELD_W, L_field_y + i, "|", ">", C_BORDER, C_DEFAULT, 1);
        }
        for (i = 0; i < BOARD_W; i++) {
            scr_put2(L_field_x + i * 2, bottom, "=", "=", C_BORDER, C_DEFAULT, 1);
            scr_put2(L_field_x + i * 2, bottom + 1, "\\", "/", C_BORDER, C_DEFAULT, 1);
        }
        return;
    }
    for (i = 0; i < VISIBLE_H; i++) {
        scr_put(L_field_x - 1, L_field_y + i, "│", C_BORDER, C_DEFAULT, 0);
        scr_put(L_field_x + FIELD_W, L_field_y + i, "│", C_BORDER, C_DEFAULT, 0);
    }
    scr_put(L_field_x - 1, bottom, "└", C_BORDER, C_DEFAULT, 0);
    scr_put(L_field_x + FIELD_W, bottom, "┘", C_BORDER, C_DEFAULT, 0);
    for (i = 0; i < FIELD_W; i++) {
        scr_put(L_field_x + i, bottom, "─", C_BORDER, C_DEFAULT, 0);
    }
}

static void draw_field(const game_t *g)
{
    int y, x, i;
    int flash = 0;

    if (g->state == STATE_CLEARING) {
        /* two quick blinks over the animation */
        flash = ((g->clear_anim_us * 4) / LINE_CLEAR_ANIM_US) % 2 == 0;
    }

    for (y = 0; y < VISIBLE_H; y++) {
        int row = TOP_ROW + y;
        int clearing = 0;

        for (i = 0; i < g->clear_count; i++) {
            if (g->clear_rows[i] == row) {
                clearing = 1;
            }
        }
        for (x = 0; x < BOARD_W; x++) {
            int sx = L_field_x + x * 2;
            int sy = L_field_y + y;
            uint8_t v = g->board[row][x];

            if (clearing) {
                draw_cell(sx, sy, flash ? 1 : 0, C_TEXT);
            } else if (v) {
                draw_cell(sx, sy, 1, PIECE_COLOR[v - 1]);
            } else {
                draw_cell(sx, sy, 0, C_DEFAULT);
            }
        }
    }

    if (g->type < 0 || g->state == STATE_CLEARING) {
        return;
    }

    if (g_cfg.ghost) {
        int gy = ghost_y(g);
        int cells[4][2];

        if (gy != g->y) {
            piece_cells(g->type, g->rot, g->x, gy, cells);
            for (i = 0; i < 4; i++) {
                int row = cells[i][1] - TOP_ROW;

                if (row >= 0 && row < VISIBLE_H) {
                    draw_cell(L_field_x + cells[i][0] * 2, L_field_y + row, 2, C_GHOST);
                }
            }
        }
    }

    {
        int cells[4][2];

        piece_cells(g->type, g->rot, g->x, g->y, cells);
        for (i = 0; i < 4; i++) {
            int row = cells[i][1] - TOP_ROW;

            if (row >= 0 && row < VISIBLE_H) {
                draw_cell(L_field_x + cells[i][0] * 2, L_field_y + row, 1,
                          PIECE_COLOR[g->type]);
            }
        }
    }
}

/* Draw a piece into a small preview box, centred on four columns. */
static void draw_mini(int type, int sx, int sy, int dim)
{
    int i;
    int minx = 4, maxx = -1, miny = 4, maxy = -1;
    int ox, oy;

    for (i = 0; i < 4; i++) {
        int cx = SHAPES[type][ROT_0][i][0];
        int cy = SHAPES[type][ROT_0][i][1];

        if (cx < minx) minx = cx;
        if (cx > maxx) maxx = cx;
        if (cy < miny) miny = cy;
        if (cy > maxy) maxy = cy;
    }
    ox = sx + (4 - (maxx - minx + 1)) ;
    oy = sy - miny;

    for (i = 0; i < 4; i++) {
        int cx = SHAPES[type][ROT_0][i][0] - minx;
        int cy = SHAPES[type][ROT_0][i][1];

        draw_cell(ox + cx * 2, oy + cy, 1, dim ? C_GHOST : PIECE_COLOR[type]);
    }
}

/* ------------------------------------------------------------------ *
 * Scoreboard
 *
 * Every finished run is appended to a plain text file next to the binary, so
 * it can be read, edited or thrown away without the game's help. Sprint is a
 * race and ranks on time; every other mode ranks on score.
 * ------------------------------------------------------------------ */

static const char *MODE_NAME[MODE_COUNT];

/* Directory the binary sits in. The config file and the scoreboard live
 * there, so the whole project stays in one folder instead of scattering into
 * the home directory. */
static char g_base_dir[1024] = ".";

static void locate_base_dir(const char *argv0)
{
    char *resolved;
    char *slash;

    if (argv0 && strchr(argv0, '/')) {
        resolved = realpath(argv0, NULL);
        if (resolved) {
            slash = strrchr(resolved, '/');
            if (slash && slash != resolved) {
                *slash = '\0';
                snprintf(g_base_dir, sizeof(g_base_dir), "%s", resolved);
                free(resolved);
                return;
            }
            free(resolved);
        }
    }
    /* started through PATH: fall back to where we were launched from */
    if (!getcwd(g_base_dir, sizeof(g_base_dir))) {
        snprintf(g_base_dir, sizeof(g_base_dir), ".");
    }
}

typedef struct {
    int mode;
    long score;
    int lines;
    long time_us;
    int finished;
    char date[12];
    char name[NAME_MAX + 1];
} score_entry;

static score_entry g_board[BOARD_MAX];
static int g_board_count;
static char g_player[NAME_MAX + 1];

static void board_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/scoreboard", g_base_dir);
}

static void board_load(void)
{
    char path[1100];
    char line[256];
    FILE *f;

    g_board_count = 0;
    board_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f) && g_board_count < BOARD_MAX) {
        char mode[16], date[12], name[NAME_MAX + 1];
        long score, us;
        int cleared, finished, i;
        score_entry *e;

        if (strncmp(line, "player ", 7) == 0) {
            char *nl;

            snprintf(g_player, sizeof(g_player), "%s", line + 7);
            nl = strchr(g_player, '\n');
            if (nl) {
                *nl = '\0';
            }
            continue;
        }
        if (sscanf(line, "%15s %ld %d %ld %d %11s %16[^\n]",
                   mode, &score, &cleared, &us, &finished, date, name) != 7) {
            continue;
        }
        for (i = 0; i < MODE_COUNT; i++) {
            if (strcmp(mode, MODE_NAME[i]) == 0) {
                break;
            }
        }
        if (i == MODE_COUNT) {
            continue;
        }
        e = &g_board[g_board_count++];
        e->mode = i;
        e->score = score;
        e->lines = cleared;
        e->time_us = us;
        e->finished = finished;
        snprintf(e->date, sizeof(e->date), "%s", date);
        snprintf(e->name, sizeof(e->name), "%s", name);
    }
    fclose(f);
}

static void board_save(void)
{
    char path[1100];
    FILE *f;
    int i;

    board_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "player %s\n", g_player[0] ? g_player : "anon");
    for (i = 0; i < g_board_count; i++) {
        const score_entry *e = &g_board[i];

        fprintf(f, "%s %ld %d %ld %d %s %s\n", MODE_NAME[e->mode], e->score,
                e->lines, e->time_us, e->finished, e->date, e->name);
    }
    fclose(f);
}

static int ranks_above(const score_entry *a, const score_entry *b)
{
    if (a->mode == MODE_SPRINT) {
        /* a race: finishing beats not finishing, then the quicker time */
        if (a->finished != b->finished) {
            return a->finished;
        }
        if (a->finished) {
            return a->time_us < b->time_us;
        }
    }
    if (a->score != b->score) {
        return a->score > b->score;
    }
    return a->lines > b->lines;
}

/* Fill `out` with the indices of one mode's entries, best first. */
static int board_top(int mode, int *out, int max)
{
    int idx[BOARD_MAX];
    int n = 0, i, j, t;

    for (i = 0; i < g_board_count; i++) {
        if (g_board[i].mode == mode) {
            idx[n++] = i;
        }
    }
    for (i = 1; i < n; i++) {           /* insertion sort: n stays small */
        t = idx[i];
        for (j = i; j > 0 && ranks_above(&g_board[t], &g_board[idx[j - 1]]); j--) {
            idx[j] = idx[j - 1];
        }
        idx[j] = t;
    }
    if (n > max) {
        n = max;
    }
    for (i = 0; i < n; i++) {
        out[i] = idx[i];
    }
    return n;
}

/* Keep the file from growing without end: drop a mode's weakest entries. */
static void board_trim(int mode)
{
    int idx[BOARD_MAX];
    int drop[BOARD_MAX];
    int n, i, j, d, k = 0;

    n = board_top(mode, idx, BOARD_MAX);
    if (n <= BOARD_PER_MODE) {
        return;
    }
    for (i = BOARD_PER_MODE; i < n; i++) {
        drop[k++] = idx[i];
    }
    for (i = 0, j = 0; i < g_board_count; i++) {
        int dropped = 0;

        for (d = 0; d < k; d++) {
            if (drop[d] == i) {
                dropped = 1;
                break;
            }
        }
        if (!dropped) {
            g_board[j++] = g_board[i];
        }
    }
    g_board_count = j;
}

/* Records a run and returns the place it took, or 0 if it missed the board. */
static int board_add(const game_t *g)
{
    score_entry *e;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int idx[BOARD_SHOWN];
    int mine, n, i, rank = 0;

    if (g_board_count >= BOARD_MAX) {
        board_trim(g->mode);
    }
    if (g_board_count >= BOARD_MAX) {
        return 0;
    }
    e = &g_board[g_board_count++];
    memset(e, 0, sizeof(*e));
    e->mode = g->mode;
    e->score = g->score;
    e->lines = g->lines;
    e->time_us = g->elapsed_us;
    e->finished = g->finished;
    if (tm) {
        strftime(e->date, sizeof(e->date), "%Y-%m-%d", tm);
    } else {
        snprintf(e->date, sizeof(e->date), "?");
    }
    snprintf(e->name, sizeof(e->name), "%s",
             g->player[0] ? g->player : "anon");

    mine = g_board_count - 1;
    n = board_top(g->mode, idx, BOARD_SHOWN);
    for (i = 0; i < n; i++) {
        if (idx[i] == mine) {
            rank = i + 1;
            break;
        }
    }
    board_trim(g->mode);
    board_save();
    return rank;
}

/* ------------------------------------------------------------------ *
 * Panels and overlays
 * ------------------------------------------------------------------ */

static const char *MODE_NAME[MODE_COUNT] = {"MARATHON", "SPRINT", "ULTRA", "ZEN"};
static const char *MODE_DESC[MODE_COUNT] = {
    "150 lines, rising level",
    "40 lines against the clock",
    "two minutes, highest score",
    "endless, no goal",
};

static void fmt_time(char *buf, size_t n, long us)
{
    long total_ms = us / 1000;

    snprintf(buf, n, "%ld:%02ld.%03ld", total_ms / 60000,
             (total_ms / 1000) % 60, total_ms % 1000);
}

static void draw_panels(const game_t *g)
{
    char buf[64];
    int y = L_field_y;
    int i;
    double secs = (double)g->elapsed_us / 1000000.0;

    scr_text(L_hold_x + 1, y - 1, C_DIM, 1, "HOLD");
    if (g->hold >= 0) {
        draw_mini(g->hold, L_hold_x + 1, y + 1, g->hold_used);
    }

    scr_text(L_next_x + 1, y - 1, C_DIM, 1, "NEXT");
    for (i = 0; i < NEXT_COUNT && i < g->queue_len; i++) {
        draw_mini(g->queue[i], L_next_x + 1, y + 1 + i * 3, 0);
    }

    y = L_field_y + 5;
    scr_text(L_hold_x, y++, C_DIM, 0, "SCORE");
    scr_text(L_hold_x, y++, C_TEXT, 1, "%ld", g->score);
    y++;
    scr_text(L_hold_x, y++, C_DIM, 0, "LINES");
    if (g->mode == MODE_SPRINT) {
        scr_text(L_hold_x, y++, C_TEXT, 1, "%d / %d", g->lines, SPRINT_GOAL);
    } else if (g->mode == MODE_MARATHON) {
        scr_text(L_hold_x, y++, C_TEXT, 1, "%d / %d", g->lines, MARATHON_GOAL);
    } else {
        scr_text(L_hold_x, y++, C_TEXT, 1, "%d", g->lines);
    }
    y++;
    scr_text(L_hold_x, y++, C_DIM, 0, "LEVEL");
    scr_text(L_hold_x, y++, C_TEXT, 1, "%d", g->level);
    y++;
    scr_text(L_hold_x, y++, C_DIM, 0, "TIME");
    if (g->mode == MODE_ULTRA) {
        long left = ULTRA_TIME_US - g->elapsed_us;

        fmt_time(buf, sizeof(buf), left > 0 ? left : 0);
    } else {
        fmt_time(buf, sizeof(buf), g->elapsed_us);
    }
    scr_text(L_hold_x, y++, C_TEXT, 1, "%s", buf);
    y++;
    scr_text(L_hold_x, y++, C_DIM, 0, "PPS");
    scr_text(L_hold_x, y++, C_TEXT, 1, "%.2f", secs > 0.5 ? g->pieces / secs : 0.0);

    y = L_next_x ? L_field_y + NEXT_COUNT * 3 + 2 : 0;
    if (g->combo > 1) {
        scr_text(L_next_x, y++, C_ACCENT, 1, "COMBO %d", g->combo - 1);
    }
    if (g->b2b > 1) {
        scr_text(L_next_x, y++, C_WARN, 1, "B2B %d", g->b2b - 1);
    }

    /* Last scoring event, fading out after a moment. */
    if (g->event[0] && g->event_us < 2000000L) {
        scr_text(L_field_x, L_field_y + VISIBLE_H + 2, C_ACCENT, 1, "%s", g->event);
    }

    scr_text(L_field_x, L_y0 - 1, C_DIM, 1, "%s", MODE_NAME[g->mode]);

    if (g_help_visible && L_next_x + PANEL_W + 22 < g_scr_w) {
        int hx = L_next_x + PANEL_W;
        int hy = L_field_y;

        scr_text(hx, hy++, C_DIM, 1, "CONTROLS");
        hy++;
        scr_text(hx, hy++, C_DIM, 0, "left/right  move");
        scr_text(hx, hy++, C_DIM, 0, "down        soft drop");
        scr_text(hx, hy++, C_DIM, 0, "space       hard drop");
        scr_text(hx, hy++, C_DIM, 0, "up / x      rotate cw");
        scr_text(hx, hy++, C_DIM, 0, "z / ctrl    rotate ccw");
        scr_text(hx, hy++, C_DIM, 0, "a           rotate 180");
        scr_text(hx, hy++, C_DIM, 0, "c / shift   hold");
        scr_text(hx, hy++, C_DIM, 0, "v           style");
        scr_text(hx, hy++, C_DIM, 0, "h           this help");
        scr_text(hx, hy++, C_DIM, 0, "esc         pause");
        {
            const char *label = "r           restart";
            int width = (int)strlen(label);
            int filled = (int)(g_restart_hold * width / RESTART_HOLD_US);

            if (filled > width) {
                filled = width;
            }
            scr_text_bar(hx, hy++, C_DIM, 0, filled, C_WARN, label);
        }
        scr_text(hx, hy++, C_DIM, 0, "q           quit");
        hy++;
        scr_text(hx, hy++, C_DIM, 0, "DAS %ldms", g_cfg.das_us / 1000);
        scr_text(hx, hy++, C_DIM, 0, "ARR %ldms", g_cfg.arr_us / 1000);
        scr_text(hx, hy++, C_DIM, 0, "SOFT %d/s", g_cfg.sdf);
        if (!g_kitty_active) {
            scr_text(hx, hy++, C_WARN, 0, "no key-release");
            scr_text(hx, hy++, C_WARN, 0, "events: DAS is");
            scr_text(hx, hy++, C_WARN, 0, "approximated");
        }
    }
}

static void draw_box(int x, int y, int w, int h, int color)
{
    int i;

    for (i = 1; i < w - 1; i++) {
        scr_put(x + i, y, "─", color, C_DEFAULT, 0);
        scr_put(x + i, y + h - 1, "─", color, C_DEFAULT, 0);
    }
    for (i = 1; i < h - 1; i++) {
        int j;

        scr_put(x, y + i, "│", color, C_DEFAULT, 0);
        scr_put(x + w - 1, y + i, "│", color, C_DEFAULT, 0);
        for (j = 1; j < w - 1; j++) {
            scr_put(x + j, y + i, " ", C_DEFAULT, C_DEFAULT, 0);
        }
    }
    scr_put(x, y, "┌", color, C_DEFAULT, 0);
    scr_put(x + w - 1, y, "┐", color, C_DEFAULT, 0);
    scr_put(x, y + h - 1, "└", color, C_DEFAULT, 0);
    scr_put(x + w - 1, y + h - 1, "┘", color, C_DEFAULT, 0);
}

static void draw_centered(int y, int color, int bold, const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    scr_text((g_scr_w - (int)strlen(buf)) / 2, y, color, bold, "%s", buf);
}

static int g_last_rank;         /* place the finished run took */

static void draw_gameover(const game_t *g)
{
    int w = 40, h = 11;
    int x = (g_scr_w - w) / 2;
    int y = (g_scr_h - h) / 2;
    char buf[64];

    draw_box(x, y, w, h, g->finished ? C_GOOD : C_WARN);
    draw_centered(y + 1, g->finished ? C_GOOD : C_WARN, 1,
                  g->finished ? "%s COMPLETE" : "GAME OVER", MODE_NAME[g->mode]);
    fmt_time(buf, sizeof(buf), g->elapsed_us);
    draw_centered(y + 3, C_TEXT, 0, "score  %ld", g->score);
    draw_centered(y + 4, C_TEXT, 0, "lines  %d", g->lines);
    draw_centered(y + 5, C_TEXT, 0, "time   %s", buf);
    draw_centered(y + 6, C_TEXT, 0, "pieces %d", g->pieces);
    if (g_last_rank == 1) {
        draw_centered(y + 7, C_WARN, 1, "top of the scoreboard, %s", g->player);
    } else if (g_last_rank > 0) {
        draw_centered(y + 7, C_GOOD, 0, "number %d on the scoreboard", g_last_rank);
    }
    {
        const char *hint = "r restart    esc menu    q quit";
        int hx = (g_scr_w - (int)strlen(hint)) / 2;
        int seg = 9;            /* the "r restart" part carries the bar */
        int filled = (int)(g_restart_hold * seg / RESTART_HOLD_US);

        if (filled > seg) {
            filled = seg;
        }
        scr_text_bar(hx, y + 9, C_ACCENT, 1, filled, C_WARN, hint);
    }
}

static void draw_pause(void)
{
    int w = 30, h = 5;
    int x = (g_scr_w - w) / 2;
    int y = (g_scr_h - h) / 2;

    draw_box(x, y, w, h, C_ACCENT);
    draw_centered(y + 1, C_ACCENT, 1, "PAUSED");
    draw_centered(y + 3, C_DIM, 0, "esc resume   q quit");
}

static int g_menu_sel = MENU_MARATHON;
static int g_name_editing;
static int g_board_mode;

static void draw_menu(void)
{
    int w = 54, h = 20;
    int x = (g_scr_w - w) / 2;
    int y = (g_scr_h - h) / 2;
    int len = (int)strlen(g_player);
    int i;

    draw_box(x, y, w, h, C_ACCENT);
    draw_centered(y + 1, C_ACCENT, 1, "ULTIMATE TERMINAL TETRIS");
    draw_centered(y + 2, C_TEXT, 0, "by Alextout");

    /* the name that goes on the scoreboard */
    {
        int sel = (g_menu_sel == MENU_NAME);
        int row = y + 4;

        scr_text(x + 4, row, sel ? C_ACCENT : C_TEXT, sel,
                 "%s %-9s", sel ? ">" : " ", "NAME");
        scr_put(x + 17, row, "[", C_DIM, C_DEFAULT, 0);
        for (i = 0; i < NAME_MAX; i++) {
            char ch[2] = {' ', 0};
            int cursor = (g_name_editing && i == len);

            if (i < len) {
                ch[0] = g_player[i];
            }
            scr_put(x + 18 + i, row, ch, C_TEXT,
                    cursor ? C_ACCENT : C_DEFAULT, 0);
        }
        scr_put(x + 18 + NAME_MAX, row, "]", C_DIM, C_DEFAULT, 0);
        if (g_name_editing) {
            scr_text(x + 20 + NAME_MAX, row, C_WARN, 1, "typing");
        } else if (!len) {
            scr_text(x + 20 + NAME_MAX, row, C_DIM, 0, "unset");
        }
    }

    for (i = MENU_MARATHON; i <= MENU_ZEN; i++) {
        int sel = (i == g_menu_sel);
        int row = y + 6 + (i - MENU_MARATHON);
        int mode = i - MENU_MARATHON;

        scr_text(x + 4, row, sel ? C_ACCENT : C_TEXT, sel,
                 "%s %-9s", sel ? ">" : " ", MODE_NAME[mode]);
        scr_text(x + 17, row, C_DIM, 0, "%s", MODE_DESC[mode]);
    }
    {
        int sel = (g_menu_sel == MENU_BOARD);

        scr_text(x + 4, y + 11, sel ? C_ACCENT : C_TEXT, sel,
                 "%s %-9s", sel ? ">" : " ", "SCOREBOARD");
        scr_text(x + 17, y + 11, C_DIM, 0, "who got how far");
    }

    draw_centered(y + 13, C_DIM, 0, "guideline rules, tetrio handling");
    draw_centered(y + 15, C_DIM, 0, "after the DVK tetris by Kirill Timofeev");
    draw_centered(y + 16, C_DIM, 0, "github.com/kt97679/tetris");
    draw_centered(y + h - 2, C_DIM, 0, g_name_editing
                  ? "type a name   backspace deletes   enter done"
                  : "up/down choose   enter select   q quit");
}

static void draw_scoreboard(void)
{
    int w = 60, h = 20;
    int x = (g_scr_w - w) / 2;
    int y = (g_scr_h - h) / 2;
    int idx[BOARD_SHOWN];
    char buf[32];
    int n, i;

    draw_box(x, y, w, h, C_ACCENT);
    draw_centered(y + 1, C_ACCENT, 1, "SCOREBOARD");
    draw_centered(y + 2, C_TEXT, 1, "< %s >", MODE_NAME[g_board_mode]);

    scr_text(x + 3, y + 4, C_DIM, 0, "#");
    scr_text(x + 6, y + 4, C_DIM, 0, "NAME");
    scr_text(x + 23, y + 4, C_DIM, 0, "%8s", "SCORE");
    scr_text(x + 32, y + 4, C_DIM, 0, "LINES");
    scr_text(x + 38, y + 4, C_DIM, 0, "TIME");
    scr_text(x + 48, y + 4, C_DIM, 0, "DATE");

    n = board_top(g_board_mode, idx, BOARD_SHOWN);
    for (i = 0; i < n; i++) {
        const score_entry *e = &g_board[idx[i]];
        int row = y + 5 + i;
        int color = (i == 0) ? C_WARN : C_TEXT;

        scr_text(x + 3, row, color, i == 0, "%d", i + 1);
        scr_text(x + 6, row, color, i == 0, "%-16s", e->name);
        scr_text(x + 23, row, color, 0, "%8ld", e->score);
        scr_text(x + 32, row, color, 0, "%5d", e->lines);
        fmt_time(buf, sizeof(buf), e->time_us);
        scr_text(x + 38, row, color, 0, "%-9s", buf);
        scr_text(x + 48, row, C_DIM, 0, "%s", e->date);
    }
    if (!n) {
        draw_centered(y + 9, C_DIM, 0, "no runs in this mode yet");
    }
    draw_centered(y + h - 2, C_DIM, 0, "left/right mode   esc back");
}

static void draw_too_small(void)
{
    scr_clear();
    scr_text(0, 0, C_WARN, 1, "Terminal too small");
    scr_text(0, 1, C_DIM, 0, "need %dx%d, have %dx%d",
             MIN_SCREEN_W, MIN_SCREEN_H, g_scr_w, g_scr_h);
}

/* ------------------------------------------------------------------ *
 * Configuration file and command line
 * ------------------------------------------------------------------ */

static int key_from_name(const char *s)
{
    static const struct { const char *name; int key; } NAMES[] = {
        {"left", K_LEFT}, {"right", K_RIGHT}, {"up", K_UP}, {"down", K_DOWN},
        {"escape", K_ESC}, {"esc", K_ESC}, {"enter", K_ENTER},
        {"space", K_SPACE}, {"tab", K_TAB},
        {"lshift", K_LSHIFT}, {"rshift", K_RSHIFT}, {"shift", K_LSHIFT},
        {"lctrl", K_LCTRL}, {"rctrl", K_RCTRL}, {"ctrl", K_LCTRL},
        {"backspace", K_BACKSPACE},
    };
    size_t i;

    for (i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (strcasecmp(s, NAMES[i].name) == 0) {
            return NAMES[i].key;
        }
    }
    if (strlen(s) == 1) {
        return tolower((unsigned char)s[0]);
    }
    return 0;
}

static int action_from_name(const char *s)
{
    static const char *NAMES[ACT_COUNT] = {
        "left", "right", "soft_drop", "hard_drop", "rotate_cw", "rotate_ccw",
        "rotate_180", "hold", "pause", "restart", "quit", "style", "help",
    };
    int i;

    for (i = 0; i < ACT_COUNT; i++) {
        if (strcasecmp(s, NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void bind_keys(int act, const char *value)
{
    char tmp[128];
    char *tok;
    int n = 0;

    snprintf(tmp, sizeof(tmp), "%s", value);
    memset(g_cfg.binds[act], 0, sizeof(g_cfg.binds[act]));
    for (tok = strtok(tmp, ","); tok && n < MAX_BINDS; tok = strtok(NULL, ",")) {
        while (*tok == ' ') {
            tok++;
        }
        g_cfg.binds[act][n] = key_from_name(tok);
        if (g_cfg.binds[act][n]) {
            n++;
        }
    }
}

static int mode_from_name(const char *s)
{
    int i;

    for (i = 0; i < MODE_COUNT; i++) {
        if (strcasecmp(s, MODE_NAME[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* Returns 0 for a key it does not know, so the command line can complain. */
static int config_set(const char *key, const char *value)
{
    int act;

    if (strcasecmp(key, "das") == 0) {
        g_cfg.das_us = atol(value) * 1000;
    } else if (strcasecmp(key, "arr") == 0) {
        g_cfg.arr_us = atol(value) * 1000;
    } else if (strcasecmp(key, "sdf") == 0) {
        g_cfg.sdf = atoi(value);
    } else if (strcasecmp(key, "ghost") == 0) {
        g_cfg.ghost = atoi(value) != 0;
    } else if (strcasecmp(key, "colors") == 0) {
        g_cfg.color_depth = atoi(value);
    } else if (strcasecmp(key, "style") == 0) {
        g_cfg.style = (strcasecmp(value, "classic") == 0) ? STYLE_CLASSIC : STYLE_SOLID;
    } else if (strcasecmp(key, "mode") == 0) {
        int m = mode_from_name(value);

        if (m >= 0) {
            g_cfg.mode = m;
        }
    } else if (strncasecmp(key, "key_", 4) == 0) {
        act = action_from_name(key + 4);
        if (act < 0) {
            return 0;
        }
        bind_keys(act, value);
    } else {
        return 0;
    }
    return 1;
}

static void config_load(void)
{
    char path[1100];
    char line[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/config", g_base_dir);
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char key[64], value[128];
        char *eq, *p;

        p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\0') {
            continue;
        }
        eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        snprintf(key, sizeof(key), "%s", p);
        snprintf(value, sizeof(value), "%s", eq + 1);
        for (p = key + strlen(key); p > key && (p[-1] == ' ' || p[-1] == '\t'); p--) {
            p[-1] = '\0';
        }
        p = value;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        memmove(value, p, strlen(p) + 1);
        for (p = value + strlen(value); p > value &&
             (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '); p--) {
            p[-1] = '\0';
        }
        config_set(key, value);
    }
    fclose(f);
}

static void usage(void)
{
    printf("ultimate-terminal-tetris, by Alextout\n");
    printf("after the DVK tetris by Kirill Timofeev, github.com/kt97679/tetris\n\n");
    printf("usage: tetris [options]\n\n");
    printf("  --mode=marathon|sprint|ultra|zen   game mode (default marathon)\n");
    printf("  --das=MS        delay before the auto shift starts, ms (default 266,\n");
    printf("                  the NES 16 frames)\n");
    printf("  --arr=MS        auto shift rate in ms, 0 = slide to the wall\n");
    printf("                  (default 100, the NES 6 frames)\n");
    printf("  --sdf=N         soft drop speed in rows/s, 0 = instant (default 30,\n");
    printf("                  the NES rate of one row every two frames)\n");
    printf("  --style=solid|classic\n");
    printf("  --ghost=0|1     landing preview (default 1)\n");
    printf("  --colors=0|8|256|24   force colour depth (default: autodetect)\n");
    printf("  --seed=N        fixed randomiser seed\n");
    printf("  --selftest      run the internal test suite and exit\n");
    printf("  --help          this text\n\n");
    printf("The config file and the scoreboard sit next to the binary, in\n");
    printf("config and scoreboard, so the whole project stays in one folder.\n");
    printf("The config takes one 'key = value' per line, the same names as\n");
    printf("the options above plus key_left, key_hard_drop, ... for bindings.\n");
    printf("The scoreboard is plain text: mode, score, lines, time in\n");
    printf("microseconds, whether the run finished, the date and the name.\n");
}

/* ------------------------------------------------------------------ *
 * Self test
 *
 * Runs the game logic headless, without a terminal, so the rotation tables,
 * the randomiser and the scoring can be checked in CI. The rendering is
 * covered separately by the pty harness.
 * ------------------------------------------------------------------ */

static int g_test_fail;

static void check(int ok, const char *what)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        g_test_fail++;
    }
}

static int test_shapes(void)
{
    int t, r, i, j;

    for (t = 0; t < PIECE_COUNT; t++) {
        for (r = 0; r < 4; r++) {
            for (i = 0; i < 4; i++) {
                int x = SHAPES[t][r][i][0];
                int y = SHAPES[t][r][i][1];

                if (x < 0 || x > 3 || y < 0 || y > 3) {
                    return 0;
                }
                for (j = i + 1; j < 4; j++) {
                    if (SHAPES[t][r][j][0] == x && SHAPES[t][r][j][1] == y) {
                        return 0;   /* duplicate cell */
                    }
                }
            }
        }
    }
    return 1;
}

static int test_rotation_identity(void)
{
    game_t g;
    int t, i;

    for (t = 0; t < PIECE_COUNT; t++) {
        if (t == PIECE_O) {
            continue;
        }
        memset(&g, 0, sizeof(g));
        g.hold = -1;
        spawn_piece(&g, t);
        {
            int x0 = g.x, y0 = g.y, r0 = g.rot;

            for (i = 0; i < 4; i++) {
                if (!try_rotate(&g, 1)) {
                    return 0;
                }
            }
            if (g.x != x0 || g.y != y0 || g.rot != r0) {
                return 0;
            }
            if (!try_rotate(&g, 1) || !try_rotate(&g, -1)) {
                return 0;
            }
            if (g.x != x0 || g.y != y0 || g.rot != r0) {
                return 0;
            }
        }
    }
    return 1;
}

static int test_bag(void)
{
    game_t g;
    int counts[PIECE_COUNT] = {0};
    int i, j;

    memset(&g, 0, sizeof(g));
    rng_seed(12345);
    bag_refill(&g);
    for (i = 0; i < 100; i++) {
        int seen[PIECE_COUNT] = {0};

        for (j = 0; j < PIECE_COUNT; j++) {
            int p = bag_take(&g);

            if (p < 0 || p >= PIECE_COUNT || seen[p]) {
                return 0;       /* a bag must hold each piece exactly once */
            }
            seen[p] = 1;
            counts[p]++;
        }
    }
    for (i = 0; i < PIECE_COUNT; i++) {
        if (counts[i] != 100) {
            return 0;
        }
    }
    return 1;
}

/* A T rotating clockwise into a slot it can only reach through the third
 * kick offset. Checks the kick table and the three-corner rule together. */
static int test_tspin(void)
{
    game_t g;

    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.board[37][3] = 1;
    g.board[37][5] = 1;
    g.board[35][5] = 1;
    g.board[35][2] = 1;

    g.type = PIECE_T;
    g.rot = ROT_R;
    g.x = 2;
    g.y = 34;
    if (collides(&g, g.type, g.rot, g.x, g.y)) {
        return 0;               /* the start position must be legal */
    }
    if (!try_rotate(&g, 1)) {
        return 0;
    }
    if (g.rot != ROT_2 || g.x != 3 || g.y != 35 || g.last_kick != 2) {
        return 0;               /* must have landed via the third kick */
    }
    return detect_spin(&g) == SPIN_FULL;
}

static int test_line_clear(void)
{
    game_t g;
    int x;

    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 1;
    g.state = STATE_PLAYING;
    for (x = 1; x < BOARD_W; x++) {
        g.board[BOARD_H - 1][x] = 1;
    }
    /* vertical I filling column 0 over the bottom four rows */
    g.type = PIECE_I;
    g.rot = ROT_R;
    g.x = -2;
    g.y = BOARD_H - 4;
    if (collides(&g, g.type, g.rot, g.x, g.y)) {
        return 0;
    }
    lock_piece(&g);
    if (g.lines != 1 || g.score != 100 || g.clear_count != 1) {
        return 0;
    }
    collapse_rows(&g);
    if (!g.board[BOARD_H - 1][0] || !g.board[BOARD_H - 2][0] ||
        !g.board[BOARD_H - 3][0] || g.board[BOARD_H - 4][0]) {
        return 0;               /* the stack must have dropped by one row */
    }
    for (x = 1; x < BOARD_W; x++) {
        if (g.board[BOARD_H - 1][x]) {
            return 0;           /* the completed row must be gone */
        }
    }
    return 1;
}

/* Locking into a completed row must run through the clear animation and come
 * back with the row gone and a fresh piece in play. */
static int test_clear_animation(void)
{
    game_t g;
    int x;

    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 1;
    g.state = STATE_PLAYING;
    rng_seed(99);
    bag_refill(&g);
    queue_fill(&g);
    for (x = 1; x < BOARD_W; x++) {
        g.board[BOARD_H - 1][x] = 1;
    }
    g.type = PIECE_I;
    g.rot = ROT_R;
    g.x = -2;
    g.y = BOARD_H - 4;
    lock_piece(&g);
    if (g.state != STATE_CLEARING || g.type >= 0) {
        return 0;               /* the animation must be running */
    }
    game_update(&g, LINE_CLEAR_ANIM_US + 1);
    if (g.state != STATE_PLAYING || g.type < 0) {
        return 0;               /* and hand back to a new piece afterwards */
    }
    for (x = 1; x < BOARD_W; x++) {
        if (g.board[BOARD_H - 1][x]) {
            return 0;
        }
    }
    return g.board[BOARD_H - 1][0] != 0;
}

/* Pressing soft drop must move exactly one row, however much gravity had
 * accumulated before the press. */
/* Holding a direction: one cell on the press, nothing until the delay runs
 * out, then a steady cell per repeat. */
static int test_das_arr(void)
{
    game_t g;
    long saved_das = g_cfg.das_us;
    long saved_arr = g_cfg.arr_us;
    int x0, i, ok = 1;

    g_cfg.das_us = 266000;      /* the NES 16 frames */
    g_cfg.arr_us = 100000;      /* and 6 frames between repeats */
    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 1;
    g.state = STATE_PLAYING;
    rng_seed(5);
    bag_refill(&g);
    queue_fill(&g);
    spawn_piece(&g, PIECE_T);
    keys_clear();
    g_shift_dir = 0;
    g_das_acc = 0;
    g_arr_acc = 0;
    g_das_charged = 0;
    x0 = g.x;

    key_press(K_LEFT, 1000);
    update_shift(&g, 16000);
    if (g.x != x0 - 1) {
        ok = 0;                 /* the press itself moves one cell */
    }
    for (i = 0; i < 15; i++) {  /* 240 ms, still inside the delay */
        update_shift(&g, 16000);
    }
    if (g.x != x0 - 1) {
        ok = 0;
    }
    update_shift(&g, 32000);    /* 272 ms: the delay expires and it moves */
    if (g.x != x0 - 2) {
        ok = 0;
    }
    for (i = 0; i < 5; i++) {   /* 80 ms is not yet a repeat */
        update_shift(&g, 16000);
    }
    if (g.x != x0 - 2) {
        ok = 0;
    }
    update_shift(&g, 32000);    /* past 100 ms: the next cell */
    if (g.x != x0 - 3) {
        ok = 0;
    }

    keys_clear();
    g_shift_dir = 0;
    g_das_acc = 0;
    g_arr_acc = 0;
    g_das_charged = 0;
    g_cfg.das_us = saved_das;
    g_cfg.arr_us = saved_arr;
    return ok;
}

/* On the NES the auto shift stays charged when a new piece arrives, which is
 * what lets a held direction carry the next piece straight to the wall. */
static int test_das_charge_survives_spawn(void)
{
    game_t g;
    long saved_das = g_cfg.das_us;
    long saved_arr = g_cfg.arr_us;
    int x0, i, ok = 1;

    g_cfg.das_us = 266000;
    g_cfg.arr_us = 100000;
    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 1;
    g.state = STATE_PLAYING;
    rng_seed(5);
    bag_refill(&g);
    queue_fill(&g);
    spawn_piece(&g, PIECE_T);
    keys_clear();
    g_shift_dir = 0;
    g_das_acc = 0;
    g_arr_acc = 0;
    g_das_charged = 0;

    key_press(K_LEFT, 1000);
    update_shift(&g, 16000);
    for (i = 0; i < 20; i++) {  /* hold past the delay */
        update_shift(&g, 16000);
    }
    if (!g_das_charged) {
        ok = 0;
    }

    spawn_piece(&g, PIECE_T);   /* a new piece, the key still held */
    x0 = g.x;
    update_shift(&g, 100000);
    if (g.x >= x0) {
        ok = 0;                 /* it must move without charging again */
    }

    keys_clear();
    g_shift_dir = 0;
    g_das_acc = 0;
    g_arr_acc = 0;
    g_das_charged = 0;
    g_cfg.das_us = saved_das;
    g_cfg.arr_us = saved_arr;
    return ok;
}

static int test_soft_drop_start(void)
{
    game_t g;
    int y0;

    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 1;
    g.state = STATE_PLAYING;
    rng_seed(5);
    bag_refill(&g);
    queue_fill(&g);
    spawn_piece(&g, PIECE_I);
    g.gravity_acc = 900000;     /* almost a full second queued up */
    y0 = g.y;

    keys_clear();
    g_soft_held = 0;
    key_press(K_DOWN, 1000);
    update_gravity(&g, 16000);
    if (g.y != y0 + 1) {
        return 0;               /* one row, not the whole backlog */
    }
    /* the next row has to wait a full soft drop interval */
    update_gravity(&g, 16000);
    if (g.y != y0 + 1) {
        return 0;
    }
    update_gravity(&g, 1000000L / g_cfg.sdf);
    if (g.y != y0 + 2) {
        return 0;
    }
    /* Auto repeat without the protocol re-presses the same key. That must not
     * count as a fresh press, or it would drive the speed instead of SDF. */
    g.gravity_acc = 0;
    key_press(K_DOWN, 2000);
    update_gravity(&g, 16000);
    keys_clear();
    g_soft_held = 0;
    return g.y == y0 + 2;
}

/* The soft drop runs at a fixed rate like the NES, and never slows the piece
 * down at levels where gravity is already faster. */
static int test_soft_drop_rate(void)
{
    game_t g;
    int saved = g_cfg.sdf;
    int y0, i, ok = 1;

    g_cfg.sdf = 30;             /* one row every two frames at 60 Hz */
    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 1;
    g.state = STATE_PLAYING;
    rng_seed(5);
    bag_refill(&g);
    queue_fill(&g);
    spawn_piece(&g, PIECE_I);
    keys_clear();
    g_soft_held = 0;
    key_press(K_DOWN, 1000);
    update_gravity(&g, 1000);   /* the press itself moves one row */
    y0 = g.y;
    for (i = 0; i < 8; i++) {
        update_gravity(&g, 1000000L / 30);
    }
    if (g.y != y0 + 8) {
        ok = 0;                 /* one row per 1/30 s */
    }

    /* at level 19 gravity is well under a millisecond per row */
    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 19;
    g.state = STATE_PLAYING;
    bag_refill(&g);
    queue_fill(&g);
    spawn_piece(&g, PIECE_I);
    y0 = g.y;
    g_soft_held = 1;            /* already held, no fresh press */
    update_gravity(&g, 1000000L / 30);
    if (g.y - y0 <= 8) {
        ok = 0;                 /* the soft drop must not cap gravity */
    }

    keys_clear();
    g_soft_held = 0;
    g_cfg.sdf = saved;
    return ok;
}

static int test_scoring(void)
{
    game_t g;

    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 1;
    g.board[0][0] = 1;          /* keep the board non-empty: no perfect clear */

    award(&g, 4, SPIN_NONE);
    if (g.score != 800 || g.b2b != 1 || g.combo != 1) {
        return 0;
    }
    /* second tetris: back-to-back is 1.5x, plus 50 for the combo */
    award(&g, 4, SPIN_NONE);
    if (g.score != 800 + 1250 || g.b2b != 2 || g.combo != 2) {
        return 0;
    }
    /* a single breaks back-to-back and keeps the combo running */
    award(&g, 1, SPIN_NONE);
    if (g.b2b != 0 || g.combo != 3) {
        return 0;
    }
    /* no clear resets the combo */
    award(&g, 0, SPIN_NONE);
    if (g.combo != 0) {
        return 0;
    }
    return 1;
}

static int test_perfect_clear(void)
{
    game_t g;
    int x;

    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.level = 1;
    for (x = 0; x < BOARD_W; x++) {
        g.board[BOARD_H - 1][x] = 1;
    }
    find_full_rows(&g);
    collapse_rows(&g);
    if (!board_empty(&g)) {
        return 0;
    }
    award(&g, 1, SPIN_NONE);
    /* single 100 plus the 800 perfect clear bonus */
    return g.score == 900;
}

static int test_gravity(void)
{
    if (gravity_us(1) < 990000 || gravity_us(1) > 1010000) {
        return 0;
    }
    if (!(gravity_us(20) < gravity_us(10) && gravity_us(10) < gravity_us(1))) {
        return 0;
    }
    return gravity_us(20) > 0;
}

static int test_lock_out(void)
{
    game_t g;
    int y, x;

    memset(&g, 0, sizeof(g));
    g.hold = -1;
    g.state = STATE_PLAYING;
    /* fill the visible field so a new piece cannot appear */
    for (y = TOP_ROW; y < BOARD_H; y++) {
        for (x = 0; x < BOARD_W; x++) {
            g.board[y][x] = 1;
        }
    }
    spawn_piece(&g, PIECE_T);
    return g.topped_out && g.state == STATE_GAMEOVER;
}

/* Each mode has to end on its own goal and Zen on none. */
static int test_modes(void)
{
    game_t g;

    memset(&g, 0, sizeof(g));
    g.mode = MODE_MARATHON;
    g.lines = MARATHON_GOAL - 1;
    if (mode_finished(&g)) {
        return 0;
    }
    g.lines = MARATHON_GOAL;
    if (!mode_finished(&g)) {
        return 0;
    }

    memset(&g, 0, sizeof(g));
    g.mode = MODE_SPRINT;
    g.lines = SPRINT_GOAL;
    if (!mode_finished(&g) || level_for(&g) != 1) {
        return 0;               /* sprint stays at level 1 */
    }

    memset(&g, 0, sizeof(g));
    g.mode = MODE_ULTRA;
    g.state = STATE_PLAYING;
    g.hold = -1;
    g.type = -1;
    g.elapsed_us = ULTRA_TIME_US - 1000;
    game_update(&g, 2000);
    if (g.state != STATE_GAMEOVER || !g.finished) {
        return 0;               /* the clock has to end the run */
    }

    memset(&g, 0, sizeof(g));
    g.mode = MODE_ZEN;
    g.lines = 10000;
    g.elapsed_us = ULTRA_TIME_US * 10;
    return !mode_finished(&g);
}

static int run_selftest(void)
{
    printf("ultimate-terminal-tetris self test\n\n");
    check(test_shapes(), "piece shapes are four distinct cells");
    check(test_rotation_identity(), "four rotations return to the start");
    check(test_bag(), "7-bag yields every piece once per bag");
    check(test_tspin(), "SRS kick into a T-slot is a full T-spin");
    check(test_line_clear(), "completed row clears and the stack drops");
    check(test_clear_animation(), "clear animation resolves into a new piece");
    check(test_das_arr(), "auto shift waits for DAS then repeats at ARR");
    check(test_das_charge_survives_spawn(), "auto shift stays charged across a piece");
    check(test_soft_drop_start(), "soft drop starts with a single row");
    check(test_soft_drop_rate(), "soft drop holds the NES rate at any level");
    check(test_scoring(), "scoring, back-to-back and combo");
    check(test_perfect_clear(), "perfect clear bonus");
    check(test_gravity(), "gravity curve falls with the level");
    check(test_lock_out(), "blocked spawn ends the game");
    check(test_modes(), "each mode ends on its own goal");
    printf("\n%s\n", g_test_fail ? "FAILURES" : "all tests passed");
    return g_test_fail ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 * Main loop
 * ------------------------------------------------------------------ */

static int g_in_menu = 1;
static int g_in_board;
static int g_submitted;

static void start_game(int mode)
{
    game_reset(&g_game, mode);
    snprintf(g_game.player, sizeof(g_game.player), "%s",
             g_player[0] ? g_player : "anon");
    g_last_rank = 0;
    g_in_menu = 0;
    g_submitted = 0;
    g_shift_dir = 0;
    g_das_acc = 0;
    g_arr_acc = 0;
    g_das_charged = 0;
    g_restart_hold = 0;
    keys_clear();
}

static void wait_for_frame(long frame_start)
{
    long remaining = FRAME_US - (now_us() - frame_start);
    struct timeval tv;
    fd_set fs;

    if (remaining <= 0) {
        return;
    }
    tv.tv_sec = 0;
    tv.tv_usec = (int)remaining;
    FD_ZERO(&fs);
    FD_SET(STDIN_FILENO, &fs);
    select(STDIN_FILENO + 1, &fs, NULL, NULL, &tv);
}

int main(int argc, char **argv)
{
    long last;
    unsigned long seed = 0;
    int mode_given = 0;
    int i;

    locate_base_dir(argv[0]);
    config_load();

    for (i = 1; i < argc; i++) {
        char *a = argv[i];

        if (strncmp(a, "--", 2) != 0) {
            continue;
        }
        a += 2;
        if (strcmp(a, "help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(a, "selftest") == 0) {
            rng_seed(1);
            return run_selftest();
        }
        {
            char *eq = strchr(a, '=');

            if (!eq) {
                fprintf(stderr, "unknown option --%s\n", a);
                return 2;
            }
            *eq = '\0';
            if (strcmp(a, "seed") == 0) {
                seed = strtoul(eq + 1, NULL, 10);
            } else if (!config_set(a, eq + 1)) {
                fprintf(stderr, "unknown option --%s\n", a);
                return 2;
            } else if (strcmp(a, "mode") == 0) {
                mode_given = 1;
            }
        }
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "tetris needs an interactive terminal\n");
        return 2;
    }

    rng_seed(seed ? seed : (unsigned long)time(NULL) ^ (unsigned long)getpid());
    board_load();
    detect_color_depth();
    term_init();
    kitty_enable();
    term_size();
    layout();
    scr_invalidate();
    g_menu_sel = MENU_MARATHON + g_cfg.mode;
    game_reset(&g_game, g_cfg.mode);
    if (mode_given) {
        start_game(g_cfg.mode);   /* an explicit --mode skips the menu */
    }

    last = now_us();
    while (!g_interrupted) {
        long frame_start = now_us();
        long dt = frame_start - last;

        last = frame_start;
        if (dt > 200000) {
            dt = 200000;        /* do not fast-forward after a suspend */
        }

        input_poll();
        fallback_expire_keys(frame_start);

        if (g_resized) {
            g_resized = 0;
            term_size();
            layout();
            scr_invalidate();
        }

        if (g_scr_w < MIN_SCREEN_W || g_scr_h < MIN_SCREEN_H) {
            draw_too_small();
            scr_flush();
            wait_for_frame(frame_start);
            continue;
        }

        if (g_name_editing) {
            int k;

            if (g_keys[K_BACKSPACE].edges) {
                size_t len = strlen(g_player);

                if (len) {
                    g_player[len - 1] = '\0';
                }
            }
            if (g_keys[K_ENTER].edges || g_keys[K_ESC].edges) {
                g_name_editing = 0;
                board_save();       /* remember the name for next time */
            } else {
                for (k = 0; k < g_text_len; k++) {
                    size_t len = strlen(g_player);

                    if (len < NAME_MAX) {
                        g_player[len] = g_text[k];
                        g_player[len + 1] = '\0';
                    }
                }
            }
            /* while typing, no key means anything else */
            for (k = 0; k < K_MAX; k++) {
                g_keys[k].edges = 0;
            }
            g_text_len = 0;
        } else {
            g_text_len = 0;
            if (action_pressed(ACT_QUIT)) {
                break;
            }
            if (action_pressed(ACT_STYLE)) {
                g_cfg.style = (g_cfg.style + 1) % STYLE_COUNT;
                scr_invalidate();
            }
            if (action_pressed(ACT_HELP)) {
                g_help_visible = !g_help_visible;
            }
        }

        if (g_in_board) {
            while (g_keys[K_LEFT].edges > 0) {
                g_keys[K_LEFT].edges--;
                g_board_mode = (g_board_mode + MODE_COUNT - 1) % MODE_COUNT;
            }
            while (g_keys[K_RIGHT].edges > 0) {
                g_keys[K_RIGHT].edges--;
                g_board_mode = (g_board_mode + 1) % MODE_COUNT;
            }
            if (g_keys[K_ENTER].edges || action_pressed(ACT_PAUSE)) {
                g_keys[K_ENTER].edges = 0;
                g_in_board = 0;
            }
        } else if (g_in_menu) {
            /* one step per press, not per frame, so nothing is lost when two
             * land between two frames */
            while (g_keys[K_UP].edges > 0) {
                g_keys[K_UP].edges--;
                g_menu_sel = (g_menu_sel + MENU_COUNT - 1) % MENU_COUNT;
            }
            while (g_keys[K_DOWN].edges > 0) {
                g_keys[K_DOWN].edges--;
                g_menu_sel = (g_menu_sel + 1) % MENU_COUNT;
            }
            if (g_keys[K_ENTER].edges || g_keys[K_SPACE].edges) {
                g_keys[K_ENTER].edges = 0;
                g_keys[K_SPACE].edges = 0;
                if (g_menu_sel == MENU_NAME) {
                    g_name_editing = 1;
                    g_text_len = 0;
                } else if (g_menu_sel == MENU_BOARD) {
                    int probe[1];
                    int m;

                    /* open on a mode that has something to show */
                    if (!board_top(g_board_mode, probe, 1)) {
                        for (m = 0; m < MODE_COUNT; m++) {
                            if (board_top(m, probe, 1)) {
                                g_board_mode = m;
                                break;
                            }
                        }
                    }
                    g_in_board = 1;
                } else {
                    start_game(g_menu_sel - MENU_MARATHON);
                }
            }
            action_pressed(ACT_PAUSE);   /* swallow escape in the menu */
        } else {
            /* Restarting throws away the run, so it wants a deliberate hold
             * rather than a stray keypress. The help panel draws the bar. */
            if (action_down(ACT_RESTART)) {
                g_restart_hold += dt;
                if (g_restart_hold >= RESTART_HOLD_US) {
                    start_game(g_game.mode);
                }
            } else {
                g_restart_hold = 0;
            }
            action_pressed(ACT_RESTART);        /* the edge alone does nothing */

            if (action_pressed(ACT_PAUSE)) {
                if (g_game.state == STATE_PLAYING) {
                    g_game.state = STATE_PAUSED;
                } else if (g_game.state == STATE_PAUSED) {
                    g_game.state = STATE_PLAYING;
                    keys_clear();
                } else if (g_game.state == STATE_GAMEOVER) {
                    g_in_menu = 1;
                    g_menu_sel = MENU_MARATHON + g_game.mode;
                    g_board_mode = g_game.mode;
                    /* Whatever was still held or queued belongs to the run
                     * that just ended; without this a player who was mashing
                     * hard drop would start the next run on the way in. */
                    keys_clear();
                }
            }
            game_update(&g_game, dt);
            if (g_game.state == STATE_GAMEOVER && !g_submitted) {
                g_submitted = 1;
                g_last_rank = board_add(&g_game);
            }
        }

        scr_clear();
        if (g_in_board) {
            draw_scoreboard();
        } else if (g_in_menu) {
            draw_menu();
        } else {
            draw_border(&g_game);
            draw_field(&g_game);
            draw_panels(&g_game);
            if (g_game.state == STATE_PAUSED) {
                draw_pause();
            } else if (g_game.state == STATE_GAMEOVER) {
                draw_gameover(&g_game);
            }
        }
        scr_flush();

        wait_for_frame(frame_start);
    }

    term_restore();
    return 0;
}
