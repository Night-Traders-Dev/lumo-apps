/*
 * app_calendar.c — Calendar app for Lumo OS.
 * Month view with day grid, current date highlight, and basic navigation.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* hit-test: 1=prev month, 2=next month, 100+day=day tap, -1=nothing */
int lumo_app_calendar_button_at(uint32_t width, uint32_t height,
    double x, double y)
{
    int w = (int)width;
    int ty = (int)y, tx = (int)x;
    (void)height;

    /* nav buttons in header */
    if (ty >= 8 && ty < 48) {
        if (tx < 60) return 1;        /* prev */
        if (tx > w - 60) return 2;    /* next */
    }

    /* day grid starts at y=100, 7 columns, 6 rows */
    if (ty >= 100) {
        int cell_w = w / 7;
        int cell_h = 48;
        int col = tx / cell_w;
        int row = (ty - 100) / cell_h;
        if (col >= 0 && col < 7 && row >= 0 && row < 6)
            return 100 + row * 7 + col;
    }
    return -1;
}

void lumo_app_render_calendar(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;
    (void)ctx;

    lumo_app_draw_background(pixels, width, height);

    /* current time */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;  /* 0-11 */
    int today = tm->tm_mday;

    /* month offset from scroll_offset */
    int offset = ctx != NULL ? ctx->scroll_offset : 0;
    month += offset;
    while (month < 0) { month += 12; year--; }
    while (month > 11) { month -= 12; year++; }
    bool is_current_month = (offset == 0);

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 52, theme.header_bg);
    static const char *months[] = {
        "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
        "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
    };
    char title[64];
    snprintf(title, sizeof(title), "%s %d", months[month], year);
    int tw = (int)strlen(title) * 3 * 6;
    lumo_app_draw_text(pixels, width, height, (w - tw) / 2, 14, 3,
        theme.text, title);

    /* nav buttons */
    {
        struct lumo_rect prev = {8, 8, 44, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &prev, 8,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &prev, 3,
            theme.accent, "<");
    }
    {
        struct lumo_rect next = {w - 52, 8, 44, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &next, 8,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &next, 3,
            theme.accent, ">");
    }

    /* day-of-week header */
    static const char *dow[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    int cell_w = w / 7;
    for (int i = 0; i < 7; i++) {
        struct lumo_rect dr = {i * cell_w, 60, cell_w, 28};
        uint32_t color = (i == 0 || i == 6) ? theme.text_dim : theme.text;
        lumo_app_draw_text_centered(pixels, width, height, &dr, 2,
            color, dow[i]);
    }
    lumo_app_fill_rect(pixels, width, height, 8, 92, w - 16, 1,
        theme.separator);

    /* compute first day of month */
    struct tm first = {0};
    first.tm_year = year - 1900;
    first.tm_mon = month;
    first.tm_mday = 1;
    mktime(&first);
    int start_dow = first.tm_wday; /* 0=sun */

    /* days in month */
    int days_in_month;
    {
        struct tm test = {0};
        test.tm_year = year - 1900;
        test.tm_mon = month + 1;
        test.tm_mday = 0;
        mktime(&test);
        days_in_month = test.tm_mday;
    }

    /* draw day grid */
    int cell_h = 48;
    if (100 + 6 * cell_h > h) cell_h = (h - 100) / 6;
    int day = 1;
    for (int row = 0; row < 6 && day <= days_in_month; row++) {
        for (int col = 0; col < 7 && day <= days_in_month; col++) {
            if (row == 0 && col < start_dow) continue;

            int cx = col * cell_w;
            int cy = 100 + row * cell_h;
            struct lumo_rect cell = {cx + 2, cy + 2, cell_w - 4, cell_h - 4};

            bool is_today = is_current_month && day == today;
            if (is_today) {
                lumo_app_fill_rounded_rect(pixels, width, height, &cell,
                    10, theme.accent);
            }

            char label[4];
            snprintf(label, sizeof(label), "%d", day);
            lumo_app_draw_text_centered(pixels, width, height, &cell, 2,
                is_today ? theme.bg : theme.text, label);
            day++;
        }
    }
}
