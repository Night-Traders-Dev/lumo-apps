#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void lumo_app_render_music(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    int count = ctx != NULL ? ctx->media_file_count : 0;
    int selected = ctx != NULL ? ctx->media_selected : -1;
    bool playing = ctx != NULL ? ctx->media_playing : false;
    int scroll = ctx != NULL ? ctx->scroll_offset : 0;

    memset(pixels, 0, (size_t)width * height * 4);
    lumo_app_draw_background(pixels, width, height);

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, (int)width, 48,
        theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 16, 3, theme.accent,
        "MUSIC");
    lumo_app_fill_rect(pixels, width, height, 12, 48, (int)width - 24, 1,
        theme.separator);

    /* now playing bar */
    if (selected >= 0 && selected < count) {
        struct lumo_rect np = {12, 56, (int)width - 24, 48};
        lumo_app_fill_rounded_rect(pixels, width, height, &np, 12,
            theme.card_bg);
        lumo_app_draw_outline(pixels, width, height, &np, 1,
            theme.card_stroke);

        lumo_app_draw_text(pixels, width, height, 24, 64, 2, theme.text,
            ctx->media_files[selected]);

        /* play/pause indicator */
        const char *status = playing ? "> PLAYING" : "|| PAUSED";
        lumo_app_draw_text(pixels, width, height, 24, 84, 2,
            playing ? theme.accent : theme.text_dim, status);

        /* progress bar */
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int progress = (int)(ts.tv_sec % 60) * ((int)width - 80) / 60;
            struct lumo_rect bar_bg = {24, 96, (int)width - 48, 4};
            struct lumo_rect bar_fill = {24, 96, progress, 4};
            lumo_app_fill_rounded_rect(pixels, width, height, &bar_bg, 2,
                theme.separator);
            if (playing) {
                lumo_app_fill_rounded_rect(pixels, width, height, &bar_fill,
                    2, theme.accent);
            }
        }
    } else {
        lumo_app_draw_text(pixels, width, height, 16, 68, 2,
            theme.text_dim, "NO TRACK SELECTED");
    }

    /* file list */
    int list_y = 116;
    lumo_app_draw_text(pixels, width, height, 16, list_y, 2,
        theme.text_dim, "LIBRARY");
    list_y += 20;
    lumo_app_fill_rect(pixels, width, height, 12, list_y, (int)width - 24,
        1, theme.separator);
    list_y += 8;

    if (count == 0) {
        lumo_app_draw_text(pixels, width, height, 16, list_y + 20, 2,
            theme.text_dim, "NO AUDIO FILES FOUND");
        lumo_app_draw_text(pixels, width, height, 16, list_y + 42, 2,
            theme.text_dim, "PLACE .WAV OR .MP3 IN ~/MUSIC");
    } else {
        int row_h = 36;
        for (int i = scroll; i < count && list_y + row_h < (int)height; i++) {
            bool is_sel = (i == selected);
            struct lumo_rect row = {12, list_y, (int)width - 24, row_h - 4};
            if (is_sel) {
                lumo_app_fill_rounded_rect(pixels, width, height, &row, 8,
                    theme.card_bg);
            }
            /* track number */
            char num[8];
            snprintf(num, sizeof(num), "%02d", i + 1);
            lumo_app_draw_text(pixels, width, height, 20, list_y + 8, 2,
                is_sel ? theme.accent : theme.text_dim, num);
            /* filename */
            lumo_app_draw_text(pixels, width, height, 56, list_y + 8, 2,
                is_sel ? theme.text : theme.text_dim,
                ctx->media_files[i]);
            list_y += row_h;
        }
    }
}
