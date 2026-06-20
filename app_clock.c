#include "lumo/app_render.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* hit test: returns tab index (0-3) for tab bar taps,
 * 10 = stopwatch start/stop, 11 = stopwatch reset,
 * 20 = timer +1m, 21 = timer +5m, 22 = timer start/stop, 23 = timer reset,
 * 30 = alarm toggle, 31 = alarm hour+, 32 = alarm min+ */
int lumo_app_clock_card_at(
    uint32_t width, uint32_t height, double x, double y
) {
    int tab_w = (int)width / 4;
    int tab_y = 48;
    (void)height;

    /* tab bar */
    if (y >= tab_y && y < tab_y + 36) {
        int tab = (int)x / tab_w;
        if (tab >= 0 && tab <= 3) return tab;
    }

    /* content area actions depend on which tab is active, but
     * we return generic zones here — the client maps them */
    int content_y = 96;
    if (y >= content_y) {
        int zone = ((int)y - content_y) / 60;
        int half = (int)x >= (int)width / 2 ? 1 : 0;
        return 10 + zone * 2 + half;
    }
    return -1;
}

static void draw_tab_bar(uint32_t *pixels, uint32_t width, uint32_t height,
    int active_tab, const struct lumo_app_theme *theme)
{
    static const char *tabs[] = {"CLOCK", "ALARM", "STOPWATCH", "TIMER"};
    int tab_w = (int)width / 4;
    int tab_y = 48;

    for (int i = 0; i < 4; i++) {
        int tx = i * tab_w;
        bool active = (i == active_tab);
        uint32_t color = active ? theme->accent : theme->text_dim;

        lumo_app_draw_text(pixels, width, height,
            tx + (tab_w - (int)strlen(tabs[i]) * 12) / 2,
            tab_y + 10, 2, color, tabs[i]);

        if (active) {
            struct lumo_rect indicator = {tx + 8, tab_y + 32,
                tab_w - 16, 3};
            lumo_app_fill_rounded_rect(pixels, width, height,
                &indicator, 1, theme->accent);
        }
    }

    /* separator */
    lumo_app_fill_rect(pixels, width, height, 0, tab_y + 36,
        (int)width, 1, theme->separator);
}

static void draw_clock_tab(uint32_t *pixels, uint32_t width, uint32_t height,
    const struct lumo_app_theme *theme)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    char time_buf[16], date_buf[32], day_buf[16], zone_buf[8];

    localtime_r(&now, &tm_now);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_now);
    strftime(date_buf, sizeof(date_buf), "%B %d, %Y", &tm_now);
    strftime(day_buf, sizeof(day_buf), "%A", &tm_now);
    strftime(zone_buf, sizeof(zone_buf), "%Z", &tm_now);

    int cx = (int)width / 2;
    int y = 120;

    /* large time */
    {
        int tw = (int)strlen(time_buf) * 8 * 6 - 8;
        lumo_app_draw_text(pixels, width, height,
            cx - tw / 2, y, 8, theme->accent, time_buf);
    }
    y += 70;

    /* day name */
    {
        int dw = (int)strlen(day_buf) * 3 * 6 - 3;
        lumo_app_draw_text(pixels, width, height,
            cx - dw / 2, y, 3, theme->text, day_buf);
    }
    y += 30;

    /* full date */
    {
        int dw = (int)strlen(date_buf) * 2 * 6 - 2;
        lumo_app_draw_text(pixels, width, height,
            cx - dw / 2, y, 2, theme->text_dim, date_buf);
    }
    y += 24;

    /* timezone */
    {
        int zw = (int)strlen(zone_buf) * 2 * 6 - 2;
        lumo_app_draw_text(pixels, width, height,
            cx - zw / 2, y, 2, theme->text_dim, zone_buf);
    }

    /* unix timestamp */
    y += 40;
    {
        char unix_buf[32];
        snprintf(unix_buf, sizeof(unix_buf), "UNIX %ld", (long)now);
        int uw = (int)strlen(unix_buf) * 2 * 6 - 2;
        lumo_app_draw_text(pixels, width, height,
            cx - uw / 2, y, 2, theme->separator, unix_buf);
    }
}

static void draw_alarm_tab(uint32_t *pixels, uint32_t width, uint32_t height,
    const struct lumo_app_render_context *ctx,
    const struct lumo_app_theme *theme)
{
    int cx = (int)width / 2;
    int y = 110;
    uint32_t ah = ctx != NULL ? ctx->alarm_hour : 6;
    uint32_t am = ctx != NULL ? ctx->alarm_min : 30;
    bool enabled = ctx != NULL ? ctx->alarm_enabled : false;
    bool firing = ctx != NULL ? ctx->alarm_firing : false;
    char alarm_buf[8];

    snprintf(alarm_buf, sizeof(alarm_buf), "%02u:%02u", ah, am);

    /* alarm firing banner */
    if (firing) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        bool flash = (ts.tv_nsec / 500000000) == 0;
        struct lumo_rect banner = {12, 96, (int)width - 24, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &banner, 12,
            flash ? theme->accent : 0xFFFF2222);
        lumo_app_draw_text_centered(pixels, width, height, &banner, 2,
            theme->text, "!! ALARM RINGING !!");
        y += 30;
    }

    /* large alarm time */
    {
        int tw = (int)strlen(alarm_buf) * 8 * 6 - 8;
        uint32_t time_color = firing ? 0xFFFF4444 :
            (enabled ? theme->accent : theme->text_dim);
        lumo_app_draw_text(pixels, width, height,
            cx - tw / 2, y, 8, time_color, alarm_buf);
    }
    y += 80;

    /* toggle / dismiss */
    {
        const char *label = firing ? "DISMISS" :
            (enabled ? "ALARM ON" : "ALARM OFF");
        uint32_t btn_color = firing ? 0xFFFF2222 :
            (enabled ? theme->accent : theme->card_bg);
        struct lumo_rect btn = {cx - 80, y, 160, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 18,
            btn_color);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 2,
            theme->text, label);
    }
    y += 52;

    /* adjust buttons */
    {
        struct lumo_rect hr_btn = {cx - 140, y, 120, 36};
        struct lumo_rect mn_btn = {cx + 20, y, 120, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &hr_btn, 12,
            theme->card_bg);
        lumo_app_draw_outline(pixels, width, height, &hr_btn, 1,
            theme->card_stroke);
        lumo_app_draw_text_centered(pixels, width, height, &hr_btn, 2,
            theme->text, "HOUR +");
        lumo_app_fill_rounded_rect(pixels, width, height, &mn_btn, 12,
            theme->card_bg);
        lumo_app_draw_outline(pixels, width, height, &mn_btn, 1,
            theme->card_stroke);
        lumo_app_draw_text_centered(pixels, width, height, &mn_btn, 2,
            theme->text, "MIN +");
    }
}

static void draw_stopwatch_tab(uint32_t *pixels, uint32_t width,
    uint32_t height, const struct lumo_app_render_context *ctx,
    const struct lumo_app_theme *theme)
{
    int cx = (int)width / 2;
    int y = 120;
    uint64_t ms = ctx != NULL ? ctx->stopwatch_elapsed_ms : 0;
    bool running = ctx != NULL && ctx->stopwatch_running;
    uint32_t secs = (uint32_t)(ms / 1000);
    uint32_t mins = secs / 60;
    uint32_t hrs = mins / 60;
    uint32_t centis = (uint32_t)((ms % 1000) / 10);
    char sw_buf[16];

    snprintf(sw_buf, sizeof(sw_buf), "%02u:%02u:%02u", hrs, mins % 60,
        secs % 60);

    /* large stopwatch time */
    {
        int tw = (int)strlen(sw_buf) * 8 * 6 - 8;
        lumo_app_draw_text(pixels, width, height,
            cx - tw / 2, y, 8,
            running ? theme->accent : theme->text, sw_buf);
    }
    y += 70;

    /* centiseconds */
    {
        char cs_buf[8];
        snprintf(cs_buf, sizeof(cs_buf), ".%02u", centis);
        int cw = (int)strlen(cs_buf) * 4 * 6 - 4;
        lumo_app_draw_text(pixels, width, height,
            cx - cw / 2, y, 4, theme->text_dim, cs_buf);
    }
    y += 50;

    /* start/stop and reset buttons */
    {
        struct lumo_rect start_btn = {cx - 140, y, 120, 40};
        struct lumo_rect reset_btn = {cx + 20, y, 120, 40};
        lumo_app_fill_rounded_rect(pixels, width, height, &start_btn, 14,
            running ? theme->card_bg : theme->accent);
        lumo_app_draw_text_centered(pixels, width, height, &start_btn, 2,
            theme->text, running ? "STOP" : "START");
        lumo_app_fill_rounded_rect(pixels, width, height, &reset_btn, 14,
            theme->card_bg);
        lumo_app_draw_outline(pixels, width, height, &reset_btn, 1,
            theme->card_stroke);
        lumo_app_draw_text_centered(pixels, width, height, &reset_btn, 2,
            theme->text, "RESET");
    }
}

static void draw_timer_tab(uint32_t *pixels, uint32_t width,
    uint32_t height, const struct lumo_app_render_context *ctx,
    const struct lumo_app_theme *theme)
{
    int cx = (int)width / 2;
    int y = 120;
    uint32_t total = ctx != NULL ? ctx->timer_total_sec : 0;
    uint32_t remaining = ctx != NULL ? ctx->timer_remaining_sec : 0;
    bool running = ctx != NULL && ctx->timer_running;
    uint32_t disp = running ? remaining : total;
    uint32_t mins = disp / 60;
    uint32_t secs = disp % 60;
    char tm_buf[8];

    snprintf(tm_buf, sizeof(tm_buf), "%02u:%02u", mins, secs);

    /* timer finished indicator */
    bool finished = !running && total == 0 && remaining == 0;
    if (finished) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        bool flash = (ts.tv_nsec / 500000000) == 0;
        struct lumo_rect banner = {12, 96, (int)width - 24, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &banner, 12,
            flash ? theme->accent : 0xFF28A745);
        lumo_app_draw_text_centered(pixels, width, height, &banner, 2,
            theme->text, "TIMER COMPLETE!");
        y += 30;
    }

    /* large timer display */
    {
        int tw = (int)strlen(tm_buf) * 8 * 6 - 8;
        lumo_app_draw_text(pixels, width, height,
            cx - tw / 2, y, 8,
            running ? theme->accent : theme->text, tm_buf);
    }
    y += 80;

    /* progress ring (simplified as a bar) */
    if (total > 0) {
        int bar_w = (int)width - 80;
        int fill_w = running && total > 0
            ? (int)((uint64_t)remaining * (uint64_t)bar_w / total)
            : bar_w;
        struct lumo_rect bar_bg = {40, y, bar_w, 8};
        struct lumo_rect bar_fill = {40, y, fill_w, 8};
        lumo_app_fill_rounded_rect(pixels, width, height, &bar_bg, 4,
            theme->separator);
        lumo_app_fill_rounded_rect(pixels, width, height, &bar_fill, 4,
            theme->accent);
    }
    y += 30;

    /* preset buttons */
    {
        struct lumo_rect btn1 = {cx - 160, y, 70, 36};
        struct lumo_rect btn5 = {cx - 75, y, 70, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn1, 12,
            theme->card_bg);
        lumo_app_draw_outline(pixels, width, height, &btn1, 1,
            theme->card_stroke);
        lumo_app_draw_text_centered(pixels, width, height, &btn1, 2,
            theme->text, "+1M");
        lumo_app_fill_rounded_rect(pixels, width, height, &btn5, 12,
            theme->card_bg);
        lumo_app_draw_outline(pixels, width, height, &btn5, 1,
            theme->card_stroke);
        lumo_app_draw_text_centered(pixels, width, height, &btn5, 2,
            theme->text, "+5M");
    }
    y += 50;

    /* start/stop and reset */
    {
        struct lumo_rect start_btn = {cx - 140, y, 120, 40};
        struct lumo_rect reset_btn = {cx + 20, y, 120, 40};
        lumo_app_fill_rounded_rect(pixels, width, height, &start_btn, 14,
            running ? theme->card_bg : theme->accent);
        lumo_app_draw_text_centered(pixels, width, height, &start_btn, 2,
            theme->text, running ? "STOP" : "START");
        lumo_app_fill_rounded_rect(pixels, width, height, &reset_btn, 14,
            theme->card_bg);
        lumo_app_draw_outline(pixels, width, height, &reset_btn, 1,
            theme->card_stroke);
        lumo_app_draw_text_centered(pixels, width, height, &reset_btn, 2,
            theme->text, "RESET");
    }
}

void lumo_app_render_clock(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    int tab = ctx != NULL ? ctx->clock_tab : 0;

    lumo_app_theme_get(&theme);
    memset(pixels, 0, (size_t)width * height * 4);
    lumo_app_draw_background(pixels, width, height);

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, (int)width, 48,
        theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 16, 3, theme.accent,
        "CLOCK");
    lumo_app_fill_rect(pixels, width, height, 0, 47, (int)width, 1,
        theme.separator);

    /* tab bar */
    draw_tab_bar(pixels, width, height, tab, &theme);

    /* tab content */
    switch (tab) {
    case 0: draw_clock_tab(pixels, width, height, &theme); break;
    case 1: draw_alarm_tab(pixels, width, height, ctx, &theme); break;
    case 2: draw_stopwatch_tab(pixels, width, height, ctx, &theme); break;
    case 3: draw_timer_tab(pixels, width, height, ctx, &theme); break;
    default: draw_clock_tab(pixels, width, height, &theme); break;
    }
}
