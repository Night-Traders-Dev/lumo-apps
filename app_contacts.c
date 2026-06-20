/*
 * app_contacts.c — Contacts app for Lumo OS.
 * Standalone contact list with add/edit/delete. Data shared with Phone app.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* hit-test: 0=add button, 100+row=contact row, -1=nothing */
int lumo_app_contacts_button_at(uint32_t width, uint32_t height,
    double x, double y)
{
    int w = (int)width, h = (int)height;
    int tx = (int)x, ty = (int)y;

    /* add button top-right */
    if (ty < 48 && tx > w - 60) return 0;

    /* contact rows */
    if (ty >= 60 && ty < h - 10) {
        int row = (ty - 60) / 56;
        return 100 + row;
    }
    return -1;
}

void lumo_app_render_contacts(
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
        "CONTACTS");

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
            theme.text_dim, "NO CONTACTS YET");
        lumo_app_draw_text(pixels, width, height, pad, y + 44, 2,
            theme.text_dim, "TAP + TO ADD");
        return;
    }

    for (int i = 0; i < count && y + 56 < h; i++) {
        struct lumo_rect row = {pad, y, w - pad * 2, 48};
        bool selected = (ctx != NULL && ctx->selected_row == i);

        lumo_app_fill_rounded_rect(pixels, width, height, &row, 10,
            selected ? theme.accent : theme.card_bg);
        lumo_app_draw_outline(pixels, width, height, &row, 1,
            theme.card_stroke);

        /* avatar circle */
        char initial[2] = {0};
        if (ctx->notes[i][0] >= 'A' && ctx->notes[i][0] <= 'z')
            initial[0] = ctx->notes[i][0] & ~0x20; /* uppercase */
        else
            initial[0] = '#';
        struct lumo_rect avatar = {pad + 6, y + 8, 32, 32};
        lumo_app_fill_rounded_rect(pixels, width, height, &avatar, 16,
            selected ? theme.bg : theme.accent);
        lumo_app_draw_text_centered(pixels, width, height, &avatar, 2,
            selected ? theme.accent : theme.text, initial);

        /* name */
        const char *name = ctx->notes[i][0] ? ctx->notes[i] : "UNNAMED";
        lumo_app_draw_text(pixels, width, height, pad + 48, y + 16, 2,
            selected ? theme.bg : theme.text, name);

        /* editing indicator */
        if (editing == i) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if ((ts.tv_nsec / 500000000) == 0) {
                int cw = (int)strlen(name) * 12;
                lumo_app_fill_rect(pixels, width, height,
                    pad + 48 + cw, y + 14, 2, 20, theme.accent);
            }
        }
        y += 56;
    }
}
