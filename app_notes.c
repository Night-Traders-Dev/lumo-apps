#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Notes hit test:
 * >= 0: row index
 * -1: nothing
 * -2: add note button
 * -3: delete note button (list view)
 * -4: done button (editor)
 * -5: delete button (editor)
 *
 * Context-dependent zones are disambiguated by the caller (app_client.c)
 * which knows whether we're in list or editor mode. */
int lumo_app_notes_row_at(
    uint32_t width, uint32_t height, double x, double y
) {
    int h = (int)height;
    int w = (int)width;
    int header_y = 96;
    int row_h = 48;
    int max_rows = (h - header_y - 120) / row_h;

    /* done button: top-right header area */
    if (x >= (double)(w - 100) && y >= 4.0 && y < 44.0)
        return -4;

    /* bottom buttons zone (h-106..h-66) */
    if (y >= (double)(h - 106) && y < (double)(h - 66)) {
        /* right half = add (list), center = delete (editor) */
        if (x >= (double)(w / 2 - 80) && x < (double)(w / 2 + 80))
            return -5; /* editor delete (centered) */
        return -2; /* add note (full width in list) */
    }

    /* delete note (list): h-152..h-112 */
    if (y >= (double)(h - 152) && y < (double)(h - 112))
        return -3;

    /* note rows */
    if (x < 12.0 || x > (double)(w - 12)) return -1;
    if (y < (double)header_y) return -1;
    int idx = (int)(y - (double)header_y) / row_h;
    if (idx < 0 || idx >= max_rows) return -1;
    return idx;
}

/* Render the full-screen note editor */
static void render_editor(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height,
    int editing, const struct lumo_app_theme *theme
) {
    int w = (int)width;
    int h = (int)height;
    const char *text = (ctx != NULL && ctx->notes[editing][0] != '\0')
        ? ctx->notes[editing] : "";
    int text_len = (int)strlen(text);

    /* editor header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 48, theme->header_bg);
    {
        char title[16];
        snprintf(title, sizeof(title), "NOTE %d", editing + 1);
        lumo_app_draw_text(pixels, width, height, 16, 16, 3,
            theme->accent, title);
    }
    lumo_app_fill_rect(pixels, width, height, 0, 47, w, 1, theme->separator);

    /* done button (top-right) */
    {
        struct lumo_rect done_btn = {w - 88, 8, 76, 32};
        lumo_app_fill_rounded_rect(pixels, width, height, &done_btn, 16,
            theme->accent);
        lumo_app_draw_text_centered(pixels, width, height, &done_btn, 2,
            theme->text, "DONE");
    }

    /* editing indicator */
    lumo_app_draw_text(pixels, width, height, 16, 58, 2,
        theme->text_dim, "TAP KEYS TO TYPE");
    lumo_app_fill_rect(pixels, width, height, 12, 78, w - 24, 1,
        theme->separator);

    /* text area — full width card */
    {
        int pad = 16;
        int area_top = 88;
        int area_h = h - area_top - 120;
        struct lumo_rect area = {pad, area_top, w - pad * 2, area_h};
        lumo_app_fill_rounded_rect(pixels, width, height, &area, 16,
            theme->card_bg);
        lumo_app_draw_outline(pixels, width, height, &area, 2,
            theme->accent);

        /* render text with word wrap */
        int tx = area.x + 16;
        int ty = area.y + 16;
        int char_w = 2 * 6; /* scale=2 */
        int line_h = 22;
        int max_chars = (area.width - 32) / char_w;
        if (max_chars < 1) max_chars = 1;

        int pos = 0;
        while (pos < text_len && ty + line_h < area.y + area.height - 8) {
            int line_end = pos + max_chars;
            if (line_end > text_len) line_end = text_len;

            char line_buf[130];
            int line_len = line_end - pos;
            if (line_len >= (int)sizeof(line_buf))
                line_len = (int)sizeof(line_buf) - 1;
            memcpy(line_buf, text + pos, (size_t)line_len);
            line_buf[line_len] = '\0';

            lumo_app_draw_text(pixels, width, height, tx, ty, 2,
                theme->text, line_buf);

            pos += line_len;
            ty += line_h;
        }

        /* blinking cursor */
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if ((ts.tv_nsec / 500000000) == 0) {
                int cursor_line = text_len / max_chars;
                int cursor_col = text_len % max_chars;
                int cx = area.x + 16 + cursor_col * char_w;
                int cy = area.y + 16 + cursor_line * line_h;
                if (cy + line_h < area.y + area.height)
                    lumo_app_fill_rect(pixels, width, height,
                        cx, cy, 2, 18, theme->accent);
            }
        }

        /* character count */
        {
            char count[16];
            snprintf(count, sizeof(count), "%d/128", text_len);
            lumo_app_draw_text(pixels, width, height,
                area.x + area.width - 72, area.y + area.height - 24,
                2, theme->text_dim, count);
        }
    }

    /* delete button (centered, above gesture zone) */
    {
        struct lumo_rect del_btn = {w / 2 - 80, h - 102, 160, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &del_btn, 14,
            0xFFDC3545);
        lumo_app_draw_text_centered(pixels, width, height, &del_btn, 2,
            theme->text, "DELETE");
    }
}

void lumo_app_render_notes(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    int selected = ctx != NULL ? ctx->selected_row : -1;
    int editing = ctx != NULL ? ctx->note_editing : -1;
    int note_count = ctx != NULL ? ctx->note_count : 0;
    int row_y;

    memset(pixels, 0, (size_t)width * height * 4);
    lumo_app_draw_background(pixels, width, height);

    /* full-screen editor when editing a note */
    if (editing >= 0 && editing < note_count) {
        render_editor(ctx, pixels, width, height, editing, &theme);
        return;
    }

    /* === LIST VIEW === */

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, (int)width, 48,
        theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 16, 3, theme.accent,
        "NOTES");
    lumo_app_fill_rect(pixels, width, height, 0, 47, (int)width, 1,
        theme.separator);

    /* instructions */
    lumo_app_draw_text(pixels, width, height, 16, 60, 2,
        theme.text_dim, "TAP TO SELECT / TAP AGAIN TO EDIT");
    lumo_app_fill_rect(pixels, width, height, 12, 88, (int)width - 24, 1,
        theme.separator);

    /* note list as cards */
    row_y = 96;
    for (int i = 0; i < note_count && i < 8; i++) {
        int row_h = 52;
        struct lumo_rect row = {12, row_y, (int)width - 24, row_h - 4};
        bool is_sel = (selected == i);
        const char *text = (ctx != NULL && ctx->notes[i][0] != '\0')
            ? ctx->notes[i] : "EMPTY NOTE";

        uint32_t fill = is_sel ? theme.card_bg : theme.bg;
        lumo_app_fill_rounded_rect(pixels, width, height, &row, 14, fill);

        if (is_sel) {
            lumo_app_draw_outline(pixels, width, height, &row, 2,
                theme.accent);
        }

        /* note number badge */
        {
            char num[4];
            struct lumo_rect badge = {row.x + 8, row.y + 8, 28, 28};
            snprintf(num, sizeof(num), "%d", i + 1);
            lumo_app_fill_rounded_rect(pixels, width, height, &badge, 14,
                is_sel ? theme.accent : theme.card_bg);
            lumo_app_draw_text_centered(pixels, width, height, &badge, 2,
                is_sel ? theme.text : theme.text_dim, num);
        }

        /* note text preview */
        lumo_app_draw_text(pixels, width, height, row.x + 44, row.y + 8,
            2, is_sel ? theme.text : theme.text_dim, text);

        /* hint */
        lumo_app_draw_text(pixels, width, height, row.x + 44, row.y + 28,
            1, theme.text_dim, "TAP TO EDIT");

        row_y += row_h;
    }

    if (note_count == 0) {
        lumo_app_draw_text(pixels, width, height, 16, row_y + 20, 2,
            theme.text_dim, "NO NOTES YET");
        lumo_app_draw_text(pixels, width, height, 16, row_y + 42, 2,
            theme.text_dim, "TAP + TO CREATE ONE");
    }

    /* delete button (only when a note is selected) */
    if (selected >= 0 && note_count > 0) {
        struct lumo_rect del_btn = {12, (int)height - 152, (int)width - 24, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &del_btn, 14,
            0xFFDC3545);
        lumo_app_draw_text_centered(pixels, width, height, &del_btn, 2,
            theme.text, "DELETE NOTE");
    }

    /* add button (above gesture zone) */
    {
        struct lumo_rect add_btn = {12, (int)height - 106, (int)width - 24, 40};
        lumo_app_fill_rounded_rect(pixels, width, height, &add_btn, 14,
            theme.accent);
        lumo_app_draw_text_centered(pixels, width, height, &add_btn, 2,
            theme.text, "+ ADD NOTE");
    }
}
