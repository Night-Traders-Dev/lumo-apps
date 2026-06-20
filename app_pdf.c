/*
 * app_pdf.c — Native PDF reader for Lumo OS.
 *
 * Uses poppler-glib to render PDF pages into the SHM pixel buffer.
 * Features: page navigation, pinch-to-zoom, vertical scroll,
 * page counter, and touch-friendly controls.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <cairo.h>
#include <poppler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── PDF state (global, one document at a time) ──────────────────── */

static struct {
    PopplerDocument *doc;
    int page_count;
    int current_page;
    double zoom;
    int scroll_y;       /* vertical scroll offset within page */
    char title[128];
    char filepath[1024];
    bool loaded;

    /* rendered page cache */
    uint32_t *page_pixels;
    int page_w, page_h;
    int cached_page;
    double cached_zoom;
} pdf;

/* ── load/render ─────────────────────────────────────────────────── */

void lumo_pdf_open(const char *path) {
    if (path == NULL || path[0] == '\0') return;

    /* close previous */
    if (pdf.doc) {
        g_object_unref(pdf.doc);
        pdf.doc = NULL;
    }
    free(pdf.page_pixels);
    pdf.page_pixels = NULL;
    pdf.loaded = false;

    /* poppler needs a URI */
    char uri[2048];
    if (strncmp(path, "file://", 7) == 0)
        snprintf(uri, sizeof(uri), "%s", path);
    else
        snprintf(uri, sizeof(uri), "file://%s", path);

    GError *err = NULL;
    pdf.doc = poppler_document_new_from_file(uri, NULL, &err);
    if (!pdf.doc) {
        fprintf(stderr, "lumo-pdf: failed to open %s: %s\n",
            path, err ? err->message : "unknown");
        if (err) g_error_free(err);
        return;
    }

    pdf.page_count = poppler_document_get_n_pages(pdf.doc);
    pdf.current_page = 0;
    pdf.zoom = 1.0;
    pdf.scroll_y = 0;
    pdf.cached_page = -1;
    pdf.cached_zoom = 0;
    snprintf(pdf.filepath, sizeof(pdf.filepath), "%s", path);

    const char *t = poppler_document_get_title(pdf.doc);
    if (t && t[0])
        snprintf(pdf.title, sizeof(pdf.title), "%s", t);
    else {
        const char *base = strrchr(path, '/');
        snprintf(pdf.title, sizeof(pdf.title), "%s",
            base ? base + 1 : path);
    }

    pdf.loaded = true;
    fprintf(stderr, "lumo-pdf: opened %s (%d pages)\n",
        pdf.title, pdf.page_count);
}

static void render_page_to_cache(int page_idx, double zoom,
    uint32_t viewport_w)
{
    if (!pdf.doc || page_idx < 0 || page_idx >= pdf.page_count)
        return;
    if (pdf.cached_page == page_idx &&
            fabs(pdf.cached_zoom - zoom) < 0.01)
        return;

    PopplerPage *page = poppler_document_get_page(pdf.doc, page_idx);
    if (!page) return;

    double pw, ph;
    poppler_page_get_size(page, &pw, &ph);

    /* scale to fit viewport width, then apply zoom */
    double base_scale = (double)viewport_w / pw;
    double scale = base_scale * zoom;
    int rw = (int)(pw * scale);
    int rh = (int)(ph * scale);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;

    /* render via cairo */
    cairo_surface_t *cs = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, rw, rh);
    cairo_t *cr = cairo_create(cs);

    /* white background */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    /* scale and render */
    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);

    cairo_destroy(cr);
    g_object_unref(page);

    /* copy to cache buffer */
    free(pdf.page_pixels);
    pdf.page_pixels = malloc((size_t)rw * (size_t)rh * 4);
    if (!pdf.page_pixels) {
        cairo_surface_destroy(cs);
        return;
    }

    cairo_surface_flush(cs);
    unsigned char *data = cairo_image_surface_get_data(cs);
    int stride = cairo_image_surface_get_stride(cs);

    for (int y = 0; y < rh; y++) {
        uint32_t *src = (uint32_t *)(data + y * stride);
        uint32_t *dst = pdf.page_pixels + y * rw;
        memcpy(dst, src, (size_t)rw * 4);
    }

    cairo_surface_destroy(cs);
    pdf.page_w = rw;
    pdf.page_h = rh;
    pdf.cached_page = page_idx;
    pdf.cached_zoom = zoom;
}

/* ── hit-test ────────────────────────────────────────────────────── */

/* 0=prev page, 1=next page, 2=zoom in, 3=zoom out, -1=nothing */
int lumo_app_pdf_button_at(uint32_t width, uint32_t height,
    double x, double y)
{
    int w = (int)width, h = (int)height;
    int tx = (int)x, ty = (int)y;
    int toolbar_h = 48;

    /* toolbar at bottom */
    if (ty >= h - toolbar_h) {
        int btn_w = w / 4;
        if (tx < btn_w) return 0;           /* prev */
        if (tx < btn_w * 2) return 3;       /* zoom out */
        if (tx < btn_w * 3) return 2;       /* zoom in */
        return 1;                            /* next */
    }
    return -1;
}

/* ── render ──────────────────────────────────────────────────────── */

void lumo_app_render_pdf(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;
    int toolbar_h = 48;
    int content_h = h - toolbar_h;

    lumo_app_draw_background(pixels, width, height);

    if (!pdf.loaded || !pdf.doc) {
        /* no PDF loaded */
        lumo_app_draw_text(pixels, width, height, 16, h / 2 - 20, 3,
            theme.accent, "PDF READER");
        lumo_app_draw_text(pixels, width, height, 16, h / 2 + 16, 2,
            theme.text_dim, "OPEN A PDF FROM THE FILE MANAGER");
        goto draw_toolbar;
    }

    /* apply zoom from context */
    double zoom = pdf.zoom;
    if (ctx != NULL && ctx->zoom_scale > 0.0)
        zoom = ctx->zoom_scale;

    /* render current page */
    int content_w = w - 16; /* 8px padding each side */
    render_page_to_cache(pdf.current_page, zoom, (uint32_t)content_w);

    if (pdf.page_pixels && pdf.page_w > 0 && pdf.page_h > 0) {
        /* clamp scroll */
        int scroll = pdf.scroll_y;
        if (ctx != NULL) scroll = ctx->scroll_offset;
        int max_scroll = pdf.page_h - content_h;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll < 0) scroll = 0;
        if (scroll > max_scroll) scroll = max_scroll;

        /* blit page pixels to screen (centered horizontally) */
        int ox = (w - pdf.page_w) / 2;
        if (ox < 0) ox = 0;

        for (int py = 0; py < content_h && py + scroll < pdf.page_h; py++) {
            int src_y = py + scroll;
            if (src_y < 0 || src_y >= pdf.page_h) continue;

            uint32_t *src_row = pdf.page_pixels + src_y * pdf.page_w;
            uint32_t *dst_row = pixels + py * (int)width;

            int copy_w = pdf.page_w;
            if (ox + copy_w > w) copy_w = w - ox;
            if (copy_w <= 0) continue;

            /* blit with horizontal offset */
            for (int px = 0; px < copy_w; px++) {
                int dx = ox + px;
                if (dx >= 0 && dx < w)
                    dst_row[dx] = src_row[px];
            }
        }
    }

draw_toolbar:
    /* bottom toolbar */
    lumo_app_fill_rect(pixels, width, height, 0, h - toolbar_h,
        w, toolbar_h, theme.header_bg);
    lumo_app_fill_rect(pixels, width, height, 0, h - toolbar_h,
        w, 1, theme.separator);

    int btn_w = w / 4;

    /* prev button */
    {
        struct lumo_rect btn = {4, h - toolbar_h + 6, btn_w - 8, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            pdf.current_page > 0 ? theme.card_bg
                : theme.bg);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 3,
            pdf.current_page > 0 ? theme.text : theme.text_dim, "<");
    }

    /* zoom out */
    {
        struct lumo_rect btn = {btn_w + 4, h - toolbar_h + 6,
            btn_w - 8, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 3,
            theme.text, "-");
    }

    /* zoom in */
    {
        struct lumo_rect btn = {btn_w * 2 + 4, h - toolbar_h + 6,
            btn_w - 8, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            theme.card_bg);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 3,
            theme.text, "+");
    }

    /* next button */
    {
        struct lumo_rect btn = {btn_w * 3 + 4, h - toolbar_h + 6,
            btn_w - 8, 36};
        lumo_app_fill_rounded_rect(pixels, width, height, &btn, 8,
            pdf.current_page < pdf.page_count - 1 ? theme.card_bg
                : theme.bg);
        lumo_app_draw_text_centered(pixels, width, height, &btn, 3,
            pdf.current_page < pdf.page_count - 1 ? theme.text
                : theme.text_dim, ">");
    }

    /* page counter (centered) */
    if (pdf.loaded) {
        char counter[32];
        snprintf(counter, sizeof(counter), "%d / %d",
            pdf.current_page + 1, pdf.page_count);
        int tw = (int)strlen(counter) * 2 * 6;
        lumo_app_draw_text(pixels, width, height,
            (w - tw) / 2, h - toolbar_h - 16, 2, theme.text_dim, counter);

        /* title at very top */
        lumo_app_draw_text(pixels, width, height, 8, 4, 1,
            theme.text_dim, pdf.title);
    }
}

/* ── touch handling ──────────────────────────────────────────────── */

void lumo_app_pdf_handle_tap(int btn) {
    if (!pdf.loaded) return;

    switch (btn) {
    case 0: /* prev page */
        if (pdf.current_page > 0) {
            pdf.current_page--;
            pdf.scroll_y = 0;
        }
        break;
    case 1: /* next page */
        if (pdf.current_page < pdf.page_count - 1) {
            pdf.current_page++;
            pdf.scroll_y = 0;
        }
        break;
    case 2: /* zoom in */
        pdf.zoom *= 1.25;
        if (pdf.zoom > 5.0) pdf.zoom = 5.0;
        break;
    case 3: /* zoom out */
        pdf.zoom /= 1.25;
        if (pdf.zoom < 0.25) pdf.zoom = 0.25;
        break;
    }
}

void lumo_app_pdf_scroll(int direction) {
    pdf.scroll_y += direction * 40;
    if (pdf.scroll_y < 0) pdf.scroll_y = 0;
}

bool lumo_app_pdf_is_loaded(void) {
    return pdf.loaded;
}
