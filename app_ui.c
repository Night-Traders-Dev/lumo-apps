#define _DEFAULT_SOURCE
#include "lumo/app_render.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

#if defined(__riscv) && defined(__riscv_v_intrinsic)
#include <riscv_vector.h>
#define LUMO_HAS_RVV 1
#endif

void lumo_app_theme_get(struct lumo_app_theme *theme) {
    time_t now = time(NULL);
    struct tm tm_now;
    uint32_t r, g, b;
    localtime_r(&now, &tm_now);
    uint32_t hour = (uint32_t)tm_now.tm_hour;

    if (hour >= 5 && hour < 7) {
        r = 0x14; g = 0x28; b = 0x38;
    } else if (hour >= 7 && hour < 10) {
        r = 0x30; g = 0x10; b = 0x28;
    } else if (hour >= 10 && hour < 14) {
        r = 0x2C; g = 0x00; b = 0x1E;
    } else if (hour >= 14 && hour < 17) {
        r = 0x28; g = 0x14; b = 0x18;
    } else if (hour >= 17 && hour < 19) {
        r = 0x42; g = 0x0C; b = 0x16;
    } else if (hour >= 19 && hour < 21) {
        r = 0x10; g = 0x18; b = 0x30;
    } else {
        r = 0x12; g = 0x08; b = 0x1A;
    }

    theme->bg = ((uint32_t)0xFF << 24) | (r << 16) | (g << 8) | b;
    theme->header_bg = ((uint32_t)0xFF << 24) |
        ((r > 0x08 ? r - 0x08 : 0) << 16) |
        ((g > 0x04 ? g - 0x04 : 0) << 8) |
        (b > 0x06 ? b - 0x06 : 0);
    uint32_t cr = r + 0x0A, cg = g + 0x08, cb = b + 0x0A;
    theme->card_bg = ((uint32_t)0xFF << 24) |
        ((cr > 0xFF ? 0xFF : cr) << 16) |
        ((cg > 0xFF ? 0xFF : cg) << 8) |
        (cb > 0xFF ? 0xFF : cb);
    uint32_t sr = r + 0x20, sg = g + 0x14, sb = b + 0x1C;
    theme->card_stroke = ((uint32_t)0xFF << 24) |
        ((sr > 0xFF ? 0xFF : sr) << 16) |
        ((sg > 0xFF ? 0xFF : sg) << 8) |
        (sb > 0xFF ? 0xFF : sb);
    theme->accent = 0xFFE95420;
    theme->text = 0xFFFFFFFF;
    theme->text_dim = 0xFFAEA79F;
    uint32_t dr = r + 0x30, dg = g + 0x18, db = b + 0x28;
    theme->separator = ((uint32_t)0x40 << 24) |
        ((dr > 0xFF ? 0xFF : dr) << 16) |
        ((dg > 0xFF ? 0xFF : dg) << 8) |
        (db > 0xFF ? 0xFF : db);
}

uint32_t lumo_app_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
        ((uint32_t)g << 8) | (uint32_t)b;
}

/* RISC-V Vector 1.0 accelerated fill when available */
#if defined(__riscv) && defined(__riscv_v) && __riscv_v_intrinsic >= 1000000
#include <riscv_vector.h>
#define LUMO_HAS_RVV 1
#endif

static void lumo_app_fill_span(uint32_t *row_ptr, int count, uint32_t color) {
    if (count <= 0) return;
#ifdef LUMO_HAS_RVV
    /* RVV 1.0: vectorized 32-bit fill — processes VLEN/32 pixels per
     * iteration.  On the Ky X60 with typical VLEN=128, that's 4 pixels
     * per vector op (8x with LMUL=2).  For large fills this is
     * significantly faster than scalar unrolling. */
    {
        size_t n = (size_t)count;
        size_t i = 0;
        while (i < n) {
            size_t vl = __riscv_vsetvl_e32m2(n - i);
            vuint32m2_t vc = __riscv_vmv_v_x_u32m2(color, vl);
            __riscv_vse32_v_u32m2(row_ptr + i, vc, vl);
            i += vl;
        }
        return;
    }
#endif
    if (count <= 8) {
        for (int i = 0; i < count; i++) row_ptr[i] = color;
        return;
    }
    if (color == 0) { memset(row_ptr, 0, (size_t)count * 4); return; }
    {
        uint8_t b0 = (uint8_t)color;
        if (b0 == (uint8_t)(color >> 8) && b0 == (uint8_t)(color >> 16) &&
                b0 == (uint8_t)(color >> 24)) {
            memset(row_ptr, b0, (size_t)count * 4);
            return;
        }
    }
    {
        int i = 0, bulk = count - (count & 7);
        for (; i < bulk; i += 8) {
            row_ptr[i]   = color; row_ptr[i+1] = color;
            row_ptr[i+2] = color; row_ptr[i+3] = color;
            row_ptr[i+4] = color; row_ptr[i+5] = color;
            row_ptr[i+6] = color; row_ptr[i+7] = color;
        }
        for (; i < count; i++) row_ptr[i] = color;
    }
}

void lumo_app_fill_rect(
    uint32_t *pixels, uint32_t width, uint32_t height,
    int x, int y, int rect_width, int rect_height, uint32_t color
) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + rect_width;
    int y1 = y + rect_height;

    if (pixels == NULL || rect_width <= 0 || rect_height <= 0 ||
            width == 0 || height == 0) return;
    if (x0 >= (int)width || y0 >= (int)height) return;
    if (x1 > (int)width) x1 = (int)width;
    if (y1 > (int)height) y1 = (int)height;

    {
        int span = x1 - x0;
        for (int row = y0; row < y1; row++)
            lumo_app_fill_span(pixels + row * (int)width + x0, span, color);
    }
}

void lumo_app_fill_gradient(
    uint32_t *pixels, uint32_t width, uint32_t height,
    const struct lumo_rect *rect, uint32_t top_color, uint32_t bottom_color
) {
    int x0, y0, x1, y1, span, denom;
    int32_t a, r, g, b, da, dr, dg, db;

    if (pixels == NULL || rect == NULL || rect->width <= 0 || rect->height <= 0)
        return;

    x0 = rect->x < 0 ? 0 : rect->x;
    y0 = rect->y < 0 ? 0 : rect->y;
    x1 = rect->x + rect->width;
    y1 = rect->y + rect->height;
    if (x0 >= (int)width || y0 >= (int)height) return;
    if (x1 > (int)width) x1 = (int)width;
    if (y1 > (int)height) y1 = (int)height;
    span = x1 - x0;
    if (span <= 0) return;

    denom = rect->height > 1 ? rect->height - 1 : 1;
    a = (int32_t)(top_color >> 24) << 16;
    r = (int32_t)((top_color >> 16) & 0xFF) << 16;
    g = (int32_t)((top_color >> 8) & 0xFF) << 16;
    b = (int32_t)(top_color & 0xFF) << 16;
    da = (((int32_t)(bottom_color >> 24) - (int32_t)(top_color >> 24)) << 16) / denom;
    dr = (((int32_t)((bottom_color >> 16) & 0xFF) - (int32_t)((top_color >> 16) & 0xFF)) << 16) / denom;
    dg = (((int32_t)((bottom_color >> 8) & 0xFF) - (int32_t)((top_color >> 8) & 0xFF)) << 16) / denom;
    db = (((int32_t)(bottom_color & 0xFF) - (int32_t)(top_color & 0xFF)) << 16) / denom;

    { int skip = y0 - rect->y; a += da * skip; r += dr * skip; g += dg * skip; b += db * skip; }

    for (int row = y0; row < y1; row++) {
        uint32_t c = ((uint32_t)(a >> 16) << 24) | ((uint32_t)(r >> 16) << 16) |
            ((uint32_t)(g >> 16) << 8) | (uint32_t)(b >> 16);
        lumo_app_fill_span(pixels + row * (int)width + x0, span, c);
        a += da; r += dr; g += dg; b += db;
    }
}

static bool lumo_app_rounded_rect_contains(
    const struct lumo_rect *rect, int radius, int x, int y
) {
    int lx, ly, dx, dy, mx, my;
    if (rect == NULL || rect->width <= 0 || rect->height <= 0) return false;
    lx = x - rect->x; ly = y - rect->y;
    if (lx < 0 || ly < 0 || lx >= rect->width || ly >= rect->height) return false;
    if (radius <= 0) return true;
    if ((lx >= radius && lx < rect->width - radius) ||
            (ly >= radius && ly < rect->height - radius)) return true;
    mx = rect->width - radius - 1; my = rect->height - radius - 1;
    dx = lx < radius ? lx - radius : lx - mx;
    dy = ly < radius ? ly - radius : ly - my;
    return dx * dx + dy * dy <= radius * radius;
}

void lumo_app_fill_rounded_rect(
    uint32_t *pixels, uint32_t width, uint32_t height,
    const struct lumo_rect *rect, uint32_t radius, uint32_t color
) {
    int r2;
    if (pixels == NULL || rect == NULL || rect->width <= 0 || rect->height <= 0)
        return;
    if (radius == 0) {
        lumo_app_fill_rect(pixels, width, height,
            rect->x, rect->y, rect->width, rect->height, color);
        return;
    }
    int x0 = rect->x < 0 ? 0 : rect->x;
    int y0 = rect->y < 0 ? 0 : rect->y;
    int x1 = rect->x + rect->width; if (x1 > (int)width) x1 = (int)width;
    int y1 = rect->y + rect->height; if (y1 > (int)height) y1 = (int)height;
    r2 = (int)(radius * radius);
    for (int row = y0; row < y1; row++) {
        int local_y = row - rect->y;
        int row_x0 = x0, row_x1 = x1;
        if (local_y < (int)radius) {
            int dy = (int)radius - local_y, dx2 = r2 - dy * dy;
            int inset = (int)radius;
            if (dx2 > 0) { int s = 0; while ((s+1)*(s+1) <= dx2) s++; inset = (int)radius - s; }
            if (rect->x + inset > row_x0) row_x0 = rect->x + inset;
            if (rect->x + rect->width - inset < row_x1) row_x1 = rect->x + rect->width - inset;
        } else if (local_y >= rect->height - (int)radius) {
            int dy = local_y - (rect->height - (int)radius - 1), dx2 = r2 - dy * dy;
            int inset = (int)radius;
            if (dx2 > 0) { int s = 0; while ((s+1)*(s+1) <= dx2) s++; inset = (int)radius - s; }
            if (rect->x + inset > row_x0) row_x0 = rect->x + inset;
            if (rect->x + rect->width - inset < row_x1) row_x1 = rect->x + rect->width - inset;
        }
        if (row_x1 > row_x0)
            lumo_app_fill_span(pixels + row * (int)width + row_x0, row_x1 - row_x0, color);
    }
}

void lumo_app_draw_outline(
    uint32_t *pixels, uint32_t width, uint32_t height,
    const struct lumo_rect *rect, int thickness, uint32_t color
) {
    lumo_app_fill_rect(pixels, width, height, rect->x, rect->y,
        rect->width, thickness, color);
    lumo_app_fill_rect(pixels, width, height, rect->x,
        rect->y + rect->height - thickness, rect->width, thickness, color);
    lumo_app_fill_rect(pixels, width, height, rect->x, rect->y,
        thickness, rect->height, color);
    lumo_app_fill_rect(pixels, width, height,
        rect->x + rect->width - thickness, rect->y, thickness, rect->height, color);
}

static bool lumo_app_glyph_rows(char ch, uint8_t rows[7]) {
    switch ((unsigned char)ch) {
    case 'A': memcpy(rows, (uint8_t[]){0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, 7); return true;
    case 'B': memcpy(rows, (uint8_t[]){0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, 7); return true;
    case 'C': memcpy(rows, (uint8_t[]){0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, 7); return true;
    case 'D': memcpy(rows, (uint8_t[]){0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, 7); return true;
    case 'E': memcpy(rows, (uint8_t[]){0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, 7); return true;
    case 'F': memcpy(rows, (uint8_t[]){0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, 7); return true;
    case 'G': memcpy(rows, (uint8_t[]){0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}, 7); return true;
    case 'H': memcpy(rows, (uint8_t[]){0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, 7); return true;
    case 'I': memcpy(rows, (uint8_t[]){0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, 7); return true;
    case 'J': memcpy(rows, (uint8_t[]){0x07,0x02,0x02,0x02,0x12,0x12,0x0C}, 7); return true;
    case 'K': memcpy(rows, (uint8_t[]){0x11,0x12,0x14,0x18,0x14,0x12,0x11}, 7); return true;
    case 'L': memcpy(rows, (uint8_t[]){0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, 7); return true;
    case 'M': memcpy(rows, (uint8_t[]){0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, 7); return true;
    case 'N': memcpy(rows, (uint8_t[]){0x11,0x19,0x15,0x13,0x11,0x11,0x11}, 7); return true;
    case 'O': memcpy(rows, (uint8_t[]){0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, 7); return true;
    case 'P': memcpy(rows, (uint8_t[]){0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, 7); return true;
    case 'Q': memcpy(rows, (uint8_t[]){0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, 7); return true;
    case 'R': memcpy(rows, (uint8_t[]){0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, 7); return true;
    case 'S': memcpy(rows, (uint8_t[]){0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, 7); return true;
    case 'T': memcpy(rows, (uint8_t[]){0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, 7); return true;
    case 'U': memcpy(rows, (uint8_t[]){0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, 7); return true;
    case 'V': memcpy(rows, (uint8_t[]){0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, 7); return true;
    case 'W': memcpy(rows, (uint8_t[]){0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, 7); return true;
    case 'X': memcpy(rows, (uint8_t[]){0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, 7); return true;
    case 'Y': memcpy(rows, (uint8_t[]){0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, 7); return true;
    case 'Z': memcpy(rows, (uint8_t[]){0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, 7); return true;
    case 'a': memcpy(rows, (uint8_t[]){0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, 7); return true;
    case 'b': memcpy(rows, (uint8_t[]){0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, 7); return true;
    case 'c': memcpy(rows, (uint8_t[]){0x00,0x00,0x0E,0x10,0x10,0x10,0x0E}, 7); return true;
    case 'd': memcpy(rows, (uint8_t[]){0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, 7); return true;
    case 'e': memcpy(rows, (uint8_t[]){0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, 7); return true;
    case 'f': memcpy(rows, (uint8_t[]){0x06,0x08,0x08,0x1E,0x08,0x08,0x08}, 7); return true;
    case 'g': memcpy(rows, (uint8_t[]){0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}, 7); return true;
    case 'h': memcpy(rows, (uint8_t[]){0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, 7); return true;
    case 'i': memcpy(rows, (uint8_t[]){0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, 7); return true;
    case 'j': memcpy(rows, (uint8_t[]){0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, 7); return true;
    case 'k': memcpy(rows, (uint8_t[]){0x10,0x10,0x12,0x14,0x18,0x14,0x12}, 7); return true;
    case 'l': memcpy(rows, (uint8_t[]){0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, 7); return true;
    case 'm': memcpy(rows, (uint8_t[]){0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, 7); return true;
    case 'n': memcpy(rows, (uint8_t[]){0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, 7); return true;
    case 'o': memcpy(rows, (uint8_t[]){0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, 7); return true;
    case 'p': memcpy(rows, (uint8_t[]){0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, 7); return true;
    case 'q': memcpy(rows, (uint8_t[]){0x00,0x00,0x0F,0x11,0x0F,0x01,0x01}, 7); return true;
    case 'r': memcpy(rows, (uint8_t[]){0x00,0x00,0x16,0x19,0x10,0x10,0x10}, 7); return true;
    case 's': memcpy(rows, (uint8_t[]){0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}, 7); return true;
    case 't': memcpy(rows, (uint8_t[]){0x08,0x08,0x1E,0x08,0x08,0x08,0x06}, 7); return true;
    case 'u': memcpy(rows, (uint8_t[]){0x00,0x00,0x11,0x11,0x11,0x11,0x0E}, 7); return true;
    case 'v': memcpy(rows, (uint8_t[]){0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, 7); return true;
    case 'w': memcpy(rows, (uint8_t[]){0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, 7); return true;
    case 'x': memcpy(rows, (uint8_t[]){0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, 7); return true;
    case 'y': memcpy(rows, (uint8_t[]){0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, 7); return true;
    case 'z': memcpy(rows, (uint8_t[]){0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, 7); return true;
    case '0': memcpy(rows, (uint8_t[]){0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, 7); return true;
    case '1': memcpy(rows, (uint8_t[]){0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, 7); return true;
    case '2': memcpy(rows, (uint8_t[]){0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, 7); return true;
    case '3': memcpy(rows, (uint8_t[]){0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, 7); return true;
    case '4': memcpy(rows, (uint8_t[]){0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, 7); return true;
    case '5': memcpy(rows, (uint8_t[]){0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}, 7); return true;
    case '6': memcpy(rows, (uint8_t[]){0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}, 7); return true;
    case '7': memcpy(rows, (uint8_t[]){0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, 7); return true;
    case '8': memcpy(rows, (uint8_t[]){0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, 7); return true;
    case '9': memcpy(rows, (uint8_t[]){0x0E,0x11,0x11,0x0F,0x01,0x02,0x1C}, 7); return true;
    case ':': memcpy(rows, (uint8_t[]){0x00,0x04,0x04,0x00,0x04,0x04,0x00}, 7); return true;
    case '-': memcpy(rows, (uint8_t[]){0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, 7); return true;
    case '/': memcpy(rows, (uint8_t[]){0x01,0x02,0x04,0x08,0x10,0x00,0x00}, 7); return true;
    case '+': memcpy(rows, (uint8_t[]){0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, 7); return true;
    case ' ': memset(rows, 0, 7); return true;
    case '.': memcpy(rows, (uint8_t[]){0x00,0x00,0x00,0x00,0x00,0x00,0x04}, 7); return true;
    case '!': memcpy(rows, (uint8_t[]){0x04,0x04,0x04,0x04,0x04,0x00,0x04}, 7); return true;
    case '(': memcpy(rows, (uint8_t[]){0x02,0x04,0x04,0x04,0x04,0x04,0x02}, 7); return true;
    case ')': memcpy(rows, (uint8_t[]){0x08,0x04,0x04,0x04,0x04,0x04,0x08}, 7); return true;
    case '%': memcpy(rows, (uint8_t[]){0x11,0x12,0x04,0x04,0x04,0x09,0x11}, 7); return true;
    case '@': memcpy(rows, (uint8_t[]){0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, 7); return true;
    case '#': memcpy(rows, (uint8_t[]){0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}, 7); return true;
    case '_': memcpy(rows, (uint8_t[]){0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, 7); return true;
    case '>': memcpy(rows, (uint8_t[]){0x08,0x04,0x02,0x01,0x02,0x04,0x08}, 7); return true;
    case '<': memcpy(rows, (uint8_t[]){0x02,0x04,0x08,0x10,0x08,0x04,0x02}, 7); return true;
    case '=': memcpy(rows, (uint8_t[]){0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, 7); return true;
    case '*': memcpy(rows, (uint8_t[]){0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}, 7); return true;
    case '~': memcpy(rows, (uint8_t[]){0x00,0x00,0x08,0x15,0x02,0x00,0x00}, 7); return true;
    case '$': memcpy(rows, (uint8_t[]){0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, 7); return true;
    case '&': memcpy(rows, (uint8_t[]){0x0C,0x12,0x0C,0x12,0x11,0x11,0x0E}, 7); return true;
    case '[': memcpy(rows, (uint8_t[]){0x06,0x04,0x04,0x04,0x04,0x04,0x06}, 7); return true;
    case ']': memcpy(rows, (uint8_t[]){0x0C,0x04,0x04,0x04,0x04,0x04,0x0C}, 7); return true;
    default: memset(rows, 0, 7); return false;
    }
}

void lumo_app_draw_text(
    uint32_t *pixels, uint32_t width, uint32_t height,
    int x, int y, int scale, uint32_t color, const char *text
) {
    int cursor_x = x;
    uint8_t glyph[7];
    if (pixels == NULL || text == NULL || scale <= 0) return;
    for (size_t i = 0; text[i] != '\0'; i++) {
        lumo_app_glyph_rows(text[i], glyph);
        for (int grow = 0; grow < 7; grow++) {
            uint8_t bits = glyph[grow];
            if (bits == 0) continue;
            int py = y + grow * scale;
            if (py >= (int)height) break;
            if (py + scale <= 0) continue;
            int col = 0;
            while (col < 5) {
                if ((bits & (1u << (4 - col))) == 0) { col++; continue; }
                int span_start = col;
                while (col < 5 && (bits & (1u << (4 - col))) != 0) col++;
                int px = cursor_x + span_start * scale;
                int pw = (col - span_start) * scale;
                for (int sy = 0; sy < scale; sy++) {
                    int ry = py + sy;
                    if (ry < 0 || ry >= (int)height) continue;
                    int cx0 = px < 0 ? 0 : px;
                    int cx1 = px + pw > (int)width ? (int)width : px + pw;
                    if (cx1 > cx0)
                        lumo_app_fill_span(pixels + ry * (int)width + cx0,
                            cx1 - cx0, color);
                }
            }
        }
        cursor_x += scale * 6;
    }
}

void lumo_app_draw_text_centered(
    uint32_t *pixels, uint32_t width, uint32_t height,
    const struct lumo_rect *rect, int scale, uint32_t color, const char *text
) {
    if (rect == NULL || text == NULL) return;
    int tw = (int)strlen(text) * scale * 6 - scale;
    int th = scale * 7;
    lumo_app_draw_text(pixels, width, height,
        rect->x + (rect->width - tw) / 2,
        rect->y + (rect->height - th) / 2, scale, color, text);
}

/* Dead code: close button removed — navigation uses bottom-edge swipe.
 * Declaration kept for ABI compatibility; body is intentionally empty. */
void lumo_app_draw_close_button(
    uint32_t *pixels, uint32_t width, uint32_t height, bool close_active
) {
    (void)pixels;
    (void)width;
    (void)height;
    (void)close_active;
}

void lumo_app_draw_background(
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    struct lumo_rect full = {0, 0, (int)width, (int)height};
    lumo_app_theme_get(&theme);
    /* fill_gradient writes every pixel — skip separate memset */
    lumo_app_fill_gradient(pixels, width, height, &full,
        theme.bg, theme.header_bg);
}

static void lumo_app_card_text(
    enum lumo_app_id app_id, uint32_t card_index,
    const char **title, const char **body
) {
    static const char *const titles[][3] = {
        [LUMO_APP_PHONE] = {"Favorites", "Recents", "Dialer"},
        [LUMO_APP_MESSAGES] = {"Unread", "Pinned", "Compose"},
        [LUMO_APP_BROWSER] = {"Quick Start", "Tabs", "Reading"},
        [LUMO_APP_CAMERA] = {"Photo", "Video", "Gallery"},
        [LUMO_APP_MAPS] = {"Home", "Nearby", "Route"},
        [LUMO_APP_MUSIC] = {"Now Playing", "Library", "Mixes"},
        [LUMO_APP_PHOTOS] = {"Memories", "Albums", "Shared"},
        [LUMO_APP_VIDEOS] = {"Continue", "Downloads", "Queue"},
    };
    static const char *const bodies[][3] = {
        [LUMO_APP_PHONE] = {"Mira, Kai, and Dev", "Last call 2m ago", "Tap to open keypad"},
        [LUMO_APP_MESSAGES] = {"2 new threads", "Family and team", "Start a new message"},
        [LUMO_APP_BROWSER] = {"Open your saved starts", "3 tabs waiting", "Continue later list"},
        [LUMO_APP_CAMERA] = {"Ready for quick capture", "Last mode used", "Recent shots saved"},
        [LUMO_APP_MAPS] = {"18 min away", "Coffee and fuel", "Fastest arrival"},
        [LUMO_APP_MUSIC] = {"Resume favorite album", "Recent artists", "Fresh queue"},
        [LUMO_APP_PHOTOS] = {"Highlights for today", "Trips and camera roll", "Latest updates"},
        [LUMO_APP_VIDEOS] = {"Resume in one tap", "Offline ready", "Watch later list"},
    };

    if (title == NULL || body == NULL || card_index > 2) return;
    if (app_id < sizeof(titles) / sizeof(titles[0]) && titles[app_id][0] != NULL) {
        *title = titles[app_id][card_index];
        *body = bodies[app_id][card_index];
    } else {
        *title = "Panel";
        *body = "Ready";
    }
}

void lumo_app_render_stub(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    enum lumo_app_id app_id = ctx != NULL ? ctx->app_id : LUMO_APP_PHONE;
    bool close_active = ctx != NULL ? ctx->close_active : false;
    struct lumo_rect hero;
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    uint32_t accent = lumo_app_accent_argb(app_id);
    uint32_t text_primary = theme.text;
    uint32_t text_secondary = theme.text_dim;
    uint32_t panel_fill = theme.card_bg;
    uint32_t panel_stroke = theme.card_stroke;

    if (pixels == NULL || width == 0 || height == 0) return;
    lumo_app_draw_background(pixels, width, height);

    {
        struct lumo_rect badge = {28, 28, 180, 28};
        lumo_app_fill_rounded_rect(pixels, width, height, &badge, 14,
            lumo_app_argb(0xFF, 0x2C, 0x00, 0x1E));
        lumo_app_draw_text(pixels, width, height, badge.x + 14,
            badge.y + 8, 2, text_secondary, "LUMO NATIVE");
    }

    lumo_app_draw_text(pixels, width, height, 28, 86, 4, text_primary,
        lumo_app_title(app_id));
    lumo_app_draw_text(pixels, width, height, 28, 128, 2, text_secondary,
        lumo_app_subtitle(app_id));

    hero.x = 28; hero.y = 170; hero.width = (int)width - 56;
    hero.height = (int)(height / 4);
    lumo_app_fill_rounded_rect(pixels, width, height, &hero, 28, panel_fill);
    lumo_app_draw_outline(pixels, width, height, &hero, 2, panel_stroke);
    lumo_app_draw_text(pixels, width, height, hero.x + 24, hero.y + 24, 2,
        text_secondary, "NOW READY");
    lumo_app_fill_rect(pixels, width, height, hero.x + 24, hero.y + 62,
        hero.width - 48, 8, accent);
    lumo_app_draw_text(pixels, width, height, hero.x + 24, hero.y + 88, 3,
        text_primary, lumo_app_title(app_id));
    lumo_app_draw_text(pixels, width, height, hero.x + 24, hero.y + 130, 2,
        text_secondary, "Touch-first native client");

    for (uint32_t ci = 0; ci < 3; ci++) {
        struct lumo_rect card = {
            .x = 28 + (int)ci * ((int)width - 84) / 3,
            .y = hero.y + hero.height + 24,
            .width = ((int)width - 112) / 3,
            .height = (int)height - (hero.y + hero.height + 56),
        };
        const char *ct = NULL, *cb = NULL;
        if (card.width <= 0 || card.height <= 0) continue;
        lumo_app_fill_rounded_rect(pixels, width, height, &card, 22,
            lumo_app_argb(0xFF, 0x2C, 0x16, 0x28));
        lumo_app_draw_outline(pixels, width, height, &card, 2, panel_stroke);
        lumo_app_fill_rect(pixels, width, height, card.x + 18, card.y + 18,
            card.width - 36, 6, accent);
        lumo_app_card_text(app_id, ci, &ct, &cb);
        lumo_app_draw_text(pixels, width, height, card.x + 18, card.y + 36, 2,
            text_secondary, ct ? ct : "PANEL");
        lumo_app_draw_text(pixels, width, height, card.x + 18, card.y + 68, 2,
            text_primary, cb ? cb : "Ready");
    }

    /* close button removed — use bottom-edge swipe */
}

void lumo_app_render(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    if (ctx == NULL) return;

    switch (ctx->app_id) {
    case LUMO_APP_CLOCK:
        lumo_app_render_clock(ctx, pixels, width, height); return;
    case LUMO_APP_FILES:
        lumo_app_render_files(ctx, pixels, width, height); return;
    case LUMO_APP_SETTINGS:
        lumo_app_render_settings(ctx, pixels, width, height); return;
    case LUMO_APP_SYSMON:
        lumo_app_render_sysmon(ctx, pixels, width, height); return;
    case LUMO_APP_GITHUB:
        lumo_app_render_github(ctx, pixels, width, height); return;
    case LUMO_APP_NOTES:
        lumo_app_render_notes(ctx, pixels, width, height); return;
    case LUMO_APP_MESSAGES:
        lumo_app_render_terminal(ctx, pixels, width, height); return;
    case LUMO_APP_MUSIC:
        lumo_app_render_music(ctx, pixels, width, height); return;
    case LUMO_APP_PHOTOS:
        lumo_app_render_photos(ctx, pixels, width, height); return;
    case LUMO_APP_VIDEOS:
        lumo_app_render_videos(ctx, pixels, width, height); return;
    case LUMO_APP_BROWSER:
        lumo_app_render_browser(ctx, pixels, width, height); return;
    case LUMO_APP_PHONE:
        lumo_app_render_phone(ctx, pixels, width, height); return;
    case LUMO_APP_CAMERA:
        lumo_app_render_camera(ctx, pixels, width, height); return;
    case LUMO_APP_MAPS:
        lumo_app_render_maps(ctx, pixels, width, height); return;
    case LUMO_APP_CALCULATOR:
        lumo_app_render_calculator(ctx, pixels, width, height); return;
    case LUMO_APP_CALENDAR:
        lumo_app_render_calendar(ctx, pixels, width, height); return;
    case LUMO_APP_WEATHER:
        lumo_app_render_weather(ctx, pixels, width, height); return;
    case LUMO_APP_CONTACTS:
        lumo_app_render_contacts(ctx, pixels, width, height); return;
    case LUMO_APP_RECORDER:
        lumo_app_render_recorder(ctx, pixels, width, height); return;
    case LUMO_APP_TASKS:
        lumo_app_render_tasks(ctx, pixels, width, height); return;
    case LUMO_APP_DOWNLOADS:
        lumo_app_render_downloads(ctx, pixels, width, height); return;
    case LUMO_APP_PACKAGE:
        lumo_app_render_package(ctx, pixels, width, height); return;
    case LUMO_APP_SYSLOG:
        lumo_app_render_syslog(ctx, pixels, width, height); return;
    case LUMO_APP_PDF:
        lumo_app_render_pdf(ctx, pixels, width, height); return;
    case LUMO_APP_SETUP:
        lumo_app_render_setup(ctx, pixels, width, height); return;
    default:
        lumo_app_render_stub(ctx, pixels, width, height); return;
    }
}
