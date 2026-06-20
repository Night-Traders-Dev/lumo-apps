/*
 * app_stubs.c — placeholder implementations for app modules not yet
 * fully implemented (browser, phone, camera, maps).  Shows a branded
 * "Coming Soon" screen instead of a blank surface.
 */

#include "lumo/app.h"
#include "lumo/app_render.h"

#include <stdint.h>
#include <string.h>

static void draw_coming_soon(
    uint32_t *pixels, uint32_t width, uint32_t height,
    const char *app_name
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    /* gradient background */
    struct lumo_rect full = {0, 0, (int)width, (int)height};
    memset(pixels, 0, (size_t)width * height * 4);
    lumo_app_fill_gradient(pixels, width, height, &full,
        theme.header_bg, theme.bg);

    /* app name centered */
    {
        struct lumo_rect name_rect = {0, (int)height / 2 - 50,
            (int)width, 40};
        lumo_app_draw_text_centered(pixels, width, height, &name_rect,
            4, theme.accent, app_name);
    }

    /* coming soon */
    {
        struct lumo_rect msg_rect = {0, (int)height / 2, (int)width, 24};
        lumo_app_draw_text_centered(pixels, width, height, &msg_rect,
            2, theme.text_dim, "COMING SOON");
    }

    /* hint */
    {
        struct lumo_rect hint_rect = {0, (int)height - 48, (int)width, 20};
        lumo_app_draw_text_centered(pixels, width, height, &hint_rect,
            1, theme.text_dim, "SWIPE TO GO BACK");
    }
}

/* browser is in app_browser.c — no stub needed */

void lumo_app_render_phone(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height)
{
    (void)ctx;
    draw_coming_soon(pixels, width, height, "PHONE");
}

void lumo_app_render_camera(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height)
{
    (void)ctx;
    draw_coming_soon(pixels, width, height, "CAMERA");
}

/* maps render + button_at are in app_maps.c */

/* browser_button_at is in app_browser.c */

int lumo_app_phone_button_at(
    uint32_t width, uint32_t height,
    double x, double y, int tab)
{
    (void)width; (void)height; (void)x; (void)y; (void)tab;
    return -1;
}

int lumo_app_camera_button_at(
    uint32_t width, uint32_t height,
    double x, double y, bool gallery_mode)
{
    (void)width; (void)height; (void)x; (void)y; (void)gallery_mode;
    return -1;
}

