/*
 * app_downloads.c — Downloads app for Lumo OS.
 * Shows ~/Downloads directory with file sizes and types.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>

void lumo_app_render_downloads(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;
    int count = ctx != NULL ? ctx->media_file_count : 0;
    int scroll = ctx != NULL ? ctx->scroll_offset : 0;

    lumo_app_draw_background(pixels, width, height);

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 48, theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 14, 3, theme.accent,
        "DOWNLOADS");
    {
        char badge[16];
        snprintf(badge, sizeof(badge), "%d FILES", count);
        int tw = (int)strlen(badge) * 1 * 6;
        lumo_app_draw_text(pixels, width, height, w - tw - 16, 18, 1,
            theme.text_dim, badge);
    }
    lumo_app_fill_rect(pixels, width, height, 8, 48, w - 16, 1,
        theme.separator);

    int y = 56;
    int pad = 12;
    int row_h = 48;

    if (count == 0) {
        lumo_app_draw_text(pixels, width, height, pad, y + 20, 2,
            theme.text_dim, "NO DOWNLOADS");
        lumo_app_draw_text(pixels, width, height, pad, y + 44, 2,
            theme.text_dim, "FILES SAVED HERE WILL APPEAR");
        return;
    }

    int max_visible = (h - y - 10) / row_h;
    for (int i = scroll; i < count && i < scroll + max_visible; i++) {
        struct lumo_rect row = {pad, y, w - pad * 2, row_h - 4};
        lumo_app_fill_rounded_rect(pixels, width, height, &row, 8,
            theme.card_bg);

        /* file icon (colored rectangle based on extension) */
        const char *name = ctx->media_files[i];
        const char *ext = strrchr(name, '.');
        uint32_t icon_color = theme.accent;
        if (ext) {
            if (strcmp(ext, ".pdf") == 0) icon_color = lumo_app_argb(0xFF, 0xFF, 0x44, 0x44);
            else if (strcmp(ext, ".zip") == 0 || strcmp(ext, ".tar") == 0 ||
                    strcmp(ext, ".gz") == 0) icon_color = lumo_app_argb(0xFF, 0xFF, 0xAA, 0x44);
            else if (strcmp(ext, ".deb") == 0) icon_color = lumo_app_argb(0xFF, 0x44, 0xCC, 0x44);
            else if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0)
                icon_color = lumo_app_argb(0xFF, 0x44, 0xAA, 0xFF);
        }
        struct lumo_rect icon = {pad + 6, y + 8, 28, row_h - 20};
        lumo_app_fill_rounded_rect(pixels, width, height, &icon, 4,
            icon_color);

        /* filename */
        lumo_app_draw_text(pixels, width, height,
            pad + 42, y + 14, 2, theme.text, name);
        y += row_h;
    }
}
