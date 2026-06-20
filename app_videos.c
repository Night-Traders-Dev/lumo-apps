#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void lumo_app_render_videos(
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
        "VIDEOS");
    lumo_app_fill_rect(pixels, width, height, 12, 48, (int)width - 24, 1,
        theme.separator);

    /* now playing preview area */
    if (selected >= 0 && selected < count) {
        int preview_h = (int)height / 3;
        struct lumo_rect preview = {12, 56, (int)width - 24, preview_h};
        lumo_app_fill_rounded_rect(pixels, width, height, &preview, 14,
            lumo_app_argb(0xFF, 0x0A, 0x0A, 0x0E));

        /* play button triangle in center */
        if (!playing) {
            int cx = preview.x + preview.width / 2;
            int cy = preview.y + preview.height / 2;
            int sz = 24;
            for (int dy = -sz; dy <= sz; dy++) {
                int row_w = sz - (dy < 0 ? -dy : dy);
                int py = cy + dy;
                if (py < 0 || py >= (int)height) continue;
                for (int dx = -4; dx < row_w; dx++) {
                    int px = cx + dx;
                    if (px >= 0 && px < (int)width) {
                        pixels[py * (int)width + px] = theme.accent;
                    }
                }
            }
        } else {
            /* playing indicator */
            lumo_app_draw_text(pixels, width, height,
                preview.x + preview.width / 2 - 36,
                preview.y + preview.height / 2 - 7, 2,
                theme.accent, "> PLAYING");
        }

        /* filename below preview */
        lumo_app_draw_text(pixels, width, height, 20,
            preview.y + preview.height + 8, 2, theme.text,
            ctx->media_files[selected]);

        /* progress bar */
        if (playing) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int progress = (int)(ts.tv_sec % 120) *
                ((int)width - 48) / 120;
            struct lumo_rect bar = {24,
                preview.y + preview.height + 28,
                (int)width - 48, 4};
            struct lumo_rect fill = {24,
                preview.y + preview.height + 28,
                progress, 4};
            lumo_app_fill_rounded_rect(pixels, width, height, &bar, 2,
                theme.separator);
            lumo_app_fill_rounded_rect(pixels, width, height, &fill, 2,
                theme.accent);
        }
    }

    /* file list */
    int list_start = (selected >= 0) ? 56 + (int)height / 3 + 44 : 56;
    int list_y = list_start;
    lumo_app_draw_text(pixels, width, height, 16, list_y, 2,
        theme.text_dim, "LIBRARY");
    list_y += 20;
    lumo_app_fill_rect(pixels, width, height, 12, list_y, (int)width - 24,
        1, theme.separator);
    list_y += 8;

    if (count == 0) {
        lumo_app_draw_text(pixels, width, height, 16, list_y + 20, 2,
            theme.text_dim, "NO VIDEO FILES FOUND");
        lumo_app_draw_text(pixels, width, height, 16, list_y + 42, 2,
            theme.text_dim, "PLACE .MP4 OR .MKV IN ~/VIDEOS");
    } else {
        int row_h = 40;
        for (int i = scroll; i < count && list_y + row_h < (int)height; i++) {
            bool is_sel = (i == selected);
            struct lumo_rect row = {12, list_y, (int)width - 24, row_h - 4};
            if (is_sel) {
                lumo_app_fill_rounded_rect(pixels, width, height, &row, 8,
                    theme.card_bg);
            }

            /* play icon */
            lumo_app_draw_text(pixels, width, height, 20, list_y + 10, 2,
                is_sel ? theme.accent : theme.text_dim, ">");

            /* filename */
            lumo_app_draw_text(pixels, width, height, 44, list_y + 10, 2,
                is_sel ? theme.text : theme.text_dim,
                ctx->media_files[i]);
            list_y += row_h;
        }
    }
}
