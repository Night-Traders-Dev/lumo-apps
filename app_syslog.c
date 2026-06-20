/*
 * app_syslog.c — System Log viewer for Lumo OS.
 * Reads recent journal entries via journalctl.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SYSLOG_MAX_LINES 64
#define SYSLOG_LINE_LEN 128

static char log_lines[SYSLOG_MAX_LINES][SYSLOG_LINE_LEN];
static int log_line_count = -1; /* -1 = not loaded */

static void load_journal(void) {
    FILE *fp = popen(
        "journalctl -b --no-pager -n 64 -o short-monotonic 2>/dev/null",
        "r");
    if (!fp) return;
    log_line_count = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) && log_line_count < SYSLOG_MAX_LINES) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        snprintf(log_lines[log_line_count], SYSLOG_LINE_LEN, "%s", line);
        log_line_count++;
    }
    pclose(fp);
}

void lumo_app_render_syslog(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;
    int scroll = ctx != NULL ? ctx->scroll_offset : 0;

    lumo_app_draw_background(pixels, width, height);

    /* lazy load */
    if (log_line_count < 0) load_journal();

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 48, theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 14, 3, theme.accent,
        "SYSTEM LOG");
    {
        char badge[32];
        snprintf(badge, sizeof(badge), "%d ENTRIES", log_line_count);
        int tw = (int)strlen(badge) * 6;
        lumo_app_draw_text(pixels, width, height, w - tw - 16, 18, 1,
            theme.text_dim, badge);
    }
    lumo_app_fill_rect(pixels, width, height, 8, 48, w - 16, 1,
        theme.separator);

    int y = 56;
    int line_h = 14;
    int pad = 8;
    int max_visible = (h - y - 10) / line_h;

    /* color-code by severity */
    for (int i = scroll; i < log_line_count && i < scroll + max_visible; i++) {
        const char *line = log_lines[i];
        uint32_t color = theme.text;

        /* detect severity keywords */
        if (strstr(line, "error") || strstr(line, "ERROR") ||
                strstr(line, "fail") || strstr(line, "FAIL"))
            color = lumo_app_argb(0xFF, 0xFF, 0x44, 0x44);
        else if (strstr(line, "warn") || strstr(line, "WARN"))
            color = lumo_app_argb(0xFF, 0xFF, 0xAA, 0x44);
        else if (strstr(line, "debug") || strstr(line, "DEBUG"))
            color = theme.text_dim;

        /* truncate to fit screen width */
        int max_chars = (w - pad * 2) / 6;
        char display[SYSLOG_LINE_LEN];
        snprintf(display, sizeof(display), "%.*s", max_chars, line);
        lumo_app_draw_text(pixels, width, height, pad, y, 1, color, display);
        y += line_h;
    }

    /* scroll position */
    {
        char pos[32];
        snprintf(pos, sizeof(pos), "LINE %d/%d", scroll + 1, log_line_count);
        lumo_app_draw_text(pixels, width, height, pad, h - 14, 1,
            theme.text_dim, pos);
    }
}
