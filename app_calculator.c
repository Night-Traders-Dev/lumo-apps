/*
 * app_calculator.c — Calculator app for Lumo OS.
 * Standard calculator with +, -, *, /, =, C, and decimal point.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* hit-test: 0-9=digits, 10=dot, 11=plus, 12=minus, 13=mul, 14=div,
 * 15=equals, 16=clear, 17=backspace, 18=negate, -1=nothing */
int lumo_app_calc_button_at(uint32_t width, uint32_t height,
    double x, double y)
{
    int w = (int)width, h = (int)height;
    int tx = (int)x, ty = (int)y;
    int display_h = h / 4;
    if (ty < display_h) return -1;

    int grid_y = ty - display_h;
    int btn_w = w / 4;
    int btn_h = (h - display_h) / 5;
    if (btn_w <= 0 || btn_h <= 0) return -1;

    int col = tx / btn_w;
    int row = grid_y / btn_h;
    if (col > 3) col = 3;
    if (row > 4) row = 4;

    /* row 0: C  +/-  %  / */
    /* row 1: 7  8    9  * */
    /* row 2: 4  5    6  - */
    /* row 3: 1  2    3  + */
    /* row 4: 0  0    .  = */
    static const int grid[5][4] = {
        {16, 18, 17,  14},
        { 7,  8,  9,  13},
        { 4,  5,  6,  12},
        { 1,  2,  3,  11},
        { 0,  0, 10,  15},
    };
    return grid[row][col];
}

void lumo_app_render_calculator(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;

    lumo_app_draw_background(pixels, width, height);

    /* display area */
    int display_h = h / 4;
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, display_h,
        theme.header_bg);

    /* show current value from ctx */
    const char *display = ctx != NULL && ctx->term_input_len > 0
        ? ctx->term_input : "0";
    int text_scale = 4;
    int tw = (int)strlen(display) * text_scale * 6;
    lumo_app_draw_text(pixels, width, height,
        w - tw - 20, display_h / 2 - 12, text_scale, theme.text, display);

    /* button grid */
    int btn_w = w / 4;
    int btn_h = (h - display_h) / 5;
    int pad = 3;

    static const char *labels[5][4] = {
        {"C", "+/-", "<", "/"},
        {"7", "8", "9", "x"},
        {"4", "5", "6", "-"},
        {"1", "2", "3", "+"},
        {"0", "0", ".", "="},
    };

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            if (row == 4 && col == 1) continue; /* skip second 0 cell */

            int bx = col * btn_w + pad;
            int by = display_h + row * btn_h + pad;
            int bw = btn_w - pad * 2;
            int bh = btn_h - pad * 2;

            /* wide zero button */
            if (row == 4 && col == 0) bw = btn_w * 2 - pad * 2;

            uint32_t bg, fg;
            if (row == 0) {
                bg = theme.card_bg;
                fg = theme.accent;
            } else if (col == 3) {
                bg = theme.accent;
                fg = theme.text;
            } else {
                bg = theme.card_bg;
                fg = theme.text;
            }

            struct lumo_rect r = {bx, by, bw, bh};
            lumo_app_fill_rounded_rect(pixels, width, height, &r, 12, bg);
            lumo_app_draw_text_centered(pixels, width, height, &r, 3,
                fg, labels[row][col]);
        }
    }
}
