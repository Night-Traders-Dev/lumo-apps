/*
 * app_tasks.c — Tasks/To-Do app for Lumo OS.
 * Simple checklist with add, toggle, delete. Persists to ~/.lumo-tasks.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* hit-test: 0=add task, 100+row=toggle task, 200+row=delete task, -1=nothing */
int lumo_app_tasks_button_at(uint32_t width, uint32_t height,
    double x, double y)
{
    int w = (int)width, h = (int)height;
    int tx = (int)x, ty = (int)y;

    /* add button */
    if (ty < 48 && tx > w - 60) return 0;

    /* task rows */
    if (ty >= 60 && ty < h - 10) {
        int row = (ty - 60) / 52;
        /* checkbox area (left 48px) vs delete (right 48px) */
        if (tx < 56) return 100 + row;  /* toggle */
        if (tx > w - 56) return 200 + row;  /* delete */
        return 100 + row;  /* toggle on body tap too */
    }
    return -1;
}

void lumo_app_render_tasks(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;
    int count = ctx != NULL ? ctx->note_count : 0;
    int editing = ctx != NULL ? ctx->note_editing : -1;

    lumo_app_draw_background(pixels, width, height);

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 48, theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 14, 3, theme.accent,
        "TASKS");

    /* count badge */
    {
        char badge[16];
        snprintf(badge, sizeof(badge), "%d", count);
        lumo_app_draw_text(pixels, width, height, w - 100, 16, 2,
            theme.text_dim, badge);
    }

    /* add button */
    {
        struct lumo_rect btn = {w - 52, 8, 44, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            theme.accent);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 3,
            theme.text, "+");
    }
    lumo_app_fill_rect(pixels, width, height, 8, 48, w - 16, 1,
        theme.separator);

    int y = 60;
    int pad = 12;

    if (count == 0) {
        lumo_app_draw_text(pixels, width, height, pad, y + 20, 2,
            theme.text_dim, "ALL CLEAR!");
        lumo_app_draw_text(pixels, width, height, pad, y + 44, 2,
            theme.text_dim, "TAP + TO ADD A TASK");
        return;
    }

    for (int i = 0; i < count && y + 52 < h; i++) {
        struct lumo_rect row = {pad, y, w - pad * 2, 44};
        lumo_app_fill_rounded_rect(pixels, width, height, &row, 10,
            theme.card_bg);

        /* checkbox — check if task starts with [x] or [ ] */
        bool done = (ctx->notes[i][0] == '[' && ctx->notes[i][1] == 'x');
        struct lumo_rect check = {pad + 6, y + 8, 28, 28};
        lumo_app_fill_rounded_rect(pixels, width, height, &check, 6,
            done ? theme.accent : theme.bg);
        lumo_app_draw_outline(pixels, width, height, &check, 1,
            theme.card_stroke);
        if (done) {
            lumo_app_draw_text_centered(pixels, width, height, &check, 2,
                theme.text, "v");
        }

        /* task text (skip [x] or [ ] prefix) */
        const char *text = ctx->notes[i];
        if (text[0] == '[' && (text[1] == 'x' || text[1] == ' ') &&
                text[2] == ']')
            text += 3;
        while (*text == ' ') text++;

        uint32_t text_color = done ? theme.text_dim : theme.text;
        lumo_app_draw_text(pixels, width, height,
            pad + 44, y + 14, 2, text_color,
            text[0] ? text : "UNTITLED");

        /* editing cursor */
        if (editing == i) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if ((ts.tv_nsec / 500000000) == 0) {
                int tw = (int)strlen(text) * 12;
                lumo_app_fill_rect(pixels, width, height,
                    pad + 44 + tw, y + 12, 2, 20, theme.accent);
            }
        }

        /* delete X */
        lumo_app_draw_text(pixels, width, height,
            w - pad - 24, y + 14, 2, theme.text_dim, "X");

        y += 52;
    }
}
