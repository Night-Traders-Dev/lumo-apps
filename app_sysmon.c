/*
 * app_sysmon.c — Lumo System Monitor (btop-style GUI)
 *
 * Displays real-time CPU, GPU, RAM, and filesystem stats in a
 * touch-friendly dashboard layout.  Reads from /proc and /sys.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <time.h>

/* ── CPU stats ───────────────────────────────────────────────────── */

struct cpu_stat {
    unsigned long user, nice, system, idle, iowait, irq, softirq;
};

static struct cpu_stat prev_total, prev_cores[8];
static int cpu_pcts[8];
static int cpu_total_pct;

static void read_cpu_stats(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;

    char line[256];
    int core = -1;
    while (fgets(line, sizeof(line), fp) && core < 8) {
        struct cpu_stat cur = {0};
        if (strncmp(line, "cpu ", 4) == 0) {
            sscanf(line + 4, "%lu %lu %lu %lu %lu %lu %lu",
                &cur.user, &cur.nice, &cur.system, &cur.idle,
                &cur.iowait, &cur.irq, &cur.softirq);
            unsigned long total_d = (cur.user - prev_total.user) +
                (cur.nice - prev_total.nice) +
                (cur.system - prev_total.system) +
                (cur.idle - prev_total.idle) +
                (cur.iowait - prev_total.iowait) +
                (cur.irq - prev_total.irq) +
                (cur.softirq - prev_total.softirq);
            unsigned long idle_d = (cur.idle - prev_total.idle) +
                (cur.iowait - prev_total.iowait);
            cpu_total_pct = total_d > 0
                ? (int)(100 * (total_d - idle_d) / total_d) : 0;
            prev_total = cur;
            core = 0;
        } else if (strncmp(line, "cpu", 3) == 0 && core >= 0 && core < 8) {
            sscanf(line + 5, "%lu %lu %lu %lu %lu %lu %lu",
                &cur.user, &cur.nice, &cur.system, &cur.idle,
                &cur.iowait, &cur.irq, &cur.softirq);
            unsigned long total_d = (cur.user - prev_cores[core].user) +
                (cur.nice - prev_cores[core].nice) +
                (cur.system - prev_cores[core].system) +
                (cur.idle - prev_cores[core].idle) +
                (cur.iowait - prev_cores[core].iowait) +
                (cur.irq - prev_cores[core].irq) +
                (cur.softirq - prev_cores[core].softirq);
            unsigned long idle_d = (cur.idle - prev_cores[core].idle) +
                (cur.iowait - prev_cores[core].iowait);
            cpu_pcts[core] = total_d > 0
                ? (int)(100 * (total_d - idle_d) / total_d) : 0;
            prev_cores[core] = cur;
            core++;
        }
    }
    fclose(fp);
}

/* ── helpers ─────────────────────────────────────────────────────── */

static void draw_bar(uint32_t *pixels, uint32_t width, uint32_t height,
    int x, int y, int bar_w, int bar_h, int pct,
    uint32_t fg, uint32_t bg, uint32_t outline)
{
    struct lumo_rect bg_rect = {x, y, bar_w, bar_h};
    lumo_app_fill_rounded_rect(pixels, width, height, &bg_rect, 4, bg);
    lumo_app_draw_outline(pixels, width, height, &bg_rect, 1, outline);

    int fill_w = (bar_w - 4) * pct / 100;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > bar_w - 4) fill_w = bar_w - 4;
    if (fill_w > 0) {
        struct lumo_rect fill = {x + 2, y + 2, fill_w, bar_h - 4};
        lumo_app_fill_rounded_rect(pixels, width, height, &fill, 3, fg);
    }
}

static uint32_t pct_color(int pct) {
    if (pct > 80) return lumo_app_argb(0xFF, 0xFF, 0x44, 0x44);
    if (pct > 50) return lumo_app_argb(0xFF, 0xFF, 0xAA, 0x44);
    return lumo_app_argb(0xFF, 0x44, 0xCC, 0x44);
}

/* ── render ──────────────────────────────────────────────────────── */

void lumo_app_render_sysmon(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    (void)ctx;
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    lumo_app_draw_background(pixels, width, height);

    int y = 12;
    int pad = 16;
    int col_w = (int)width - pad * 2;
    char buf[128];

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, (int)width, 44,
        theme.header_bg);
    lumo_app_draw_text(pixels, width, height, pad, 12, 3,
        theme.accent, "SYSTEM MONITOR");
    lumo_app_fill_rect(pixels, width, height, pad, 44, col_w, 1,
        theme.separator);
    y = 52;

    /* ── CPU ─────────────────────────────────────────────────── */
    read_cpu_stats();

    snprintf(buf, sizeof(buf), "CPU  %d%%", cpu_total_pct);
    lumo_app_draw_text(pixels, width, height, pad, y, 3,
        theme.text, buf);
    y += 28;

    /* total bar */
    draw_bar(pixels, width, height, pad, y, col_w, 18,
        cpu_total_pct, pct_color(cpu_total_pct),
        theme.card_bg, theme.card_stroke);
    y += 24;

    /* per-core mini bars (two rows of 4) */
    int bar_w = (col_w - 3 * 8) / 4;
    if (bar_w < 40) bar_w = 40;
    for (int i = 0; i < 8; i++) {
        int row = i / 4;
        int col = i % 4;
        int bx = pad + col * (bar_w + 8);
        int by = y + row * 24;
        draw_bar(pixels, width, height, bx, by, bar_w, 18,
            cpu_pcts[i], pct_color(cpu_pcts[i]),
            theme.card_bg, theme.card_stroke);
        snprintf(buf, sizeof(buf), "%d:%d%%", i, cpu_pcts[i]);
        lumo_app_draw_text(pixels, width, height,
            bx + 4, by + 3, 2, theme.text, buf);
    }
    y += 54;

    lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
        theme.separator);
    y += 8;

    /* ── GPU — PowerVR BXE-2-32 (K1 SoC) ──────────────────── */
    {
        int gpu_util_pct = 0;
        int gpu_3d_pct = 0;
        double gpu_freq_mhz = 0.0;
        double gpu_temp_c = 0.0;
        unsigned long cma_total_kb = 0, cma_free_kb = 0;

        /* 1. GPU utilization + 3D load from pvr/status */
        FILE *fp = fopen("/sys/kernel/debug/pvr/status", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                int val = 0;
                if (strstr(line, "GPU Utilisation") != NULL) {
                    char *c = strchr(line, ':');
                    if (c && sscanf(c + 1, " %d", &val) == 1)
                        gpu_util_pct = val;
                } else if (strstr(line, "3D:") != NULL &&
                        strstr(line, "DM") == NULL) {
                    char *c = strstr(line, "3D:");
                    if (c && sscanf(c + 3, " %d", &val) == 1)
                        gpu_3d_pct = val;
                }
            }
            fclose(fp);
        }

        /* 2. GPU frequency from clk_summary (cac00000.imggpu) */
        fp = fopen("/sys/kernel/debug/clk/clk_summary", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "cac00000.imggpu") != NULL) {
                    char *p = line;
                    while (*p) {
                        if (*p >= '0' && *p <= '9') {
                            unsigned long n = strtoul(p, &p, 10);
                            if (n > 1000000) {
                                gpu_freq_mhz = (double)n / 1000000.0;
                                break;
                            }
                        } else { p++; }
                    }
                    break;
                }
            }
            fclose(fp);
        }

        /* 3. Temperature from thermal_zone0 */
        fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
        if (fp) {
            int raw = 0;
            if (fscanf(fp, "%d", &raw) == 1)
                gpu_temp_c = (double)raw / 1000.0;
            fclose(fp);
        }

        /* 4. VRAM (CMA) from /proc/meminfo */
        fp = fopen("/proc/meminfo", "r");
        if (fp) {
            char line[128];
            while (fgets(line, sizeof(line), fp)) {
                sscanf(line, "CmaTotal: %lu", &cma_total_kb);
                sscanf(line, "CmaFree: %lu", &cma_free_kb);
            }
            fclose(fp);
        }

        /* ── render GPU section ─────────────────────────────── */

        snprintf(buf, sizeof(buf), "GPU  %d%%", gpu_util_pct);
        lumo_app_draw_text(pixels, width, height, pad, y, 3,
            theme.text, buf);
        y += 26;

        /* total load bar */
        draw_bar(pixels, width, height, pad, y, col_w, 18,
            gpu_util_pct, pct_color(gpu_util_pct),
            theme.card_bg, theme.card_stroke);
        y += 24;

        /* name + frequency */
        snprintf(buf, sizeof(buf), "BXE-2-32  %.1f MHz", gpu_freq_mhz);
        lumo_app_draw_text(pixels, width, height, pad + 8, y, 2,
            theme.accent, buf);
        y += 20;

        /* temperature */
        snprintf(buf, sizeof(buf), "TEMP %.1fC", gpu_temp_c);
        lumo_app_draw_text(pixels, width, height, pad + 8, y, 2,
            gpu_temp_c > 70.0
                ? lumo_app_argb(0xFF, 0xFF, 0x44, 0x44)
                : theme.text_dim,
            buf);

        /* 3D engine on same line, right side */
        snprintf(buf, sizeof(buf), "3D: %d%%", gpu_3d_pct);
        lumo_app_draw_text(pixels, width, height,
            (int)width / 2 + 20, y, 2, theme.text_dim, buf);
        y += 20;

        /* VRAM (CMA) bar */
        if (cma_total_kb > 0) {
            unsigned long cma_used_kb = cma_total_kb - cma_free_kb;
            int cma_pct = (int)(100 * cma_used_kb / cma_total_kb);
            snprintf(buf, sizeof(buf), "VRAM %lu/%luMB  %d%%",
                cma_used_kb / 1024, cma_total_kb / 1024, cma_pct);
            lumo_app_draw_text(pixels, width, height, pad + 8, y, 2,
                theme.text_dim, buf);
            y += 18;
            draw_bar(pixels, width, height, pad, y, col_w, 14,
                cma_pct, pct_color(cma_pct),
                theme.card_bg, theme.card_stroke);
            y += 20;
        }

        /* renderer */
        const char *renderer = getenv("WLR_RENDERER");
        snprintf(buf, sizeof(buf), "RENDERER: %s",
            renderer ? renderer : "pixman");
        lumo_app_draw_text(pixels, width, height, pad + 8, y, 2,
            renderer && strcmp(renderer, "gles2") == 0
                ? theme.accent : theme.text_dim,
            buf);
        y += 20;
    }

    lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
        theme.separator);
    y += 8;

    /* ── RAM ─────────────────────────────────────────────────── */
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            unsigned long total_mb = (si.totalram * si.mem_unit) / (1024*1024);
            unsigned long used_mb = total_mb -
                ((si.freeram + si.bufferram + si.sharedram) *
                 si.mem_unit) / (1024*1024);
            int pct = total_mb > 0 ? (int)(100 * used_mb / total_mb) : 0;

            snprintf(buf, sizeof(buf), "RAM  %lu/%luMB  %d%%",
                used_mb, total_mb, pct);
            lumo_app_draw_text(pixels, width, height, pad, y, 3,
                theme.text, buf);
            y += 28;

            draw_bar(pixels, width, height, pad, y, col_w, 18,
                pct, pct_color(pct), theme.card_bg, theme.card_stroke);
            y += 24;

            /* swap + uptime on same line */
            unsigned long swap_total = (si.totalswap * si.mem_unit) / (1024*1024);
            unsigned long swap_used = swap_total -
                (si.freeswap * si.mem_unit) / (1024*1024);
            snprintf(buf, sizeof(buf), "SWAP %lu/%luMB", swap_used, swap_total);
            lumo_app_draw_text(pixels, width, height, pad + 8, y, 2,
                theme.text_dim, buf);

            long up = si.uptime;
            int days = (int)(up / 86400);
            int hours = (int)((up % 86400) / 3600);
            int mins = (int)((up % 3600) / 60);
            snprintf(buf, sizeof(buf), "UP %dd%dh%dm", days, hours, mins);
            lumo_app_draw_text(pixels, width, height,
                (int)width / 2 + 20, y, 2, theme.text_dim, buf);
            y += 20;

            /* load + procs on same line */
            snprintf(buf, sizeof(buf), "LOAD %.1f %.1f  PROCS %d",
                si.loads[0] / 65536.0, si.loads[1] / 65536.0, si.procs);
            lumo_app_draw_text(pixels, width, height, pad + 8, y, 2,
                theme.text_dim, buf);
            y += 20;
        }
    }

    lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
        theme.separator);
    y += 8;

    /* ── Storage ─────────────────────────────────────────────── */
    lumo_app_draw_text(pixels, width, height, pad, y, 3,
        theme.text, "STORAGE");
    y += 26;

    static const char *mounts[] = {"/", "/data", "/tmp", NULL};
    static const char *names[] = {"ROOT", "DATA", "TMP"};

    for (int i = 0; mounts[i] != NULL && y + 36 < (int)height; i++) {
        struct statvfs st;
        if (statvfs(mounts[i], &st) != 0) continue;

        unsigned long total_mb = (unsigned long)
            (st.f_blocks * (st.f_frsize / 1024)) / 1024;
        unsigned long free_mb = (unsigned long)
            (st.f_bavail * (st.f_frsize / 1024)) / 1024;
        unsigned long used_mb = total_mb > free_mb ? total_mb - free_mb : 0;
        int pct = total_mb > 0 ? (int)(100 * used_mb / total_mb) : 0;

        snprintf(buf, sizeof(buf), "%-5s %lu/%luMB  %d%%",
            names[i], used_mb, total_mb, pct);
        lumo_app_draw_text(pixels, width, height, pad + 8, y, 2,
            theme.text_dim, buf);
        y += 18;

        draw_bar(pixels, width, height, pad, y, col_w, 14,
            pct, pct_color(pct), theme.card_bg, theme.card_stroke);
        y += 20;
    }

}
