#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

int lumo_app_maps_button_at(
    uint32_t width, uint32_t height, double x, double y, int tab
) {
    if (y < 80) {
        if (x < (double)width / 3.0) return 1;
        if (x < 2.0 * (double)width / 3.0) return 2;
        return 3;
    }
    if (tab == 1) {
        /* Places tab */
        /* "+ADD PLACE" button at the bottom: centered, 200px width */
        if (y >= (double)height - 120.0 && y <= (double)height - 60.0 &&
                x >= 20.0 && x <= (double)width - 20.0) {
            return 0;
        }
        /* Places rows */
        if (y >= 100.0 && y < 100.0 + 8.0 * 50.0) {
            int row = (int)((y - 100.0) / 50.0);
            return 100 + row;
        }
    }
    return -1;
}

void lumo_app_render_maps(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;

    /* Draw background */
    lumo_app_draw_background(pixels, width, height);

    /* Draw Header/Tabs */
    int tab = (ctx != NULL) ? ctx->clock_tab : 0;
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 80, theme.header_bg);
    lumo_app_fill_rect(pixels, width, height, 0, 79, w, 1, theme.separator);

    /* Draw tabs text */
    struct lumo_rect tab1 = {0, 0, w / 3, 80};
    struct lumo_rect tab2 = {w / 3, 0, w / 3, 80};
    struct lumo_rect tab3 = {2 * w / 3, 0, w - 2 * w / 3, 80};

    uint32_t c1 = (tab == 0) ? theme.accent : theme.text_dim;
    uint32_t c2 = (tab == 1) ? theme.accent : theme.text_dim;
    uint32_t c3 = (tab == 2) ? theme.accent : theme.text_dim;

    lumo_app_draw_text_centered(pixels, width, height, &tab1, 2, c1, "COMPASS");
    lumo_app_draw_text_centered(pixels, width, height, &tab2, 2, c2, "PLACES");
    lumo_app_draw_text_centered(pixels, width, height, &tab3, 2, c3, "INFO");

    /* Draw separator under active tab */
    if (tab == 0) {
        lumo_app_fill_rect(pixels, width, height, 0, 76, w / 3, 4, theme.accent);
    } else if (tab == 1) {
        lumo_app_fill_rect(pixels, width, height, w / 3, 76, w / 3, 4, theme.accent);
    } else {
        lumo_app_fill_rect(pixels, width, height, 2 * w / 3, 76, w - 2 * w / 3, 4, theme.accent);
    }

    if (tab == 0) {
        /* COMPASS tab: draw a mock compass or coordinates */
        struct lumo_rect compass_rect = {w / 2 - 100, h / 2 - 100, 200, 200};
        lumo_app_fill_rounded_rect(pixels, width, height, &compass_rect, 100, theme.card_bg);
        lumo_app_draw_outline(pixels, width, height, &compass_rect, 4, theme.accent);

        /* compass needle placeholder */
        struct lumo_rect needle_n = {w / 2 - 10, h / 2 - 80, 20, 80};
        struct lumo_rect needle_s = {w / 2 - 10, h / 2, 20, 80};
        lumo_app_fill_rounded_rect(pixels, width, height, &needle_n, 8, theme.accent);
        lumo_app_fill_rounded_rect(pixels, width, height, &needle_s, 8, theme.card_stroke);

        lumo_app_draw_text(pixels, width, height, w / 2 - 8, h / 2 - 70, 3, theme.text, "N");
        lumo_app_draw_text(pixels, width, height, w / 2 - 8, h / 2 + 40, 3, theme.text, "S");

        struct lumo_rect label_rect = {0, h - 80, w, 40};
        lumo_app_draw_text_centered(pixels, width, height, &label_rect, 2, theme.text_dim, "GPS LOCKED - 37.7749 N, 122.4194 W");
    } else if (tab == 1) {
        /* PLACES tab: draw places list */
        int note_count = (ctx != NULL) ? ctx->note_count : 0;
        int note_editing = (ctx != NULL) ? ctx->note_editing : -1;
        int selected_row = (ctx != NULL) ? ctx->selected_row : -1;

        for (int i = 0; i < 8; i++) {
            int row_y = 100 + i * 50;
            if (i < note_count) {
                struct lumo_rect row_rect = {12, row_y, w - 24, 44};
                uint32_t bg = (selected_row == i) ? theme.accent : theme.card_bg;
                uint32_t fg = (selected_row == i) ? theme.bg : theme.text;
                lumo_app_fill_rounded_rect(pixels, width, height, &row_rect, 8, bg);
                
                const char *place_name = (ctx->notes[i][0] != '\0') ? ctx->notes[i] : "(Unnamed Place)";
                
                char name_buf[128];
                snprintf(name_buf, sizeof(name_buf), "%d. %s", i + 1, place_name);
                if (note_editing == i) {
                    strncat(name_buf, "|", sizeof(name_buf) - strlen(name_buf) - 1);
                }

                lumo_app_draw_text(pixels, width, height, 24, row_y + 14, 2, fg, name_buf);
            } else {
                /* empty slot outline */
                struct lumo_rect row_rect = {12, row_y, w - 24, 44};
                lumo_app_draw_outline(pixels, width, height, &row_rect, 1, theme.card_stroke);
            }
        }

        /* "+ADD PLACE" button at the bottom */
        struct lumo_rect add_btn = {12, h - 110, w - 24, 50};
        lumo_app_fill_rounded_rect(pixels, width, height, &add_btn, 12, theme.accent);
        lumo_app_draw_text_centered(pixels, width, height, &add_btn, 2, theme.text, "+ ADD PLACE");
    } else {
        /* INFO tab */
        struct lumo_rect card = {20, 100, w - 40, h - 160};
        lumo_app_fill_rounded_rect(pixels, width, height, &card, 16, theme.card_bg);
        lumo_app_draw_outline(pixels, width, height, &card, 2, theme.card_stroke);

        lumo_app_draw_text(pixels, width, height, 40, 130, 3, theme.accent, "LUMO MAPS");
        lumo_app_draw_text(pixels, width, height, 40, 180, 2, theme.text, "Version: 1.0.0-beta");
        lumo_app_draw_text(pixels, width, height, 40, 220, 2, theme.text, "Source: OpenStreetMap mock");
        lumo_app_draw_text(pixels, width, height, 40, 260, 2, theme.text, "Vector Render: Software Pixman");
        lumo_app_draw_text(pixels, width, height, 40, 300, 2, theme.text, "Target: RISC-V 64-bit SpacemiT K1");
    }
}
