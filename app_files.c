#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

static const int files_row_height = 52;
static const int files_header_height = 140;

int lumo_app_files_entry_at(
    uint32_t width, uint32_t height, double x, double y
) {
    int row_y = files_header_height;
    int max_rows = ((int)height - row_y - 60) / files_row_height;
    (void)width;
    if (x < 28.0 || x > (double)width - 28.0) return -1;
    if (y < (double)row_y) return -1;
    int index = (int)(y - row_y) / files_row_height;
    if (index < 0 || index >= max_rows) return -1;
    return index;
}

/* returns: -2 = UP button hit */
int lumo_app_files_up_button_at(
    uint32_t width, uint32_t height, double x, double y
) {
    (void)height;
    if (y >= 52.0 && y < 100.0 && x >= (double)width - 120.0 &&
            x < (double)width - 24.0)
        return -2;
    return 0;
}

static void draw_file_info_overlay(
    uint32_t *pixels, uint32_t width, uint32_t height,
    const struct lumo_app_render_context *ctx
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    /* dim background */
    for (uint32_t i = 0; i < width * height; i++) {
        uint32_t c = pixels[i];
        uint8_t r = (uint8_t)(c >> 16);
        uint8_t g = (uint8_t)(c >> 8);
        uint8_t b = (uint8_t)c;
        pixels[i] = lumo_app_argb(0xFF, r / 2, g / 2, b / 2);
    }

    /* centered info card */
    int cw = (int)width - 60;
    int ch = 320;
    if (cw > 500) cw = 500;
    if (ch > (int)height - 60) ch = (int)height - 60;
    int cx = ((int)width - cw) / 2;
    int cy = ((int)height - ch) / 2;
    struct lumo_rect card = { cx, cy, cw, ch };
    lumo_app_fill_rounded_rect(pixels, width, height, &card, 16,
        theme.card_bg);
    lumo_app_draw_outline(pixels, width, height, &card, 1,
        theme.card_stroke);

    int y = cy + 16;
    lumo_app_draw_text(pixels, width, height, cx + 16, y, 3,
        theme.accent, "FILE INFO");
    y += 28;
    lumo_app_fill_rect(pixels, width, height, cx + 16, y,
        cw - 32, 1, theme.separator);
    y += 12;

    /* file name */
    lumo_app_draw_text(pixels, width, height, cx + 16, y, 2,
        theme.text_dim, "NAME");
    lumo_app_draw_text(pixels, width, height, cx + 110, y, 2,
        theme.text, ctx->file_info_name);
    y += 24;

    /* stat the file */
    struct stat st;
    if (stat(ctx->file_info_path, &st) == 0) {
        char buf[128];

        /* size */
        if (st.st_size < 1024)
            snprintf(buf, sizeof(buf), "%ld BYTES", (long)st.st_size);
        else if (st.st_size < 1024 * 1024)
            snprintf(buf, sizeof(buf), "%.1f KB",
                (double)st.st_size / 1024.0);
        else if (st.st_size < (off_t)1024 * 1024 * 1024)
            snprintf(buf, sizeof(buf), "%.1f MB",
                (double)st.st_size / (1024.0 * 1024.0));
        else
            snprintf(buf, sizeof(buf), "%.2f GB",
                (double)st.st_size / (1024.0 * 1024.0 * 1024.0));
        lumo_app_draw_text(pixels, width, height, cx + 16, y, 2,
            theme.text_dim, "SIZE");
        lumo_app_draw_text(pixels, width, height, cx + 110, y, 2,
            theme.text, buf);
        y += 24;

        /* type */
        const char *ftype = "FILE";
        if (S_ISDIR(st.st_mode)) ftype = "DIRECTORY";
        else if (S_ISLNK(st.st_mode)) ftype = "SYMLINK";
        else if (S_ISBLK(st.st_mode)) ftype = "BLOCK DEVICE";
        else if (S_ISCHR(st.st_mode)) ftype = "CHAR DEVICE";
        else if (S_ISFIFO(st.st_mode)) ftype = "PIPE";
        else if (S_ISSOCK(st.st_mode)) ftype = "SOCKET";
        lumo_app_draw_text(pixels, width, height, cx + 16, y, 2,
            theme.text_dim, "TYPE");
        lumo_app_draw_text(pixels, width, height, cx + 110, y, 2,
            theme.text, ftype);
        y += 24;

        /* permissions */
        snprintf(buf, sizeof(buf), "%c%c%c%c%c%c%c%c%c %o",
            (st.st_mode & S_IRUSR) ? 'R' : '-',
            (st.st_mode & S_IWUSR) ? 'W' : '-',
            (st.st_mode & S_IXUSR) ? 'X' : '-',
            (st.st_mode & S_IRGRP) ? 'R' : '-',
            (st.st_mode & S_IWGRP) ? 'W' : '-',
            (st.st_mode & S_IXGRP) ? 'X' : '-',
            (st.st_mode & S_IROTH) ? 'R' : '-',
            (st.st_mode & S_IWOTH) ? 'W' : '-',
            (st.st_mode & S_IXOTH) ? 'X' : '-',
            (unsigned)(st.st_mode & 0777));
        lumo_app_draw_text(pixels, width, height, cx + 16, y, 2,
            theme.text_dim, "PERMS");
        lumo_app_draw_text(pixels, width, height, cx + 110, y, 2,
            theme.text, buf);
        y += 24;

        /* owner */
        struct passwd *pw = getpwuid(st.st_uid);
        snprintf(buf, sizeof(buf), "%s (%u)",
            pw != NULL ? pw->pw_name : "?", (unsigned)st.st_uid);
        lumo_app_draw_text(pixels, width, height, cx + 16, y, 2,
            theme.text_dim, "OWNER");
        lumo_app_draw_text(pixels, width, height, cx + 110, y, 2,
            theme.text, buf);
        y += 24;

        /* modified */
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
        lumo_app_draw_text(pixels, width, height, cx + 16, y, 2,
            theme.text_dim, "MODIFIED");
        lumo_app_draw_text(pixels, width, height, cx + 140, y, 2,
            theme.text, buf);
        y += 24;

        /* inode + links */
        snprintf(buf, sizeof(buf), "INODE %lu  LINKS %lu",
            (unsigned long)st.st_ino, (unsigned long)st.st_nlink);
        lumo_app_draw_text(pixels, width, height, cx + 16, y, 2,
            theme.text_dim, buf);
    } else {
        lumo_app_draw_text(pixels, width, height, cx + 16, y, 2,
            theme.text_dim, "CANNOT STAT FILE");
    }

    /* dismiss hint */
    lumo_app_draw_text(pixels, width, height,
        cx + 16, cy + ch - 28, 2, theme.text_dim, "TAP TO DISMISS");
}

void lumo_app_render_files(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_rect full = {0, 0, (int)width, (int)height};
    int scroll_off = ctx != NULL ? ctx->scroll_offset : 0;
    int selected = ctx != NULL ? ctx->selected_row : -1;

    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    memset(pixels, 0, (size_t)width * height * 4);
    lumo_app_fill_gradient(pixels, width, height, &full,
        theme.header_bg, theme.bg);

    lumo_app_draw_text(pixels, width, height, 28, 28, 2,
        theme.text_dim, "FILE MANAGER");
    lumo_app_draw_text(pixels, width, height, 28, 60, 4,
        theme.text, "Files");

    const char *browse_path = (ctx != NULL && ctx->browse_path != NULL &&
        ctx->browse_path[0] != '\0') ? ctx->browse_path : "/home";

    /* path bar */
    lumo_app_draw_text(pixels, width, height, 28, 108, 2,
        theme.accent, browse_path);

    /* UP button */
    {
        int bx = (int)width - 120;
        struct lumo_rect up_btn = { bx, 52, 96, 48 };
        lumo_app_fill_rounded_rect(pixels, width, height, &up_btn, 10,
            theme.card_bg);
        lumo_app_draw_outline(pixels, width, height, &up_btn, 1,
            theme.card_stroke);
        lumo_app_draw_text_centered(pixels, width, height, &up_btn, 2,
            theme.accent, "<  UP");
    }

    lumo_app_fill_rect(pixels, width, height, 28, 130, (int)width - 56, 1,
        theme.separator);

    int row_y = files_header_height;
    DIR *dir = opendir(browse_path);
    if (dir != NULL) {
        struct dirent *entry;
        int count = 0, skipped = 0, total_visible = 0;
        int max_rows = ((int)height - row_y - 60) / files_row_height;
        if (max_rows < 1) max_rows = 1;

        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            if (skipped < scroll_off) { skipped++; total_visible++; continue; }
            if (count >= max_rows) { total_visible++; continue; }

            bool is_dir = entry->d_type == DT_DIR;
            struct lumo_rect row_rect = {28, row_y,
                (int)width - 56, files_row_height - 4};

            if (selected == total_visible) {
                lumo_app_fill_rounded_rect(pixels, width, height, &row_rect,
                    10, lumo_app_argb(0xFF, 0x3B, 0x1F, 0x34));
                lumo_app_draw_outline(pixels, width, height, &row_rect, 1,
                    theme.accent);
            } else {
                lumo_app_fill_rounded_rect(pixels, width, height, &row_rect,
                    10, theme.card_bg);
            }

            /* icon */
            {
                struct lumo_rect icon = {row_rect.x + 10,
                    row_rect.y + 10, 20, 20};
                lumo_app_fill_rounded_rect(pixels, width, height, &icon,
                    4, is_dir ? theme.accent :
                    lumo_app_argb(0xFF, 0x77, 0x21, 0x6F));
            }

            lumo_app_draw_text(pixels, width, height, row_rect.x + 42,
                row_rect.y + 12, 2, theme.text, entry->d_name);

            if (!is_dir) {
                char path_buf[1100];
                struct stat st;
                snprintf(path_buf, sizeof(path_buf), "%s/%s",
                    browse_path, entry->d_name);
                if (stat(path_buf, &st) == 0) {
                    char size_buf[16];
                    if (st.st_size < 1024)
                        snprintf(size_buf, sizeof(size_buf), "%ldB",
                            (long)st.st_size);
                    else if (st.st_size < 1024 * 1024)
                        snprintf(size_buf, sizeof(size_buf), "%ldK",
                            (long)(st.st_size / 1024));
                    else
                        snprintf(size_buf, sizeof(size_buf), "%ldM",
                            (long)(st.st_size / (1024 * 1024)));
                    lumo_app_draw_text(pixels, width, height,
                        row_rect.x + row_rect.width - 80, row_rect.y + 12,
                        2, theme.text_dim, size_buf);
                }
            } else {
                lumo_app_draw_text(pixels, width, height,
                    row_rect.x + row_rect.width - 28, row_rect.y + 12,
                    2, theme.text_dim, ">");
            }

            row_y += files_row_height;
            count++;
            total_visible++;
        }
        closedir(dir);

        if (total_visible > max_rows) {
            char scroll_buf[32];
            snprintf(scroll_buf, sizeof(scroll_buf), "%d-%d / %d",
                scroll_off + 1, scroll_off + count, total_visible);
            lumo_app_draw_text(pixels, width, height,
                (int)width - 160, (int)height - 40, 2,
                theme.text_dim, scroll_buf);
        }
        if (count == 0) {
            lumo_app_draw_text(pixels, width, height, 28, row_y, 2,
                theme.text_dim, "EMPTY DIRECTORY");
        }
    } else {
        lumo_app_draw_text(pixels, width, height, 28,
            files_header_height, 2, theme.text_dim,
            "CANNOT OPEN DIRECTORY");
    }

    /* storage info */
    {
        struct statvfs st;
        if (statvfs("/", &st) == 0) {
            char storage_buf[64];
            unsigned long free_mb = (unsigned long)(st.f_bavail *
                (st.f_frsize / 1024)) / 1024;
            unsigned long total_mb = (unsigned long)(st.f_blocks *
                (st.f_frsize / 1024)) / 1024;
            snprintf(storage_buf, sizeof(storage_buf),
                "%lu / %lu MB FREE", free_mb, total_mb);
            lumo_app_draw_text(pixels, width, height, 28,
                (int)height - 40, 2, theme.text_dim, storage_buf);
        }
    }

    /* text file viewer overlay */
    if (ctx != NULL && ctx->text_view_active) {
        struct lumo_app_theme theme;
        lumo_app_theme_get(&theme);

        /* dark overlay */
        for (uint32_t i = 0; i < width * height; i++) {
            uint32_t c = pixels[i];
            pixels[i] = lumo_app_argb(0xFF,
                (uint8_t)((c >> 16) & 0xFF) / 3,
                (uint8_t)((c >> 8) & 0xFF) / 3,
                (uint8_t)(c & 0xFF) / 3);
        }

        /* viewer card */
        int cw = (int)width - 40;
        int ch = (int)height - 80;
        if (cw > 700) cw = 700;
        int cx = ((int)width - cw) / 2;
        int cy = 40;
        struct lumo_rect card = { cx, cy, cw, ch };
        lumo_app_fill_rounded_rect(pixels, width, height, &card, 12,
            theme.card_bg);
        lumo_app_draw_outline(pixels, width, height, &card, 1,
            theme.card_stroke);

        /* title */
        lumo_app_draw_text(pixels, width, height, cx + 12, cy + 8, 2,
            theme.accent, ctx->file_info_name);
        lumo_app_fill_rect(pixels, width, height, cx + 12, cy + 28,
            cw - 24, 1, theme.separator);

        /* text content with scroll */
        int text_y = cy + 36;
        int max_y = cy + ch - 12;
        int line_h = 16;
        int scroll = ctx->scroll_offset; /* reuse scroll_offset for text */
        const char *p = ctx->text_view_content;
        int line_num = 0;

        while (*p != '\0' && text_y < max_y) {
            /* find end of line */
            const char *eol = strchr(p, '\n');
            int len = eol ? (int)(eol - p) : (int)strlen(p);
            int max_chars = (cw - 24) / 7; /* approx char width at scale 1 */
            if (len > max_chars) len = max_chars;

            if (line_num >= scroll) {
                char line_buf[256];
                if (len > (int)sizeof(line_buf) - 1)
                    len = (int)sizeof(line_buf) - 1;
                memcpy(line_buf, p, (size_t)len);
                line_buf[len] = '\0';

                lumo_app_draw_text(pixels, width, height,
                    cx + 12, text_y, 1, theme.text, line_buf);
                text_y += line_h;
            }
            line_num++;
            p = eol ? eol + 1 : p + strlen(p);
        }

        /* truncation notice if content was cut */
        if (strlen(ctx->text_view_content) >= 4095)
            lumo_app_draw_text(pixels, width, height,
                cx + 12, cy + ch - 34, 1,
                lumo_app_argb(0xA0, 0xFF, 0xAA, 0x44),
                "FILE TRUNCATED (SHOWING FIRST 4KB)");

        /* dismiss hint */
        lumo_app_draw_text(pixels, width, height,
            cx + 12, cy + ch - 20, 1, theme.text_dim, "TAP TO CLOSE");
    }

    /* file info overlay */
    if (ctx != NULL && ctx->file_info_visible)
        draw_file_info_overlay(pixels, width, height, ctx);
}
