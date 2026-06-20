#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>

/* scale-blit source image into the exact destination rectangle */
static void blit_scaled(uint32_t *dst, uint32_t dw, uint32_t dh,
    int dx, int dy, int dstw, int dsth,
    const uint32_t *src, uint32_t sw, uint32_t sh)
{
    if (!dst || !src || dstw <= 0 || dsth <= 0 || sw == 0 || sh == 0) return;
    for (int y = 0; y < dsth; y++) {
        int py = dy + y;
        if (py < 0 || py >= (int)dh) continue;
        uint32_t sy = (uint32_t)y * sh / (uint32_t)dsth;
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dstw; x++) {
            int px = dx + x;
            if (px < 0 || px >= (int)dw) continue;
            uint32_t sx = (uint32_t)x * sw / (uint32_t)dstw;
            if (sx >= sw) sx = sw - 1;
            dst[py * dw + px] = src[sy * sw + sx];
        }
    }
}

static void blit_scaled_fit(uint32_t *dst, uint32_t dw, uint32_t dh,
    int dx, int dy, int dstw, int dsth,
    const uint32_t *src, uint32_t sw, uint32_t sh)
{
    int draw_w;
    int draw_h;
    int draw_x;
    int draw_y;

    if (!dst || !src || dstw <= 0 || dsth <= 0 || sw == 0 || sh == 0) return;

    draw_w = dstw;
    draw_h = (int)((int64_t)dstw * (int64_t)sh / (int64_t)sw);
    if (draw_h > dsth) {
        draw_h = dsth;
        draw_w = (int)((int64_t)dsth * (int64_t)sw / (int64_t)sh);
    }
    if (draw_w <= 0 || draw_h <= 0) return;

    draw_x = dx + (dstw - draw_w) / 2;
    draw_y = dy + (dsth - draw_h) / 2;
    blit_scaled(dst, dw, dh, draw_x, draw_y, draw_w, draw_h, src, sw, sh);
}

void lumo_app_render_photos(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    int count = ctx != NULL ? ctx->media_file_count : 0;
    int selected = ctx != NULL ? ctx->media_selected : -1;
    int scroll = ctx != NULL ? ctx->scroll_offset : 0;
    bool viewing = ctx != NULL ? ctx->photo_viewing : false;

    memset(pixels, 0, (size_t)width * height * 4);
    lumo_app_draw_background(pixels, width, height);

    /* fullscreen photo viewer */
    if (viewing && ctx != NULL && ctx->photo_pixels != NULL &&
            ctx->photo_width > 0 && ctx->photo_height > 0) {
        /* black background */
        lumo_app_fill_rect(pixels, width, height, 0, 0,
            (int)width, (int)height, 0xFF000000);

        /* aspect-fit the image */
        float scale_x = (float)width / (float)ctx->photo_width;
        float scale_y = (float)height / (float)ctx->photo_height;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        int img_w = (int)(ctx->photo_width * scale);
        int img_h = (int)(ctx->photo_height * scale);
        int img_x = ((int)width - img_w) / 2;
        int img_y = ((int)height - img_h) / 2;

        blit_scaled(pixels, width, height, img_x, img_y, img_w, img_h,
            ctx->photo_pixels, ctx->photo_width, ctx->photo_height);

        /* filename overlay at bottom */
        if (selected >= 0 && selected < count) {
            lumo_app_fill_rect(pixels, width, height, 0,
                (int)height - 32, (int)width, 32,
                lumo_app_argb(0xA0, 0x00, 0x00, 0x00));
            lumo_app_draw_text(pixels, width, height, 12,
                (int)height - 24, 2, theme.text,
                ctx->media_files[selected]);
            lumo_app_draw_text(pixels, width, height,
                (int)width - 100, (int)height - 24, 2,
                theme.text_dim, "TAP TO EXIT");
        }
        return;
    }

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, (int)width, 48,
        theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 16, 3, theme.accent,
        "PHOTOS");
    lumo_app_fill_rect(pixels, width, height, 12, 48, (int)width - 24, 1,
        theme.separator);

    if (count == 0) {
        lumo_app_draw_text(pixels, width, height, 16, 80, 2,
            theme.text_dim, "NO IMAGES FOUND");
        lumo_app_draw_text(pixels, width, height, 16, 102, 2,
            theme.text_dim, "PLACE IMAGES IN ~/PICTURES");
        return;
    }

    /* photo grid - 3 columns */
    int cols = 3;
    int pad = 8;
    int cell_w = ((int)width - pad * (cols + 1)) / cols;
    int cell_h = cell_w * 3 / 4;
    int grid_y = 56;

    for (int i = scroll; i < count; i++) {
        int col = (i - scroll) % cols;
        int row = (i - scroll) / cols;
        int cx = pad + col * (cell_w + pad);
        int cy = grid_y + row * (cell_h + pad);

        if (cy + cell_h > (int)height - 4) break;

        bool is_sel = (i == selected);
        struct lumo_rect cell = {cx, cy, cell_w, cell_h};
        struct lumo_rect image_rect = {cx + 2, cy + 2, cell_w - 4, cell_h - 22};
        const uint32_t *thumbnail = ctx != NULL ? ctx->photo_thumbnails[i] : NULL;
        uint32_t thumbnail_width =
            ctx != NULL ? ctx->photo_thumbnail_widths[i] : 0;
        uint32_t thumbnail_height =
            ctx != NULL ? ctx->photo_thumbnail_heights[i] : 0;

        lumo_app_fill_rounded_rect(pixels, width, height, &cell, 10, theme.card_bg);
        if (thumbnail != NULL && thumbnail_width > 0 && thumbnail_height > 0 &&
                image_rect.width > 0 && image_rect.height > 0) {
            blit_scaled_fit(pixels, width, height, image_rect.x, image_rect.y,
                image_rect.width, image_rect.height, thumbnail,
                thumbnail_width, thumbnail_height);
        } else {
            uint32_t hash = 0;
            for (int j = 0; ctx->media_files[i][j]; j++) {
                hash = hash * 31 + (uint32_t)ctx->media_files[i][j];
            }
            uint32_t thumb_r = 0x20 + (hash & 0x3F);
            uint32_t thumb_g = 0x18 + ((hash >> 6) & 0x3F);
            uint32_t thumb_b = 0x28 + ((hash >> 12) & 0x3F);

            lumo_app_fill_rounded_rect(pixels, width, height, &image_rect, 8,
                lumo_app_argb(0xFF, (uint8_t)thumb_r, (uint8_t)thumb_g,
                    (uint8_t)thumb_b));
        }

        lumo_app_fill_rect(pixels, width, height, cx, cy + cell_h - 18,
            cell_w, 18, 0xE0101014u);

        if (is_sel) {
            lumo_app_draw_outline(pixels, width, height, &cell, 2,
                theme.accent);
            /* hint: tap again to view */
            lumo_app_draw_text(pixels, width, height, cx + 4,
                cy + 4, 1, theme.text, "TAP TO VIEW");
        }

        /* filename label */
        char label[18];
        strncpy(label, ctx->media_files[i], sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
        if (strlen(label) > 14) {
            label[12] = '.';
            label[13] = '.';
            label[14] = '\0';
        }
        lumo_app_draw_text(pixels, width, height, cx + 4,
            cy + cell_h - 14, 1, theme.text, label);
    }

    /* scroll hint */
    if (count > cols * 3) {
        char scroll_text[32];
        snprintf(scroll_text, sizeof(scroll_text), "SCROLL %d/%d",
            scroll / cols + 1, (count + cols - 1) / cols);
        lumo_app_draw_text(pixels, width, height,
            (int)width - 100, (int)height - 16, 1,
            theme.text_dim, scroll_text);
    }
}
