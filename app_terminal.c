/*
 * app_terminal.c — VT100 cell-grid renderer for the Lumo terminal app.
 *
 * Draws the terminal cell grid with 256-color support, cursor,
 * bold/dim/underline/inverse attributes, and the existing header bar,
 * menu overlay, and pinch-to-zoom.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include "lumo/lumo_term.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void lumo_app_render_terminal(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    uint32_t bg = theme.bg;
    uint32_t prompt_color = theme.accent;
    uint32_t dim_color = theme.separator;
    uint32_t header_bg = theme.header_bg;

    double zoom = (ctx != NULL && ctx->zoom_scale > 0.0)
        ? ctx->zoom_scale : 1.0;
    int scale = (int)(2.0 * zoom + 0.5);
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    int char_w = scale * 6;
    int line_h = scale * 6 + 4;
    int margin = 8;
    int header_h = 38;

    bool menu_open = ctx != NULL ? ctx->term_menu_open : false;
    const struct lumo_term *term = ctx != NULL ? ctx->term : NULL;

    memset(pixels, 0, (size_t)width * height * 4);
    lumo_app_fill_rect(pixels, width, height, 0, 0,
        (int)width, (int)height, bg);

    /* header bar */
    lumo_app_fill_rect(pixels, width, height, 0, 0,
        (int)width, header_h, header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 12, 2,
        prompt_color, "LUMO TERMINAL");
    lumo_app_draw_text(pixels, width, height, (int)width - 40, 12, 2,
        theme.text_dim, "...");
    lumo_app_fill_rect(pixels, width, height, margin, header_h,
        (int)width - margin * 2, 1, dim_color);

    if (menu_open) {
        for (uint32_t py = (uint32_t)header_h; py < height; py++) {
            uint32_t *row = pixels + py * width;
            for (uint32_t px = 0; px < width; px++) {
                row[px] = ((row[px] >> 1) & 0x007F7F7F) | 0xFF000000;
            }
        }
        int menu_w = (int)width * 2 / 3;
        int menu_h = 220;
        if (menu_w < 240) menu_w = 240;
        int menu_x = ((int)width - menu_w) / 2;
        int menu_y = ((int)height - menu_h) / 2;
        int item_h = 44;
        int pad = 24;

        struct lumo_rect menu_bg_rect = {menu_x, menu_y, menu_w, menu_h};
        lumo_app_fill_rounded_rect(pixels, width, height,
            &menu_bg_rect, 18,
            lumo_app_argb(0xF4, 0x28, 0x28, 0x2E));
        lumo_app_draw_outline(pixels, width, height,
            &menu_bg_rect, 1, theme.card_stroke);
        lumo_app_draw_text(pixels, width, height,
            menu_x + pad, menu_y + 16, 2, theme.text_dim,
            "TERMINAL MENU");
        lumo_app_fill_rect(pixels, width, height,
            menu_x + pad, menu_y + 36, menu_w - pad * 2, 1,
            theme.separator);
        int iy = menu_y + 46;
        lumo_app_draw_text(pixels, width, height,
            menu_x + pad, iy, 3, theme.text, "NEW");
        iy += item_h;
        lumo_app_draw_text(pixels, width, height,
            menu_x + pad, iy, 3, theme.accent, "KEYBOARD");
        iy += item_h;
        lumo_app_draw_text(pixels, width, height,
            menu_x + pad, iy, 3, theme.text, "SETTINGS");
        iy += item_h;
        lumo_app_draw_text(pixels, width, height,
            menu_x + pad, iy, 3, theme.text_dim, "ABOUT");
        return;
    }

    /* ── cell grid rendering ─────────────────────────────────────── */

    if (term == NULL) {
        lumo_app_draw_text(pixels, width, height, margin,
            header_h + 8, 2, theme.text_dim, "NO TERMINAL");
        return;
    }

    int content_y = header_h + 4;
    int grid_rows = term->rows;
    int grid_cols = term->cols;
    const struct lumo_term_cell *screen = lumo_term_screen(term);

    /* default fg/bg from theme */
    uint32_t default_fg = theme.text;
    uint32_t default_bg = bg;

    /* blinking cursor */
    bool cursor_visible;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        cursor_visible = term->cursor_visible &&
            (ts.tv_nsec / 500000000) == 0;
    }

    for (int row = 0; row < grid_rows; row++) {
        int py = content_y + row * line_h;
        if (py + line_h > (int)height) break;

        for (int col = 0; col < grid_cols; col++) {
            int px = margin + col * char_w;
            if (px + char_w > (int)width) break;

            const struct lumo_term_cell *cell =
                &screen[row * grid_cols + col];

            /* resolve colors */
            uint32_t fg_argb = (cell->attr & LUMO_TERM_ATTR_DEFAULT_FG)
                ? default_fg : lumo_term_color_argb(cell->fg);
            uint32_t bg_argb = (cell->attr & LUMO_TERM_ATTR_DEFAULT_BG)
                ? default_bg : lumo_term_color_argb(cell->bg);

            /* bold: use bright variant for standard colors 0-7 */
            if ((cell->attr & LUMO_TERM_ATTR_BOLD) &&
                    !(cell->attr & LUMO_TERM_ATTR_DEFAULT_FG) &&
                    cell->fg < 8) {
                fg_argb = lumo_term_color_argb(cell->fg + 8);
            }

            /* dim: reduce alpha */
            if (cell->attr & LUMO_TERM_ATTR_DIM) {
                uint32_t r = (fg_argb >> 16) & 0xFF;
                uint32_t g = (fg_argb >> 8) & 0xFF;
                uint32_t b = fg_argb & 0xFF;
                fg_argb = 0xFF000000 | ((r / 2) << 16) |
                    ((g / 2) << 8) | (b / 2);
            }

            /* inverse: swap fg/bg */
            if (cell->attr & LUMO_TERM_ATTR_INVERSE) {
                uint32_t tmp = fg_argb;
                fg_argb = bg_argb;
                bg_argb = tmp;
            }

            /* draw background if non-default */
            if (bg_argb != default_bg) {
                lumo_app_fill_rect(pixels, width, height,
                    px, py, char_w, line_h, bg_argb);
            }

            /* draw character */
            char ch = cell->ch;
            if (ch >= 0x21 && ch < 0x7f &&
                    !(cell->attr & LUMO_TERM_ATTR_HIDDEN)) {
                char str[2] = {ch, '\0'};
                lumo_app_draw_text(pixels, width, height,
                    px, py, scale, fg_argb, str);
            }

            /* underline */
            if (cell->attr & LUMO_TERM_ATTR_UNDERLINE) {
                lumo_app_fill_rect(pixels, width, height,
                    px, py + line_h - 2, char_w, 1, fg_argb);
            }
        }
    }

    /* cursor */
    if (cursor_visible && term->cursor_row < grid_rows &&
            term->cursor_col < grid_cols) {
        int cx = margin + term->cursor_col * char_w;
        int cy = content_y + term->cursor_row * line_h;
        /* block cursor with inverted colors */
        lumo_app_fill_rect(pixels, width, height,
            cx, cy, char_w, line_h,
            lumo_app_argb(0xC0, 0xFF, 0xFF, 0xFF));
        /* redraw character under cursor in black */
        const struct lumo_term_cell *cursor_cell =
            &screen[term->cursor_row * grid_cols + term->cursor_col];
        if (cursor_cell->ch >= 0x21 && cursor_cell->ch < 0x7f) {
            char str[2] = {cursor_cell->ch, '\0'};
            lumo_app_draw_text(pixels, width, height, cx, cy, scale,
                lumo_app_argb(0xFF, 0x00, 0x00, 0x00), str);
        }
    }

    /* status line at bottom */
    {
        char status[64];
        snprintf(status, sizeof(status), "%dx%d  %s",
            grid_cols, grid_rows,
            term->alt_active ? "ALT" : "");
        lumo_app_draw_text(pixels, width, height,
            margin, (int)height - 14, 1, theme.text_dim, status);
    }
}
