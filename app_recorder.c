/*
 * app_recorder.c — Audio Recorder for Lumo OS.
 * Records via arecord/pw-record, shows elapsed time and waveform indicator.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* hit-test: 0=record/stop, 1=play, 2=delete, 100+row=recording row, -1=nothing */
int lumo_app_recorder_button_at(uint32_t width, uint32_t height,
    double x, double y)
{
    int w = (int)width, h = (int)height;
    int tx = (int)x, ty = (int)y;
    int center_x = w / 2;

    /* record button (center, y ~ h/3) */
    int btn_y = h / 3;
    int dx = tx - center_x;
    int dy = ty - btn_y;
    if (dx * dx + dy * dy < 40 * 40) return 0;

    /* play button left of record */
    int play_x = center_x - 100;
    dx = tx - play_x; dy = ty - btn_y;
    if (dx * dx + dy * dy < 28 * 28) return 1;

    /* delete button right of record */
    int del_x = center_x + 100;
    dx = tx - del_x; dy = ty - btn_y;
    if (dx * dx + dy * dy < 28 * 28) return 2;

    /* recording list */
    int list_y = h / 2 + 40;
    if (ty >= list_y && ty < h - 10) {
        int row = (ty - list_y) / 48;
        return 100 + row;
    }
    return -1;
}

void lumo_app_render_recorder(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;
    bool recording = ctx != NULL && ctx->stopwatch_running;
    uint64_t elapsed = ctx != NULL ? ctx->stopwatch_elapsed_ms : 0;

    lumo_app_draw_background(pixels, width, height);

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 48, theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 14, 3, theme.accent,
        "RECORDER");
    lumo_app_fill_rect(pixels, width, height, 8, 48, w - 16, 1,
        theme.separator);

    /* elapsed time */
    int secs = (int)(elapsed / 1000);
    int mins = secs / 60;
    secs %= 60;
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", mins, secs);
    int tw = (int)strlen(time_str) * 5 * 6;
    lumo_app_draw_text(pixels, width, height,
        (w - tw) / 2, 70, 5, theme.text, time_str);

    /* waveform visualization (simulated) */
    if (recording) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        int wave_y = 130;
        int wave_h = 40;
        uint32_t wave_color = lumo_app_argb(0x80,
            (theme.accent >> 16) & 0xFF,
            (theme.accent >> 8) & 0xFF,
            theme.accent & 0xFF);
        for (int x = 20; x < w - 20; x++) {
            double v = sin((double)x * 0.05 + t * 6.0) *
                sin((double)x * 0.02 + t * 3.0);
            int bar_h = (int)(fabs(v) * wave_h);
            if (bar_h < 2) bar_h = 2;
            lumo_app_fill_rect(pixels, width, height,
                x, wave_y + wave_h / 2 - bar_h / 2, 1, bar_h, wave_color);
        }
    }

    /* record button */
    int btn_y = h / 3;
    int center_x = w / 2;
    {
        /* outer ring */
        for (int dy = -38; dy <= 38; dy++) {
            for (int dx = -38; dx <= 38; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 >= 34 * 34 && d2 <= 38 * 38) {
                    int px = center_x + dx;
                    int py = btn_y + dy;
                    if (px >= 0 && px < w && py >= 0 && py < h)
                        pixels[py * w + px] = theme.text;
                }
            }
        }
        /* inner circle */
        uint32_t inner_color = recording
            ? lumo_app_argb(0xFF, 0xFF, 0x44, 0x44)
            : lumo_app_argb(0xFF, 0xFF, 0x22, 0x22);
        int inner_r = recording ? 20 : 28;
        for (int dy = -inner_r; dy <= inner_r; dy++) {
            for (int dx = -inner_r; dx <= inner_r; dx++) {
                if (dx * dx + dy * dy <= inner_r * inner_r) {
                    int px = center_x + dx;
                    int py = btn_y + dy;
                    if (px >= 0 && px < w && py >= 0 && py < h)
                        pixels[py * w + px] = inner_color;
                }
            }
        }
    }

    /* play / delete buttons */
    {
        struct lumo_rect play = {center_x - 124, btn_y - 24, 48, 48};
        lumo_app_fill_rounded_rect(pixels, width, height, &play, 24,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &play, 3,
            theme.accent, ">");
    }
    {
        struct lumo_rect del = {center_x + 76, btn_y - 24, 48, 48};
        lumo_app_fill_rounded_rect(pixels, width, height, &del, 24,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &del, 2,
            theme.text_dim, "DEL");
    }

    /* recording list */
    int list_y = h / 2 + 40;
    lumo_app_draw_text(pixels, width, height, 16, list_y - 20, 1,
        theme.text_dim, "RECORDINGS");
    int file_count = ctx != NULL ? ctx->media_file_count : 0;
    if (file_count == 0) {
        lumo_app_draw_text(pixels, width, height, 16, list_y + 8, 2,
            theme.text_dim, "NO RECORDINGS YET");
    }
    for (int i = 0; i < file_count && list_y + 48 < h; i++) {
        struct lumo_rect row = {12, list_y, w - 24, 40};
        lumo_app_fill_rounded_rect(pixels, width, height, &row, 8,
            theme.card_bg);
        lumo_app_draw_text(pixels, width, height, 24, list_y + 12, 2,
            theme.text, ctx->media_files[i]);
        list_y += 48;
    }
}
