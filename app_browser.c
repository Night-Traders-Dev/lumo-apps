#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Native SHM browser app — instant toolbar response.
 * Web content is launched via system browser (epiphany/lumo-browser)
 * as a subprocess when the user enters a URL. */

#define BROWSER_MAX_BOOKMARKS 8
#define BROWSER_MAX_HISTORY 8

/* hit-test returns:
 * 0 = URL bar tap (toggle editing)
 * 1 = back button
 * 2 = forward button
 * 3 = reload / go button
 * 4 = bookmark star
 * 5 = new tab / home
 * 100+row = bookmark row
 * -1 = nothing */
int lumo_app_browser_button_at(
    uint32_t width, uint32_t height, double x, double y
) {
    int w = (int)width;
    int h = (int)height;
    int tx = (int)x;
    int ty = (int)y;

    /* top toolbar: y 0..48 */
    if (ty < 48) {
        /* back: 0..44 */
        if (tx < 44) return 1;
        /* forward: 44..88 */
        if (tx < 88) return 2;
        /* reload/go: 88..132 */
        if (tx < 132) return 3;
        /* URL bar: 132..w-88 */
        if (tx < w - 88) return 0;
        /* bookmark: w-88..w-44 */
        if (tx < w - 44) return 4;
        /* home/+: w-44..w */
        return 5;
    }

    /* bookmark/history rows below toolbar */
    if (ty >= 60 && ty < h - 10) {
        int row = (ty - 60) / 44;
        return 100 + row;
    }

    return -1;
}

void lumo_app_render_browser(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    int w = (int)width;
    int h = (int)height;
    int editing = ctx != NULL ? ctx->note_editing : -1;
    int bm_count = ctx != NULL ? ctx->note_count : 0;
    const char *url_text = (ctx != NULL && ctx->term_input_len > 0)
        ? ctx->term_input : "";
    int url_len = ctx != NULL ? ctx->term_input_len : 0;

    /* lumo_app_draw_background fills every pixel — skip redundant memset */
    lumo_app_draw_background(pixels, width, height);

    /* ── Top toolbar ── */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 48, theme.header_bg);

    /* back button */
    {
        struct lumo_rect btn = {4, 6, 36, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 2,
            theme.text_dim, "<");
    }
    /* forward button */
    {
        struct lumo_rect btn = {48, 6, 36, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 2,
            theme.text_dim, ">");
    }
    /* go/reload button */
    {
        struct lumo_rect btn = {92, 6, 36, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 2,
            url_len > 0 ? theme.accent : theme.text_dim,
            url_len > 0 ? "GO" : "R");
    }

    /* URL bar */
    {
        struct lumo_rect bar = {136, 6, w - 136 - 92, 36};
        uint32_t bar_bg = editing >= 0 ? theme.card_bg : theme.bg;
        uint32_t bar_border = editing >= 0 ? theme.accent : theme.card_stroke;
        lumo_app_fill_rounded_rect(pixels, width, height, &bar, 18, bar_bg);
        lumo_app_draw_outline(pixels, width, height, &bar, editing >= 0 ? 2 : 1,
            bar_border);

        if (url_len > 0) {
            int max_chars = (bar.width - 24) / 12;
            char display[128];
            if (url_len > max_chars) {
                snprintf(display, sizeof(display), "%.*s...",
                    max_chars - 3, url_text);
            } else {
                snprintf(display, sizeof(display), "%s", url_text);
            }
            lumo_app_draw_text(pixels, width, height, bar.x + 12, bar.y + 10,
                2, theme.text, display);

            /* blinking cursor when editing */
            if (editing >= 0) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                if ((ts.tv_nsec / 500000000) == 0) {
                    int cw = url_len * 12;
                    if (cw > bar.width - 28) cw = bar.width - 28;
                    lumo_app_fill_rect(pixels, width, height,
                        bar.x + 12 + cw, bar.y + 8, 2, 20, theme.accent);
                }
            }
        } else {
            lumo_app_draw_text(pixels, width, height, bar.x + 12, bar.y + 10,
                2, theme.text_dim,
                editing >= 0 ? "TYPE URL OR SEARCH..." : "TAP TO SEARCH");
        }
    }

    /* bookmark star */
    {
        struct lumo_rect btn = {w - 84, 6, 36, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 2,
            theme.accent, "*");
    }
    /* home/new tab */
    {
        struct lumo_rect btn = {w - 40, 6, 36, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            theme.accent);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 2,
            theme.text, "H");
    }

    /* separator */
    lumo_app_fill_rect(pixels, width, height, 0, 48, w, 1, theme.separator);

    /* ── Content area ── */
    int cy = 56;

    if (editing >= 0) {
        /* editing mode: show instructions */
        lumo_app_draw_text(pixels, width, height, 16, cy, 2,
            theme.accent, "TYPE URL OR SEARCH TERM");
        cy += 24;
        lumo_app_draw_text(pixels, width, height, 16, cy, 2,
            theme.text_dim, "PRESS ENTER ON OSK TO GO");
        cy += 24;
        lumo_app_fill_rect(pixels, width, height, 12, cy, w - 24, 1,
            theme.separator);
        cy += 12;

        /* quick links */
        static const char *quick[][2] = {
            {"DUCKDUCKGO", "https://duckduckgo.com/"},
            {"WIKIPEDIA", "https://en.m.wikipedia.org/"},
            {"GITHUB", "https://github.com/"},
            {"YOUTUBE", "https://m.youtube.com/"},
        };
        for (int i = 0; i < 4 && cy + 40 < h; i++) {
            struct lumo_rect row = {12, cy, w - 24, 36};
            lumo_app_fill_rounded_rect(pixels, width, height, &row, 10,
                theme.card_bg);
            lumo_app_draw_text(pixels, width, height, 24, cy + 10, 2,
                theme.accent, quick[i][0]);
            cy += 44;
        }
    } else {
        /* home view: Lumo Browser branding + bookmarks */
        lumo_app_draw_text(pixels, width, height, w / 2 - 84, cy, 4,
            theme.accent, "LUMO");
        cy += 36;
        lumo_app_draw_text(pixels, width, height, w / 2 - 54, cy, 2,
            theme.text_dim, "BROWSER");
        cy += 32;
        lumo_app_fill_rect(pixels, width, height, w / 4, cy, w / 2, 1,
            theme.separator);
        cy += 16;

        lumo_app_draw_text(pixels, width, height, 16, cy, 2,
            theme.text_dim, "BOOKMARKS");
        cy += 22;

        if (bm_count == 0) {
            lumo_app_draw_text(pixels, width, height, 16, cy, 2,
                theme.text_dim, "TAP * TO SAVE BOOKMARKS");
        } else {
            for (int i = 0; i < bm_count && i < BROWSER_MAX_BOOKMARKS &&
                    cy + 44 < h; i++) {
                struct lumo_rect row = {12, cy, w - 24, 40};
                lumo_app_fill_rounded_rect(pixels, width, height, &row, 10,
                    theme.card_bg);
                lumo_app_draw_outline(pixels, width, height, &row, 1,
                    theme.card_stroke);

                /* bookmark dot */
                lumo_app_fill_rect(pixels, width, height,
                    row.x + 10, row.y + 16, 6, 6, theme.accent);

                /* bookmark name */
                const char *name = ctx->notes[i][0] ? ctx->notes[i] : "BOOKMARK";
                lumo_app_draw_text(pixels, width, height,
                    row.x + 24, row.y + 12, 2, theme.text, name);

                cy += 44;
            }
        }

        cy += 16;
        lumo_app_draw_text(pixels, width, height, 16, cy, 2,
            theme.text_dim, "TAP THE URL BAR TO SEARCH");
        cy += 20;
        lumo_app_draw_text(pixels, width, height, 16, cy, 2,
            theme.text_dim, "OR TAP A BOOKMARK TO OPEN");
    }
}
