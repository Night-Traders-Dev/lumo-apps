/*
 * app_package.c — Package Manager for Lumo OS.
 * Displays installed packages and available updates via dpkg/apt.
 * Read-only view — updates require confirmation on-device.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Package info read from dpkg */
struct pkg_info {
    char name[64];
    char version[32];
    char size[16];
};

static struct pkg_info cached_pkgs[64];
static int cached_pkg_count = -1; /* -1 = not loaded */
static uint64_t last_scan;

static void scan_packages(void) {
    FILE *fp = popen("dpkg-query -W -f '${Package}\\t${Version}\\t${Installed-Size}\\n' 2>/dev/null | head -64", "r");
    if (!fp) return;
    cached_pkg_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && cached_pkg_count < 64) {
        struct pkg_info *p = &cached_pkgs[cached_pkg_count];
        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        snprintf(p->name, sizeof(p->name), "%s", line);
        char *tab2 = strchr(tab1 + 1, '\t');
        if (tab2) {
            *tab2 = '\0';
            snprintf(p->version, sizeof(p->version), "%s", tab1 + 1);
            char *nl = strchr(tab2 + 1, '\n');
            if (nl) *nl = '\0';
            int kb = atoi(tab2 + 1);
            if (kb >= 1024)
                snprintf(p->size, sizeof(p->size), "%dM", kb / 1024);
            else
                snprintf(p->size, sizeof(p->size), "%dK", kb);
        } else {
            snprintf(p->version, sizeof(p->version), "%s", tab1 + 1);
            p->size[0] = '\0';
        }
        cached_pkg_count++;
    }
    pclose(fp);
}

void lumo_app_render_package(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;
    int scroll = ctx != NULL ? ctx->scroll_offset : 0;

    lumo_app_draw_background(pixels, width, height);

    /* lazy-load package list (once, cached) */
    if (cached_pkg_count < 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        last_scan = (uint64_t)ts.tv_sec;
        scan_packages();
    }

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 48, theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 14, 3, theme.accent,
        "PACKAGES");
    {
        char badge[32];
        snprintf(badge, sizeof(badge), "%d INSTALLED", cached_pkg_count);
        int tw = (int)strlen(badge) * 6;
        lumo_app_draw_text(pixels, width, height, w - tw - 16, 18, 1,
            theme.text_dim, badge);
    }
    lumo_app_fill_rect(pixels, width, height, 8, 48, w - 16, 1,
        theme.separator);

    int y = 56;
    int pad = 12;
    int row_h = 44;
    int max_visible = (h - y - 10) / row_h;

    for (int i = scroll; i < cached_pkg_count && i < scroll + max_visible;
            i++) {
        struct pkg_info *p = &cached_pkgs[i];
        struct lumo_rect row = {pad, y, w - pad * 2, row_h - 4};
        lumo_app_fill_rounded_rect(pixels, width, height, &row, 6,
            theme.card_bg);

        /* package name */
        lumo_app_draw_text(pixels, width, height, pad + 8, y + 6, 2,
            theme.text, p->name);

        /* version */
        lumo_app_draw_text(pixels, width, height, pad + 8, y + 24, 1,
            theme.text_dim, p->version);

        /* size */
        if (p->size[0]) {
            int sw = (int)strlen(p->size) * 6;
            lumo_app_draw_text(pixels, width, height,
                w - pad - sw - 12, y + 14, 1, theme.text_dim, p->size);
        }
        y += row_h;
    }
}
