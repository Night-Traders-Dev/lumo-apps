/*
 * lumo_term.c — VT100/xterm-256color terminal emulator for Lumo OS.
 *
 * Implements a cell-based grid with ANSI CSI parsing, 256-color SGR,
 * cursor movement, scroll regions, alternate screen buffer, and
 * device attribute responses.  Enough to run btop, vim, nano, top,
 * tmux, and other curses-based programs.
 */
#define _DEFAULT_SOURCE
#include "lumo/lumo_term.h"
#include <stdio.h>
#include <string.h>

/* ── 256-color palette (xterm defaults) ──────────────────────────── */

static const uint32_t palette_16[16] = {
    0xFF000000, 0xFFCD0000, 0xFF00CD00, 0xFFCDCD00,
    0xFF0000EE, 0xFFCD00CD, 0xFF00CDCD, 0xFFE5E5E5,
    0xFF7F7F7F, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00,
    0xFF5C5CFF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
};

uint32_t lumo_term_color_argb(uint8_t index) {
    if (index < 16) return palette_16[index];

    if (index < 232) {
        /* 6×6×6 color cube (indices 16-231) */
        int ci = index - 16;
        int b = ci % 6;
        int g = (ci / 6) % 6;
        int r = ci / 36;
        uint8_t rv = r ? (uint8_t)(r * 40 + 55) : 0;
        uint8_t gv = g ? (uint8_t)(g * 40 + 55) : 0;
        uint8_t bv = b ? (uint8_t)(b * 40 + 55) : 0;
        return 0xFF000000 | ((uint32_t)rv << 16) |
            ((uint32_t)gv << 8) | bv;
    }

    /* grayscale ramp (indices 232-255) */
    uint8_t v = (uint8_t)(8 + (index - 232) * 10);
    return 0xFF000000 | ((uint32_t)v << 16) |
        ((uint32_t)v << 8) | v;
}

/* ── helpers ─────────────────────────────────────────────────────── */

static struct lumo_term_cell *term_screen(struct lumo_term *t) {
    return t->alt_active ? t->alt_cells : t->cells;
}

static struct lumo_term_cell *term_cell(struct lumo_term *t, int row, int col) {
    return &term_screen(t)[row * t->cols + col];
}

static void term_clear_cell(struct lumo_term_cell *c) {
    c->ch = 0;
    c->fg = 7;
    c->bg = 0;
    c->attr = LUMO_TERM_ATTR_DEFAULT_FG | LUMO_TERM_ATTR_DEFAULT_BG;
}

static void term_clear_region(struct lumo_term *t,
    int r1, int c1, int r2, int c2)
{
    struct lumo_term_cell *scr = term_screen(t);
    for (int r = r1; r <= r2; r++) {
        for (int c = c1; c <= c2; c++) {
            if (r >= 0 && r < t->rows && c >= 0 && c < t->cols)
                term_clear_cell(&scr[r * t->cols + c]);
        }
    }
}

static void term_scroll_up(struct lumo_term *t, int top, int bottom, int n) {
    struct lumo_term_cell *scr = term_screen(t);
    if (n <= 0 || top > bottom) return;
    if (n > bottom - top + 1) n = bottom - top + 1;

    int line_bytes = t->cols * (int)sizeof(struct lumo_term_cell);
    memmove(&scr[top * t->cols], &scr[(top + n) * t->cols],
        (size_t)(bottom - top - n + 1) * (size_t)line_bytes);

    /* clear new lines at bottom */
    for (int r = bottom - n + 1; r <= bottom; r++) {
        for (int c = 0; c < t->cols; c++)
            term_clear_cell(&scr[r * t->cols + c]);
    }
}

static void term_scroll_down(struct lumo_term *t, int top, int bottom, int n) {
    struct lumo_term_cell *scr = term_screen(t);
    if (n <= 0 || top > bottom) return;
    if (n > bottom - top + 1) n = bottom - top + 1;

    int line_bytes = t->cols * (int)sizeof(struct lumo_term_cell);
    memmove(&scr[(top + n) * t->cols], &scr[top * t->cols],
        (size_t)(bottom - top - n + 1) * (size_t)line_bytes);

    for (int r = top; r < top + n; r++) {
        for (int c = 0; c < t->cols; c++)
            term_clear_cell(&scr[r * t->cols + c]);
    }
}

static void term_linefeed(struct lumo_term *t) {
    if (t->cursor_row == t->scroll_bottom) {
        term_scroll_up(t, t->scroll_top, t->scroll_bottom, 1);
    } else if (t->cursor_row < t->rows - 1) {
        t->cursor_row++;
    }
}

static void term_reverse_index(struct lumo_term *t) {
    if (t->cursor_row == t->scroll_top) {
        term_scroll_down(t, t->scroll_top, t->scroll_bottom, 1);
    } else if (t->cursor_row > 0) {
        t->cursor_row--;
    }
}

static void term_put_response(struct lumo_term *t, const char *s) {
    int len = (int)strlen(s);
    if (t->response_len + len > (int)sizeof(t->response))
        len = (int)sizeof(t->response) - t->response_len;
    memcpy(t->response + t->response_len, s, (size_t)len);
    t->response_len += len;
}

/* ── init / reset ────────────────────────────────────────────────── */

void lumo_term_init(struct lumo_term *t, int cols, int rows) {
    memset(t, 0, sizeof(*t));
    if (cols > LUMO_TERM_MAX_COLS) cols = LUMO_TERM_MAX_COLS;
    if (rows > LUMO_TERM_MAX_ROWS) rows = LUMO_TERM_MAX_ROWS;
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;
    t->cols = cols;
    t->rows = rows;
    t->cursor_visible = true;
    t->autowrap = true;
    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;
    t->current_attr = LUMO_TERM_ATTR_DEFAULT_FG | LUMO_TERM_ATTR_DEFAULT_BG;
    t->current_fg = 7;
    t->current_bg = 0;

    /* default tab stops every 8 columns */
    for (int c = 0; c < LUMO_TERM_MAX_COLS; c++)
        t->tab_stops[c] = (c % 8 == 0) && c > 0;

    /* clear both buffers */
    for (int i = 0; i < LUMO_TERM_MAX_ROWS * LUMO_TERM_MAX_COLS; i++) {
        term_clear_cell(&t->cells[i]);
        term_clear_cell(&t->alt_cells[i]);
    }
}

void lumo_term_reset(struct lumo_term *t) {
    int cols = t->cols, rows = t->rows;
    lumo_term_init(t, cols, rows);
}

void lumo_term_resize(struct lumo_term *t, int cols, int rows) {
    if (cols > LUMO_TERM_MAX_COLS) cols = LUMO_TERM_MAX_COLS;
    if (rows > LUMO_TERM_MAX_ROWS) rows = LUMO_TERM_MAX_ROWS;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    t->cols = cols;
    t->rows = rows;
    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;

    if (t->cursor_row >= rows) t->cursor_row = rows - 1;
    if (t->cursor_col >= cols) t->cursor_col = cols - 1;
}

const struct lumo_term_cell *lumo_term_screen(const struct lumo_term *t) {
    return t->alt_active ? t->alt_cells : t->cells;
}

int lumo_term_drain_response(struct lumo_term *t, char *buf, size_t buf_size) {
    if (t->response_len <= 0) return 0;
    int n = t->response_len;
    if (n > (int)buf_size) n = (int)buf_size;
    memcpy(buf, t->response, (size_t)n);
    t->response_len -= n;
    if (t->response_len > 0)
        memmove(t->response, t->response + n, (size_t)t->response_len);
    return n;
}

/* ── CSI parameter parsing ───────────────────────────────────────── */

static int csi_param(struct lumo_term *t, int idx, int def) {
    if (idx < 0 || idx >= t->csi_param_count) return def;
    return t->csi_params[idx] > 0 ? t->csi_params[idx] : def;
}

/* ── SGR (Select Graphic Rendition) ──────────────────────────────── */

static void term_handle_sgr(struct lumo_term *t) {
    if (t->csi_param_count == 0) {
        /* ESC[m = reset */
        t->current_fg = 7;
        t->current_bg = 0;
        t->current_attr = LUMO_TERM_ATTR_DEFAULT_FG |
            LUMO_TERM_ATTR_DEFAULT_BG;
        return;
    }

    for (int i = 0; i < t->csi_param_count; i++) {
        int p = t->csi_params[i];

        if (p == 0) {
            t->current_fg = 7;
            t->current_bg = 0;
            t->current_attr = LUMO_TERM_ATTR_DEFAULT_FG |
                LUMO_TERM_ATTR_DEFAULT_BG;
        } else if (p == 1) {
            t->current_attr |= LUMO_TERM_ATTR_BOLD;
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_DIM;
        } else if (p == 2) {
            t->current_attr |= LUMO_TERM_ATTR_DIM;
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_BOLD;
        } else if (p == 4) {
            t->current_attr |= LUMO_TERM_ATTR_UNDERLINE;
        } else if (p == 7) {
            t->current_attr |= LUMO_TERM_ATTR_INVERSE;
        } else if (p == 8) {
            t->current_attr |= LUMO_TERM_ATTR_HIDDEN;
        } else if (p == 22) {
            t->current_attr &= (uint8_t)~(LUMO_TERM_ATTR_BOLD |
                LUMO_TERM_ATTR_DIM);
        } else if (p == 24) {
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_UNDERLINE;
        } else if (p == 27) {
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_INVERSE;
        } else if (p == 28) {
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_HIDDEN;
        } else if (p >= 30 && p <= 37) {
            t->current_fg = (uint8_t)(p - 30);
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_DEFAULT_FG;
        } else if (p == 38 && i + 2 < t->csi_param_count &&
                t->csi_params[i + 1] == 5) {
            /* 256-color fg: ESC[38;5;Nm */
            t->current_fg = (uint8_t)t->csi_params[i + 2];
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_DEFAULT_FG;
            i += 2;
        } else if (p == 39) {
            t->current_fg = 7;
            t->current_attr |= LUMO_TERM_ATTR_DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            t->current_bg = (uint8_t)(p - 40);
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_DEFAULT_BG;
        } else if (p == 48 && i + 2 < t->csi_param_count &&
                t->csi_params[i + 1] == 5) {
            /* 256-color bg: ESC[48;5;Nm */
            t->current_bg = (uint8_t)t->csi_params[i + 2];
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_DEFAULT_BG;
            i += 2;
        } else if (p == 49) {
            t->current_bg = 0;
            t->current_attr |= LUMO_TERM_ATTR_DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            /* bright fg */
            t->current_fg = (uint8_t)(p - 90 + 8);
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_DEFAULT_FG;
        } else if (p >= 100 && p <= 107) {
            /* bright bg */
            t->current_bg = (uint8_t)(p - 100 + 8);
            t->current_attr &= (uint8_t)~LUMO_TERM_ATTR_DEFAULT_BG;
        }
        /* ignore unrecognized SGR parameters */
    }
}

/* ── CSI dispatch ────────────────────────────────────────────────── */

static void term_handle_csi(struct lumo_term *t, char final) {
    int n, m;

    if (t->csi_private) {
        /* private mode set/reset: CSI ? ... h/l */
        int mode = csi_param(t, 0, 0);
        bool set = (final == 'h');

        switch (mode) {
        case 1:    /* DECCKM — application cursor keys */
            t->app_cursor_keys = set;
            break;
        case 6:    /* DECOM — origin mode */
            t->origin_mode = set;
            t->cursor_row = set ? t->scroll_top : 0;
            t->cursor_col = 0;
            break;
        case 7:    /* DECAWM — auto-wrap */
            t->autowrap = set;
            break;
        case 25:   /* DECTCEM — cursor visibility */
            t->cursor_visible = set;
            break;
        case 1049: /* alternate screen buffer */
            if (set && !t->alt_active) {
                /* save cursor and switch to alt screen */
                t->saved_row = t->cursor_row;
                t->saved_col = t->cursor_col;
                t->saved_fg = t->current_fg;
                t->saved_bg = t->current_bg;
                t->saved_attr = t->current_attr;
                t->alt_active = true;
                term_clear_region(t, 0, 0, t->rows - 1, t->cols - 1);
                t->cursor_row = 0;
                t->cursor_col = 0;
            } else if (!set && t->alt_active) {
                /* restore primary screen and cursor */
                t->alt_active = false;
                t->cursor_row = t->saved_row;
                t->cursor_col = t->saved_col;
                t->current_fg = t->saved_fg;
                t->current_bg = t->saved_bg;
                t->current_attr = t->saved_attr;
            }
            break;
        case 2004: /* bracketed paste */
            t->bracketed_paste = set;
            break;
        }
        return;
    }

    switch (final) {
    case 'A': /* CUU — cursor up */
        n = csi_param(t, 0, 1);
        t->cursor_row -= n;
        if (t->cursor_row < 0) t->cursor_row = 0;
        break;

    case 'B': /* CUD — cursor down */
        n = csi_param(t, 0, 1);
        t->cursor_row += n;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        break;

    case 'C': /* CUF — cursor forward */
        n = csi_param(t, 0, 1);
        t->cursor_col += n;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;

    case 'D': /* CUB — cursor back */
        n = csi_param(t, 0, 1);
        t->cursor_col -= n;
        if (t->cursor_col < 0) t->cursor_col = 0;
        break;

    case 'E': /* CNL — cursor next line */
        n = csi_param(t, 0, 1);
        t->cursor_row += n;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        t->cursor_col = 0;
        break;

    case 'F': /* CPL — cursor previous line */
        n = csi_param(t, 0, 1);
        t->cursor_row -= n;
        if (t->cursor_row < 0) t->cursor_row = 0;
        t->cursor_col = 0;
        break;

    case 'G': /* CHA — cursor character absolute */
    case '`': /* HPA — also cursor character absolute */
        n = csi_param(t, 0, 1) - 1;
        if (n < 0) n = 0;
        if (n >= t->cols) n = t->cols - 1;
        t->cursor_col = n;
        break;

    case 'H': /* CUP — cursor position */
    case 'f': /* HVP — also cursor position */
        n = csi_param(t, 0, 1) - 1;
        m = csi_param(t, 1, 1) - 1;
        if (n < 0) n = 0;
        if (m < 0) m = 0;
        if (n >= t->rows) n = t->rows - 1;
        if (m >= t->cols) m = t->cols - 1;
        t->cursor_row = n;
        t->cursor_col = m;
        break;

    case 'J': /* ED — erase in display */
        n = csi_param(t, 0, 0);
        if (n == 0) {
            /* erase from cursor to end */
            term_clear_region(t, t->cursor_row, t->cursor_col,
                t->cursor_row, t->cols - 1);
            term_clear_region(t, t->cursor_row + 1, 0,
                t->rows - 1, t->cols - 1);
        } else if (n == 1) {
            /* erase from start to cursor */
            term_clear_region(t, 0, 0, t->cursor_row - 1, t->cols - 1);
            term_clear_region(t, t->cursor_row, 0,
                t->cursor_row, t->cursor_col);
        } else if (n == 2 || n == 3) {
            /* erase entire display */
            term_clear_region(t, 0, 0, t->rows - 1, t->cols - 1);
        }
        break;

    case 'K': /* EL — erase in line */
        n = csi_param(t, 0, 0);
        if (n == 0) {
            term_clear_region(t, t->cursor_row, t->cursor_col,
                t->cursor_row, t->cols - 1);
        } else if (n == 1) {
            term_clear_region(t, t->cursor_row, 0,
                t->cursor_row, t->cursor_col);
        } else if (n == 2) {
            term_clear_region(t, t->cursor_row, 0,
                t->cursor_row, t->cols - 1);
        }
        break;

    case 'L': /* IL — insert lines */
        n = csi_param(t, 0, 1);
        if (t->cursor_row >= t->scroll_top &&
                t->cursor_row <= t->scroll_bottom)
            term_scroll_down(t, t->cursor_row, t->scroll_bottom, n);
        break;

    case 'M': /* DL — delete lines */
        n = csi_param(t, 0, 1);
        if (t->cursor_row >= t->scroll_top &&
                t->cursor_row <= t->scroll_bottom)
            term_scroll_up(t, t->cursor_row, t->scroll_bottom, n);
        break;

    case '@': /* ICH — insert characters */
        n = csi_param(t, 0, 1);
        if (n > t->cols - t->cursor_col)
            n = t->cols - t->cursor_col;
        if (n > 0) {
            struct lumo_term_cell *scr = term_screen(t);
            int row_off = t->cursor_row * t->cols;
            memmove(&scr[row_off + t->cursor_col + n],
                &scr[row_off + t->cursor_col],
                (size_t)(t->cols - t->cursor_col - n) *
                    sizeof(struct lumo_term_cell));
            for (int c = t->cursor_col; c < t->cursor_col + n; c++)
                term_clear_cell(&scr[row_off + c]);
        }
        break;

    case 'P': /* DCH — delete characters */
        n = csi_param(t, 0, 1);
        if (n > t->cols - t->cursor_col)
            n = t->cols - t->cursor_col;
        if (n > 0) {
            struct lumo_term_cell *scr = term_screen(t);
            int row_off = t->cursor_row * t->cols;
            memmove(&scr[row_off + t->cursor_col],
                &scr[row_off + t->cursor_col + n],
                (size_t)(t->cols - t->cursor_col - n) *
                    sizeof(struct lumo_term_cell));
            for (int c = t->cols - n; c < t->cols; c++)
                term_clear_cell(&scr[row_off + c]);
        }
        break;

    case 'X': /* ECH — erase characters */
        n = csi_param(t, 0, 1);
        for (int c = t->cursor_col;
                c < t->cursor_col + n && c < t->cols; c++)
            term_clear_cell(term_cell(t, t->cursor_row, c));
        break;

    case 'S': /* SU — scroll up */
        n = csi_param(t, 0, 1);
        term_scroll_up(t, t->scroll_top, t->scroll_bottom, n);
        break;

    case 'T': /* SD — scroll down */
        n = csi_param(t, 0, 1);
        term_scroll_down(t, t->scroll_top, t->scroll_bottom, n);
        break;

    case 'd': /* VPA — line position absolute */
        n = csi_param(t, 0, 1) - 1;
        if (n < 0) n = 0;
        if (n >= t->rows) n = t->rows - 1;
        t->cursor_row = n;
        break;

    case 'm': /* SGR — select graphic rendition */
        term_handle_sgr(t);
        break;

    case 'r': /* DECSTBM — set scrolling region */
        n = csi_param(t, 0, 1) - 1;
        m = csi_param(t, 1, t->rows) - 1;
        if (n < 0) n = 0;
        if (m >= t->rows) m = t->rows - 1;
        if (n < m) {
            t->scroll_top = n;
            t->scroll_bottom = m;
        }
        t->cursor_row = 0;
        t->cursor_col = 0;
        break;

    case 's': /* SCP — save cursor position */
        t->saved_row = t->cursor_row;
        t->saved_col = t->cursor_col;
        t->saved_fg = t->current_fg;
        t->saved_bg = t->current_bg;
        t->saved_attr = t->current_attr;
        break;

    case 'u': /* RCP — restore cursor position */
        t->cursor_row = t->saved_row;
        t->cursor_col = t->saved_col;
        t->current_fg = t->saved_fg;
        t->current_bg = t->saved_bg;
        t->current_attr = t->saved_attr;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;

    case 'n': /* DSR — device status report */
        n = csi_param(t, 0, 0);
        if (n == 6) {
            char resp[32];
            snprintf(resp, sizeof(resp), "\x1b[%d;%dR",
                t->cursor_row + 1, t->cursor_col + 1);
            term_put_response(t, resp);
        } else if (n == 5) {
            term_put_response(t, "\x1b[0n"); /* terminal OK */
        }
        break;

    case 'c': /* DA — device attributes */
        if (csi_param(t, 0, 0) == 0) {
            /* respond as VT100 with AVO */
            term_put_response(t, "\x1b[?1;2c");
        }
        break;

    case 't': /* window manipulation (ignore most) */
        break;

    default:
        /* unrecognized CSI — silently ignore */
        break;
    }
}

/* ── main feed function ──────────────────────────────────────────── */

static void term_put_char(struct lumo_term *t, char ch) {
    struct lumo_term_cell *c;

    if (t->cursor_col >= t->cols) {
        if (t->autowrap) {
            t->cursor_col = 0;
            term_linefeed(t);
        } else {
            t->cursor_col = t->cols - 1;
        }
    }

    c = term_cell(t, t->cursor_row, t->cursor_col);
    c->ch = ch;
    c->fg = t->current_fg;
    c->bg = t->current_bg;
    c->attr = t->current_attr;
    t->cursor_col++;
}

void lumo_term_feed(struct lumo_term *t, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        switch (t->parse_state) {
        case LUMO_TERM_STATE_GROUND:
            if (ch == '\x1b') {
                t->parse_state = LUMO_TERM_STATE_ESC;
            } else if (ch == '\n' || ch == '\x0b' || ch == '\x0c') {
                term_linefeed(t);
            } else if (ch == '\r') {
                t->cursor_col = 0;
            } else if (ch == '\b') {
                if (t->cursor_col > 0) t->cursor_col--;
            } else if (ch == '\t') {
                /* advance to next tab stop */
                do {
                    t->cursor_col++;
                } while (t->cursor_col < t->cols &&
                    !t->tab_stops[t->cursor_col]);
                if (t->cursor_col >= t->cols)
                    t->cursor_col = t->cols - 1;
            } else if (ch == '\a') {
                /* bell — ignore */
            } else if (ch == 0x0e || ch == 0x0f) {
                /* SI/SO — ignore charset switching */
            } else if (ch >= 0x20 && ch < 0x7f) {
                term_put_char(t, (char)ch);
            } else if (ch >= 0x80) {
                /* non-ASCII: show placeholder */
                term_put_char(t, '?');
            }
            break;

        case LUMO_TERM_STATE_ESC:
            if (ch == '[') {
                t->parse_state = LUMO_TERM_STATE_CSI;
                t->csi_param_count = 0;
                t->csi_private = false;
                memset(t->csi_params, 0, sizeof(t->csi_params));
            } else if (ch == ']') {
                t->parse_state = LUMO_TERM_STATE_OSC;
            } else if (ch == '7') {
                /* DECSC — save cursor */
                t->saved_row = t->cursor_row;
                t->saved_col = t->cursor_col;
                t->saved_fg = t->current_fg;
                t->saved_bg = t->current_bg;
                t->saved_attr = t->current_attr;
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == '8') {
                /* DECRC — restore cursor */
                t->cursor_row = t->saved_row;
                t->cursor_col = t->saved_col;
                t->current_fg = t->saved_fg;
                t->current_bg = t->saved_bg;
                t->current_attr = t->saved_attr;
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == 'D') {
                /* IND — index (linefeed) */
                term_linefeed(t);
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == 'M') {
                /* RI — reverse index */
                term_reverse_index(t);
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == 'c') {
                /* RIS — full reset */
                lumo_term_reset(t);
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == '=' || ch == '>') {
                /* DECKPAM / DECKPNM — keypad modes, ignore */
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == '(') {
                /* designate G0 charset — skip next byte */
                i++; /* consume the charset designator */
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == ')') {
                /* designate G1 charset — skip next byte */
                i++;
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else {
                t->parse_state = LUMO_TERM_STATE_GROUND;
            }
            break;

        case LUMO_TERM_STATE_CSI:
            if (ch == '?') {
                t->csi_private = true;
            } else if (ch >= '0' && ch <= '9') {
                if (t->csi_param_count == 0)
                    t->csi_param_count = 1;
                t->csi_params[t->csi_param_count - 1] =
                    t->csi_params[t->csi_param_count - 1] * 10 +
                    (ch - '0');
            } else if (ch == ';') {
                if (t->csi_param_count < 16)
                    t->csi_param_count++;
            } else if (ch >= 0x40 && ch <= 0x7e) {
                /* final byte — dispatch */
                if (t->csi_param_count == 0 &&
                        (ch != 'm' && ch != 'H' && ch != 'J' &&
                         ch != 'K' && ch != 'r'))
                    t->csi_param_count = 0;
                term_handle_csi(t, (char)ch);
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == 0x1b) {
                /* unexpected ESC in CSI — abort and reprocess */
                t->parse_state = LUMO_TERM_STATE_ESC;
            }
            /* intermediate bytes (0x20-0x2F) are silently consumed */
            break;

        case LUMO_TERM_STATE_OSC:
            /* absorb until ST (\x1b\\ or BEL \x07) */
            if (ch == '\x07') {
                t->parse_state = LUMO_TERM_STATE_GROUND;
            } else if (ch == '\x1b') {
                /* check for ST (\x1b\\) */
                if (i + 1 < len && data[i + 1] == '\\') {
                    i++;
                    t->parse_state = LUMO_TERM_STATE_GROUND;
                } else {
                    t->parse_state = LUMO_TERM_STATE_ESC;
                }
            }
            /* OSC content is ignored (window titles etc.) */
            break;
        }
    }
}
