/*
 * app_weather.c — Weather app for Lumo OS.
 *
 * Fetches current conditions + 3-day forecast from wttr.in JSON API.
 * Features: weather condition icons (sun/cloud/rain/snow/thunder),
 * current temp, feels like, humidity, wind, UV, pressure, visibility,
 * and daily forecast cards with hi/lo temps.
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <curl/curl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── weather data ────────────────────────────────────────────────── */

struct weather_day {
    char date[12];       /* "2026-04-05" */
    char day_name[4];    /* "SAT" */
    int max_f, min_f;
    char condition[32];
    int code;            /* weather code for icon */
};

static struct {
    /* current */
    int temp_f;
    int feels_f;
    int humidity;
    int wind_mph;
    char wind_dir[8];
    char condition[48];
    int uv_index;
    int visibility;
    int pressure;
    int code;
    /* forecast */
    struct weather_day forecast[3];
    int forecast_count;
    /* state */
    bool loaded;
    bool fetching;
    time_t last_fetch;
} wx;

/* ── minimal JSON field extraction ───────────────────────────────── */

static const char *json_find(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p = strchr(p + strlen(search), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int json_int(const char *json, const char *key) {
    const char *p = json_find(json, key);
    if (!p) return 0;
    if (*p == '"') p++;
    return atoi(p);
}

static void json_str(const char *json, const char *key, char *out, int sz) {
    const char *p = json_find(json, key);
    if (!p) { out[0] = '\0'; return; }
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < sz - 1) out[i++] = *p++;
        out[i] = '\0';
    } else {
        int i = 0;
        while (*p && *p != ',' && *p != '}' && i < sz - 1) out[i++] = *p++;
        out[i] = '\0';
    }
}

/* map weatherCode to simplified code: 0=sunny 1=partly 2=cloudy
 * 3=rain 4=thunder 5=snow 6=fog */
static int weather_icon_code(int wmo_code) {
    if (wmo_code == 113) return 0;  /* sunny */
    if (wmo_code == 116) return 1;  /* partly cloudy */
    if (wmo_code == 119 || wmo_code == 122) return 2;  /* cloudy */
    if (wmo_code >= 176 && wmo_code <= 356) return 3;  /* rain */
    if (wmo_code >= 386 && wmo_code <= 395) return 4;  /* thunder */
    if (wmo_code >= 227 && wmo_code <= 371) {
        if (wmo_code >= 320 || wmo_code == 227 || wmo_code == 230)
            return 5;  /* snow */
        return 3;  /* rain */
    }
    if (wmo_code == 143 || wmo_code == 248 || wmo_code == 260) return 6;
    return 2;  /* default: cloudy */
}

/* ── draw weather icon (programmatic) ────────────────────────────── */

static void draw_weather_icon(uint32_t *px, uint32_t w, uint32_t h,
    int cx, int cy, int sz, int code)
{
    uint32_t sun = lumo_app_argb(0xFF, 0xFF, 0xCC, 0x33);
    uint32_t cloud = lumo_app_argb(0xFF, 0xCC, 0xCC, 0xDD);
    uint32_t rain = lumo_app_argb(0xFF, 0x44, 0x88, 0xFF);
    uint32_t bolt = lumo_app_argb(0xFF, 0xFF, 0xEE, 0x44);
    uint32_t snow_c = lumo_app_argb(0xFF, 0xDD, 0xEE, 0xFF);
    uint32_t fog_c = lumo_app_argb(0x80, 0xAA, 0xAA, 0xBB);
    int r = sz / 2;

    switch (code) {
    case 0: /* sunny — filled circle */
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx*dx + dy*dy <= r*r) {
                    int px_ = cx + dx, py = cy + dy;
                    if (px_ >= 0 && px_ < (int)w && py >= 0 && py < (int)h)
                        px[py * (int)w + px_] = sun;
                }
        break;
    case 1: /* partly cloudy — small sun + cloud */
        for (int dy = -r/2; dy <= r/2; dy++)
            for (int dx = -r/2; dx <= r/2; dx++)
                if (dx*dx + dy*dy <= (r/2)*(r/2)) {
                    int px_ = cx - r/3 + dx, py = cy - r/3 + dy;
                    if (px_ >= 0 && px_ < (int)w && py >= 0 && py < (int)h)
                        px[py * (int)w + px_] = sun;
                }
        /* cloud overlay */
        for (int dy = -r/3; dy <= r/3; dy++)
            for (int dx = -r*2/3; dx <= r*2/3; dx++)
                if (dx*dx*9 + dy*dy*36 <= r*r*4) {
                    int px_ = cx + r/4 + dx, py = cy + r/4 + dy;
                    if (px_ >= 0 && px_ < (int)w && py >= 0 && py < (int)h)
                        px[py * (int)w + px_] = cloud;
                }
        break;
    case 2: /* cloudy — big cloud */
        for (int dy = -r/2; dy <= r/2; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx*dx*4 + dy*dy*16 <= r*r*4) {
                    int px_ = cx + dx, py = cy + dy;
                    if (px_ >= 0 && px_ < (int)w && py >= 0 && py < (int)h)
                        px[py * (int)w + px_] = cloud;
                }
        break;
    case 3: /* rain — cloud + drops */
        for (int dy = -r/3; dy <= r/3; dy++)
            for (int dx = -r*2/3; dx <= r*2/3; dx++)
                if (dx*dx*9 + dy*dy*36 <= r*r*4) {
                    int px_ = cx + dx, py = cy - r/4 + dy;
                    if (px_ >= 0 && px_ < (int)w && py >= 0 && py < (int)h)
                        px[py * (int)w + px_] = cloud;
                }
        /* rain drops */
        for (int i = 0; i < 3; i++) {
            int dx = cx - r/3 + i * r/3;
            for (int dy = 0; dy < r/2; dy++) {
                int py = cy + r/3 + dy;
                if (dx >= 0 && dx < (int)w && py >= 0 && py < (int)h)
                    px[py * (int)w + dx] = rain;
            }
        }
        break;
    case 4: /* thunder — cloud + bolt */
        for (int dy = -r/3; dy <= r/3; dy++)
            for (int dx = -r*2/3; dx <= r*2/3; dx++)
                if (dx*dx*9 + dy*dy*36 <= r*r*4) {
                    int px_ = cx + dx, py = cy - r/4 + dy;
                    if (px_ >= 0 && px_ < (int)w && py >= 0 && py < (int)h)
                        px[py * (int)w + px_] = cloud;
                }
        /* lightning bolt */
        lumo_app_fill_rect(px, w, h, cx - 2, cy + r/6, 4, r/3, bolt);
        lumo_app_fill_rect(px, w, h, cx - 4, cy + r/6 + r/3, 6, 2, bolt);
        lumo_app_fill_rect(px, w, h, cx, cy + r/6 + r/3, 4, r/4, bolt);
        break;
    case 5: /* snow — cloud + dots */
        for (int dy = -r/3; dy <= r/3; dy++)
            for (int dx = -r*2/3; dx <= r*2/3; dx++)
                if (dx*dx*9 + dy*dy*36 <= r*r*4) {
                    int px_ = cx + dx, py = cy - r/4 + dy;
                    if (px_ >= 0 && px_ < (int)w && py >= 0 && py < (int)h)
                        px[py * (int)w + px_] = cloud;
                }
        /* snowflakes */
        for (int i = 0; i < 4; i++) {
            int sx = cx - r/2 + i * r/3;
            int sy = cy + r/3 + (i % 2) * r/4;
            lumo_app_fill_rect(px, w, h, sx, sy, 3, 3, snow_c);
        }
        break;
    case 6: /* fog — horizontal bars */
        for (int i = 0; i < 4; i++) {
            int fy = cy - r/2 + i * r/3;
            int fw = r * 2 - i * r/4;
            lumo_app_fill_rect(px, w, h, cx - fw/2, fy, fw, 3, fog_c);
        }
        break;
    }
}

/* ── curl fetch ──────────────────────────────────────────────────── */

static size_t wx_write(void *p, size_t s, size_t n, void *ud) {
    char *buf = ud;
    size_t total = s * n;
    size_t cur = strlen(buf);
    if (cur + total >= 16384) total = 16383 - cur;
    memcpy(buf + cur, p, total);
    buf[cur + total] = '\0';
    return s * n;
}

static void wx_parse(const char *json) {
    /* find current_condition */
    const char *cc = strstr(json, "\"current_condition\"");
    if (!cc) return;

    wx.temp_f = json_int(cc, "temp_F");
    wx.feels_f = json_int(cc, "FeelsLikeF");
    wx.humidity = json_int(cc, "humidity");
    wx.wind_mph = json_int(cc, "windspeedMiles");
    json_str(cc, "winddir16Point", wx.wind_dir, sizeof(wx.wind_dir));
    wx.uv_index = json_int(cc, "uvIndex");
    wx.visibility = json_int(cc, "visibility");
    wx.pressure = json_int(cc, "pressure");

    /* weatherDesc is nested: [{"value":"..."}] */
    const char *wd = strstr(cc, "\"weatherDesc\"");
    if (wd) {
        const char *val = strstr(wd, "\"value\"");
        if (val) json_str(val, "value", wx.condition, sizeof(wx.condition));
    }
    int wcode = json_int(cc, "weatherCode");
    wx.code = weather_icon_code(wcode);

    /* forecast — find "weather" array */
    const char *weather = strstr(json, "\"weather\"");
    if (!weather) { wx.loaded = true; return; }

    wx.forecast_count = 0;
    const char *p = strchr(weather, '[');
    if (!p) { wx.loaded = true; return; }

    /* parse up to 3 day objects */
    for (int i = 0; i < 3 && wx.forecast_count < 3; i++) {
        const char *obj = strchr(p, '{');
        if (!obj) break;

        struct weather_day *day = &wx.forecast[wx.forecast_count];
        json_str(obj, "date", day->date, sizeof(day->date));
        day->max_f = json_int(obj, "maxtempF");
        day->min_f = json_int(obj, "mintempF");

        /* get condition from first hourly entry */
        const char *hourly = strstr(obj, "\"hourly\"");
        if (hourly) {
            const char *hobj = strchr(hourly, '{');
            if (hobj) {
                const char *desc = strstr(hobj, "\"weatherDesc\"");
                if (desc) {
                    const char *val = strstr(desc, "\"value\"");
                    if (val) json_str(val, "value", day->condition,
                        sizeof(day->condition));
                }
                int hcode = json_int(hobj, "weatherCode");
                day->code = weather_icon_code(hcode);
            }
        }

        /* derive day name from date */
        {
            struct tm tm = {0};
            int yr, mo, dy;
            if (sscanf(day->date, "%d-%d-%d", &yr, &mo, &dy) == 3) {
                tm.tm_year = yr - 1900;
                tm.tm_mon = mo - 1;
                tm.tm_mday = dy;
                mktime(&tm);
                static const char *days[] = {"SUN","MON","TUE","WED",
                    "THU","FRI","SAT"};
                snprintf(day->day_name, sizeof(day->day_name), "%s",
                    days[tm.tm_wday]);
            }
        }

        wx.forecast_count++;

        /* skip to next object */
        p = strchr(obj + 1, '}');
        if (p) p++;
    }

    wx.loaded = true;
    wx.last_fetch = time(NULL);
}

static void *wx_fetch_thread(void *arg) {
    (void)arg;
    wx.fetching = true;

    CURL *curl = curl_easy_init();
    if (!curl) { wx.fetching = false; return NULL; }

    char *buf = calloc(1, 16384);
    if (!buf) { curl_easy_cleanup(curl); wx.fetching = false; return NULL; }

    curl_easy_setopt(curl, CURLOPT_URL,
        "https://wttr.in/41101?format=j1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wx_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lumo-OS/0.0.82");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && buf[0])
        wx_parse(buf);

    free(buf);
    wx.fetching = false;
    return NULL;
}

static void wx_ensure_fetched(void) {
    if (wx.fetching) return;
    time_t now = time(NULL);
    if (!wx.loaded || now - wx.last_fetch > 300) {
        pthread_t tid;
        pthread_create(&tid, NULL, wx_fetch_thread, NULL);
        pthread_detach(tid);
    }
}

/* ── render ──────────────────────────────────────────────────────── */

void lumo_app_render_weather(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    int w = (int)width, h = (int)height;
    (void)ctx;

    wx_ensure_fetched();
    lumo_app_draw_background(pixels, width, height);

    /* header */
    lumo_app_fill_rect(pixels, width, height, 0, 0, w, 48, theme.header_bg);
    lumo_app_draw_text(pixels, width, height, 16, 14, 3, theme.accent,
        "WEATHER");
    lumo_app_draw_text(pixels, width, height, w - 80, 18, 2,
        theme.text_dim, "41101");
    lumo_app_fill_rect(pixels, width, height, 8, 48, w - 16, 1,
        theme.separator);

    if (!wx.loaded) {
        lumo_app_draw_text(pixels, width, height, 16, 80, 2,
            theme.text_dim,
            wx.fetching ? "FETCHING WEATHER..." : "NO DATA");
        return;
    }

    int y = 56;
    int pad = 12;

    /* ── current conditions card ─────────────────────────────── */
    {
        struct lumo_rect card = {pad, y, w - pad * 2, 140};
        lumo_app_fill_rounded_rect(pixels, width, height, &card, 14,
            theme.card_bg);

        /* weather icon (left side) */
        draw_weather_icon(pixels, width, height,
            pad + 50, y + 50, 60, wx.code);

        /* temperature (right of icon) */
        char temp[16];
        snprintf(temp, sizeof(temp), "%dF", wx.temp_f);
        lumo_app_draw_text(pixels, width, height,
            pad + 96, y + 16, 6, theme.text, temp);

        /* condition text */
        lumo_app_draw_text(pixels, width, height,
            pad + 96, y + 64, 2, theme.text_dim, wx.condition);

        /* feels like */
        snprintf(temp, sizeof(temp), "FEELS %dF", wx.feels_f);
        lumo_app_draw_text(pixels, width, height,
            pad + 96, y + 84, 2, theme.text_dim, temp);

        /* hi/lo from today's forecast */
        if (wx.forecast_count > 0) {
            snprintf(temp, sizeof(temp), "H:%dF  L:%dF",
                wx.forecast[0].max_f, wx.forecast[0].min_f);
            lumo_app_draw_text(pixels, width, height,
                pad + 96, y + 104, 2, theme.accent, temp);
        }
    }
    y += 148;

    /* ── detail grid (2x3 cards) ─────────────────────────────── */
    {
        int card_w = (w - pad * 3) / 2;
        int card_h = 56;
        struct {
            const char *label;
            char value[16];
        } stats[6];

        snprintf(stats[0].value, 16, "%d%%", wx.humidity);
        stats[0].label = "HUMIDITY";
        snprintf(stats[1].value, 16, "%d %s", wx.wind_mph, wx.wind_dir);
        stats[1].label = "WIND";
        snprintf(stats[2].value, 16, "%d", wx.uv_index);
        stats[2].label = "UV INDEX";
        snprintf(stats[3].value, 16, "%d mi", wx.visibility);
        stats[3].label = "VISIBILITY";
        snprintf(stats[4].value, 16, "%d mb", wx.pressure);
        stats[4].label = "PRESSURE";
        {
            struct tm *tm = localtime(&wx.last_fetch);
            strftime(stats[5].value, 16, "%H:%M", tm);
        }
        stats[5].label = "UPDATED";

        for (int i = 0; i < 6; i++) {
            int col = i % 2;
            int row = i / 2;
            int cx = pad + col * (card_w + pad);
            int cy = y + row * (card_h + 6);
            struct lumo_rect card = {cx, cy, card_w, card_h};
            lumo_app_fill_rounded_rect(pixels, width, height, &card, 10,
                theme.card_bg);
            lumo_app_draw_text(pixels, width, height,
                cx + 10, cy + 6, 1, theme.text_dim, stats[i].label);
            lumo_app_draw_text(pixels, width, height,
                cx + 10, cy + 22, 3, theme.text, stats[i].value);
        }
        y += 3 * (card_h + 6) + 4;
    }

    /* ── 3-day forecast ──────────────────────────────────────── */
    lumo_app_draw_text(pixels, width, height, pad + 4, y, 2,
        theme.accent, "3-DAY FORECAST");
    y += 22;
    lumo_app_fill_rect(pixels, width, height, pad, y, w - pad * 2, 1,
        theme.separator);
    y += 6;

    {
        int card_w = (w - pad * 2 - 12) / 3;
        int card_h = 90;

        for (int i = 0; i < wx.forecast_count && i < 3; i++) {
            struct weather_day *day = &wx.forecast[i];
            int cx = pad + i * (card_w + 6);
            struct lumo_rect card = {cx, y, card_w, card_h};
            lumo_app_fill_rounded_rect(pixels, width, height, &card, 10,
                theme.card_bg);

            /* day name */
            struct lumo_rect name_rect = {cx, y + 4, card_w, 16};
            lumo_app_draw_text_centered(pixels, width, height,
                &name_rect, 2,
                i == 0 ? theme.accent : theme.text, day->day_name);

            /* weather icon */
            draw_weather_icon(pixels, width, height,
                cx + card_w / 2, y + 38, 28, day->code);

            /* hi/lo */
            char hilo[24];
            snprintf(hilo, sizeof(hilo), "%dF/%dF",
                day->max_f, day->min_f);
            struct lumo_rect hilo_rect = {cx, y + 58, card_w, 14};
            lumo_app_draw_text_centered(pixels, width, height,
                &hilo_rect, 2, theme.text, hilo);

            /* condition (truncated) */
            char cond[16];
            snprintf(cond, sizeof(cond), "%.12s", day->condition);
            struct lumo_rect cond_rect = {cx, y + 74, card_w, 12};
            lumo_app_draw_text_centered(pixels, width, height,
                &cond_rect, 1, theme.text_dim, cond);
        }
    }

    (void)h;
}
