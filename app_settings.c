#include "lumo/app_render.h"
#include "lumo/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* ── layout constants ─────────────────────────────────────────────── */

static const int ROW_H      = 56;
static const int HEADER_Y   = 80;  /* main page content start */
static const int SUB_Y      = 92;  /* subpage content start (below header+sep) */
static const int SECTION_H  = 28;
static const int PAD        = 24;

/* ── theme helpers ────────────────────────────────────────────────── */

static struct lumo_app_theme th;
static bool th_loaded;

static void ensure_theme(void) {
    /* always refresh — theme changes with time of day */
    lumo_app_theme_get(&th);
    th_loaded = true;
}

/* ── drawing primitives ───────────────────────────────────────────── */

static void draw_section(
    uint32_t *px, uint32_t w, uint32_t h, int y, const char *label
) {
    ensure_theme();
    lumo_app_draw_text(px, w, h, PAD, y + 6, 2, th.accent, label);
    lumo_app_fill_rect(px, w, h, PAD, y + SECTION_H - 2,
        (int)w - PAD * 2, 1, th.separator);
}

static void draw_row(
    uint32_t *px, uint32_t w, uint32_t h,
    int y, const char *title, const char *subtitle
) {
    ensure_theme();
    struct lumo_rect r = { PAD, y, (int)w - PAD * 2, ROW_H - 4 };
    lumo_app_fill_rounded_rect(px, w, h, &r, 12, th.card_bg);
    lumo_app_draw_outline(px, w, h, &r, 1, th.card_stroke);
    lumo_app_draw_text(px, w, h, r.x + 16, r.y + 10, 2, th.text, title);
    if (subtitle != NULL) {
        lumo_app_draw_text(px, w, h, r.x + 16, r.y + 30, 2,
            th.text_dim, subtitle);
    }
    lumo_app_draw_text(px, w, h, r.x + r.width - 24, r.y + 16, 2,
        th.text_dim, ">");
}

static void draw_subheader(
    uint32_t *px, uint32_t w, uint32_t h, const char *title
) {
    ensure_theme();
    /* back button starts at y=38 to stay below the 32px top edge zone
     * so tapping it doesn't trigger the time/quick-settings panel */
    lumo_app_draw_text(px, w, h, PAD, 38, 2, th.accent, "< BACK");
    lumo_app_draw_text(px, w, h, PAD, 58, 3, th.text, title);
    lumo_app_fill_rect(px, w, h, PAD, 82, (int)w - PAD * 2, 1,
        th.separator);
}

static void draw_info(
    uint32_t *px, uint32_t w, uint32_t h,
    int y, const char *label, const char *value
) {
    ensure_theme();
    lumo_app_draw_text(px, w, h, PAD + 8, y, 2, th.text_dim, label);
    lumo_app_draw_text(px, w, h, (int)w / 3 + 20, y, 2, th.text, value);
}

static void draw_toggle(
    uint32_t *px, uint32_t w, uint32_t h,
    int y, const char *label, bool on
) {
    ensure_theme();
    lumo_app_draw_text(px, w, h, PAD + 8, y + 2, 2, th.text, label);

    int tx = (int)w - PAD - 56;
    struct lumo_rect track = { tx, y, 44, 22 };
    uint32_t track_c = on ? th.accent : th.card_stroke;
    lumo_app_fill_rounded_rect(px, w, h, &track, 11, track_c);
    struct lumo_rect knob = {
        on ? tx + 24 : tx + 2,
        y + 3, 16, 16
    };
    lumo_app_fill_rounded_rect(px, w, h, &knob, 8,
        lumo_app_argb(0xFF, 0xFF, 0xFF, 0xFF));
}

static void draw_bar(
    uint32_t *px, uint32_t w, uint32_t h,
    int y, unsigned long used, unsigned long total
) {
    ensure_theme();
    int bw = (int)w - PAD * 2 - 16;
    struct lumo_rect bg = { PAD + 8, y, bw, 14 };
    lumo_app_fill_rounded_rect(px, w, h, &bg, 7, th.card_stroke);
    if (total > 0) {
        struct lumo_rect fg = { PAD + 8, y,
            (int)((long)bw * (long)used / (long)total), 14 };
        if (fg.width > 0)
            lumo_app_fill_rounded_rect(px, w, h, &fg, 7, th.accent);
    }
}

/* ── data helpers ─────────────────────────────────────────────────── */

/* run a command and capture first line of output */
static void run_cmd(const char *cmd, char *out, size_t outsz) {
    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    if (fgets(out, (int)outsz, fp)) {
        char *nl = strchr(out, '\n'); if (nl) *nl = '\0';
    }
    pclose(fp);
}

static void get_wifi_info(char *iface, size_t isz,
                          char *ssid, size_t sidsz,
                          char *status, size_t ssz,
                          char *signal_out, size_t sgsz,
                          char *ip, size_t ipsz)
{
    snprintf(iface, isz, "NONE");
    snprintf(ssid, sidsz, "--");
    snprintf(status, ssz, "DISCONNECTED");
    snprintf(signal_out, sgsz, "--");
    snprintf(ip, ipsz, "--");

    /* find wireless interface via sysfs */
    char ifn[32] = {0};
    DIR *netdir = opendir("/sys/class/net");
    if (netdir) {
        struct dirent *ent;
        while ((ent = readdir(netdir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char wpath[128];
            struct stat st;
            snprintf(wpath, sizeof(wpath), "/sys/class/net/%s/wireless",
                ent->d_name);
            if (stat(wpath, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(ifn, sizeof(ifn), "%s", ent->d_name);
                break;
            }
        }
        closedir(netdir);
    }
    if (!ifn[0]) return;
    snprintf(iface, isz, "%s", ifn);

    /* operstate */
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifn);
    FILE *fp = fopen(path, "r");
    if (fp) {
        char st[32] = {0};
        if (fgets(st, sizeof(st), fp)) {
            char *nl = strchr(st, '\n'); if (nl) *nl = '\0';
            if (strcmp(st, "up") == 0 || strcmp(st, "unknown") == 0)
                snprintf(status, ssz, "CONNECTED");
            else
                snprintf(status, ssz, "%s", st);
        }
        fclose(fp);
    }

    /* SSID via nmcli */
    char cmd_out[128];
    run_cmd("nmcli -t -f active,ssid dev wifi 2>/dev/null | grep '^yes' | cut -d: -f2",
        cmd_out, sizeof(cmd_out));
    if (cmd_out[0])
        snprintf(ssid, sidsz, "%s", cmd_out);

    /* IP address via hostname -I (first address) */
    run_cmd("hostname -I 2>/dev/null | awk '{print $1}'",
        cmd_out, sizeof(cmd_out));
    if (cmd_out[0])
        snprintf(ip, ipsz, "%s", cmd_out);

    /* signal from /proc/net/wireless if present */
    fp = fopen("/proc/net/wireless", "r");
    if (fp) {
        char line[256]; char pifn[32] = {0}; float sig = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, " %31[^:]: %*d %*f %f", pifn, &sig) >= 1 &&
                    pifn[0] && pifn[0] != '|') {
                snprintf(signal_out, sgsz, "%.0f DBM", sig);
                break;
            }
        }
        fclose(fp);
    }
}

static int get_volume_pct(void) {
    char out[128];
    run_cmd("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | grep -oP '\\d+%' | head -1",
        out, sizeof(out));
    int v = 0;
    sscanf(out, "%d", &v);
    return v > 100 ? 100 : v;
}

static int get_brightness_pct(void) {
    /* try sysfs backlight */
    char cur_s[32] = {0}, max_s[32] = {0};
    FILE *fp = fopen("/sys/class/backlight/backlight/brightness", "r");
    if (!fp) fp = fopen("/sys/class/backlight/0/brightness", "r");
    if (fp) { if (fgets(cur_s, sizeof(cur_s), fp)) {} fclose(fp); }
    fp = fopen("/sys/class/backlight/backlight/max_brightness", "r");
    if (!fp) fp = fopen("/sys/class/backlight/0/max_brightness", "r");
    if (fp) { if (fgets(max_s, sizeof(max_s), fp)) {} fclose(fp); }
    int cur = atoi(cur_s), mx = atoi(max_s);
    if (mx > 0) return (cur * 100) / mx;
    return 50; /* default if no backlight sysfs */
}

static void get_mem_info(unsigned long *total, unsigned long *avail,
                         unsigned long *buffers, unsigned long *cached)
{
    *total = *avail = *buffers = *cached = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        sscanf(line, "MemTotal: %lu", total);
        sscanf(line, "MemAvailable: %lu", avail);
        sscanf(line, "Buffers: %lu", buffers);
        sscanf(line, "Cached: %lu", cached);
    }
    fclose(fp);
}

/* ── row hit test ─────────────────────────────────────────────────── */

int lumo_app_settings_row_at(
    uint32_t width, uint32_t height, double x, double y
) {
    (void)height;
    if (x < PAD || x > (double)width - PAD) return -1;
    if (y < HEADER_Y) return -1;

    int cy = HEADER_Y;

    /* CONNECTIVITY section header */
    cy += SECTION_H;
    /* row 0: Wi-Fi */
    if (y >= cy && y < cy + ROW_H) return 0;
    cy += ROW_H;

    /* DEVICE section header */
    cy += SECTION_H;
    /* rows 1-4: Display, Sound, Storage, Memory */
    for (int i = 1; i <= 4; i++) {
        if (y >= cy && y < cy + ROW_H) return i;
        cy += ROW_H;
    }

    /* SYSTEM section header */
    cy += SECTION_H;
    /* rows 5-8: General, About, Lumo, Processor */
    for (int i = 5; i <= 8; i++) {
        if (y >= cy && y < cy + ROW_H) return i;
        cy += ROW_H;
    }

    return -1;
}

/* toggle hit test for subpages */
int lumo_app_settings_toggle_at(
    uint32_t width, uint32_t height,
    double x, double y, int subpage
) {
    (void)height;
    /* toggles occupy the right portion of the row */
    if (x < (double)width / 2) return -1;

    int base_y = SUB_Y;
    int info_h = 28;
    int toggle_h = 34;

    switch (subpage) {
    case 0: /* Network: wifi_enabled after 3 info rows */
        if (y >= base_y + info_h * 3 && y < base_y + info_h * 3 + toggle_h)
            return 0;
        break;
    case 1: /* Display: auto_rotate after 3 info rows */
        if (y >= base_y + info_h * 3 && y < base_y + info_h * 3 + toggle_h)
            return 1;
        break;
    case 5: /* General: auto_updates after 3 info rows, persist_logs after */
        if (y >= base_y + info_h * 3 && y < base_y + info_h * 3 + toggle_h)
            return 2;
        if (y >= base_y + info_h * 3 + toggle_h &&
                y < base_y + info_h * 3 + toggle_h * 2)
            return 4;
        break;
    case 7: /* Lumo: debug_mode after 5 info rows */
        if (y >= base_y + info_h * 5 && y < base_y + info_h * 5 + toggle_h)
            return 3;
        break;
    }
    return -1;
}

/* action hit test: buttons and slider drags */
int lumo_app_settings_action_at(
    uint32_t width, uint32_t height,
    double x, double y, int subpage
) {
    (void)height;
    switch (subpage) {
    case 1: /* Display */
        /* rotate button: y = SUB_Y + 28*4 + 34*1 + 34 + 28 = roughly SUB_Y+186 */
        if (y >= SUB_Y + 28 * 3 + 34 + 34 + 28 &&
                y < SUB_Y + 28 * 3 + 34 + 34 + 28 + 32 &&
                x >= PAD + 8 && x < PAD + 128)
            return 100; /* rotate */
        /* brightness slider: y around SUB_Y + 28*3 + 34 */
        if (y >= SUB_Y + 28 * 3 + 34 && y < SUB_Y + 28 * 3 + 34 + 30 &&
                x >= PAD && x < (double)width - PAD)
            return 102; /* brightness slider */
        break;
    case 2: /* Sound */
        /* volume slider: y around SUB_Y + 28*2 */
        if (y >= SUB_Y + 28 * 2 && y < SUB_Y + 28 * 2 + 30 &&
                x >= PAD && x < (double)width - PAD)
            return 101; /* volume slider */
        break;
    }
    return -1;
}

/* ── subpage renderers ────────────────────────────────────────────── */

/* ── WiFi scan cache ──────────────────────────────────────────────── */

#define WIFI_MAX_NETWORKS 16

struct wifi_network {
    char ssid[64];
    char signal[8];
    char security[16];
    bool connected;
};

static struct wifi_network wifi_networks[WIFI_MAX_NETWORKS];
static int wifi_network_count = -1;  /* -1 = not scanned */
static time_t wifi_last_scan;

static void wifi_scan(void) {
    wifi_network_count = 0;
    /* nmcli -t -f ACTIVE,SSID,SIGNAL,SECURITY dev wifi list */
    FILE *fp = popen(
        "nmcli -t -f ACTIVE,SSID,SIGNAL,SECURITY dev wifi list 2>/dev/null"
        " | head -16", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp) &&
            wifi_network_count < WIFI_MAX_NETWORKS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        /* format: yes:MySSID:85:WPA2 or no:OtherNet:42:WPA1 */
        struct wifi_network *n = &wifi_networks[wifi_network_count];
        char active[8] = "";
        char *p = line;
        /* parse ACTIVE */
        char *sep = strchr(p, ':');
        if (!sep) continue;
        *sep = '\0';
        snprintf(active, sizeof(active), "%s", p);
        p = sep + 1;
        /* parse SSID */
        sep = strchr(p, ':');
        if (!sep) continue;
        *sep = '\0';
        if (p[0] == '\0') { p = sep + 1; continue; } /* skip empty SSID */
        snprintf(n->ssid, sizeof(n->ssid), "%s", p);
        p = sep + 1;
        /* parse SIGNAL */
        sep = strchr(p, ':');
        if (!sep) continue;
        *sep = '\0';
        snprintf(n->signal, sizeof(n->signal), "%s", p);
        p = sep + 1;
        /* parse SECURITY */
        snprintf(n->security, sizeof(n->security), "%s", p);
        n->connected = (strcmp(active, "yes") == 0);
        wifi_network_count++;
    }
    pclose(fp);
    wifi_last_scan = time(NULL);
}

/* Connect to a WiFi network (called from touch handler) */
void lumo_settings_wifi_connect(int network_index, const char *password) {
    if (network_index < 0 || network_index >= wifi_network_count) return;
    char cmd[512];
    if (password && password[0]) {
        snprintf(cmd, sizeof(cmd),
            "nmcli dev wifi connect '%s' password '%s' 2>&1",
            wifi_networks[network_index].ssid, password);
    } else {
        snprintf(cmd, sizeof(cmd),
            "nmcli dev wifi connect '%s' 2>&1",
            wifi_networks[network_index].ssid);
    }
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char out[256];
        if (fgets(out, sizeof(out), fp)) {
            (void)out; /* could show result as toast */
        }
        pclose(fp);
    }
    /* rescan after connect attempt */
    wifi_network_count = -1;
}

static void render_network(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    draw_subheader(px, w, h, "Wi-Fi & Network");
    int y = SUB_Y;
    char iface[64], ssid[64], status[64], sig[32], ip[32];
    get_wifi_info(iface, sizeof(iface), ssid, sizeof(ssid),
                  status, sizeof(status), sig, sizeof(sig),
                  ip, sizeof(ip));

    draw_info(px, w, h, y, "STATUS", status); y += 28;
    draw_info(px, w, h, y, "SSID", ssid); y += 28;
    draw_info(px, w, h, y, "IP ADDRESS", ip); y += 28;
    draw_toggle(px, w, h, y, "WI-FI ENABLED",
        ctx->settings.wifi_enabled); y += 38;

    /* scan for available networks (every 30s) */
    time_t now = time(NULL);
    if (wifi_network_count < 0 || now - wifi_last_scan > 30)
        wifi_scan();

    lumo_app_draw_text(px, w, h, PAD + 8, y, 2, th.accent,
        "AVAILABLE NETWORKS");
    y += 22;
    lumo_app_fill_rect(px, w, h, PAD + 8, y, (int)w - PAD * 2 - 16, 1,
        th.separator);
    y += 6;

    if (wifi_network_count == 0) {
        lumo_app_draw_text(px, w, h, PAD + 16, y, 2,
            th.text_dim, "NO NETWORKS FOUND");
        return;
    }

    int scroll = ctx != NULL ? ctx->scroll_offset : 0;
    for (int i = scroll; i < wifi_network_count && y + 44 < (int)h; i++) {
        struct wifi_network *n = &wifi_networks[i];
        struct lumo_rect row = {PAD + 4, y, (int)w - PAD * 2 - 8, 38};
        lumo_app_fill_rounded_rect(px, w, h, &row, 8,
            n->connected ? th.accent : th.card_bg);
        lumo_app_draw_outline(px, w, h, &row, 1, th.card_stroke);

        /* SSID */
        lumo_app_draw_text(px, w, h, PAD + 16, y + 4, 2,
            n->connected ? th.bg : th.text, n->ssid);

        /* signal + security */
        char detail[48];
        snprintf(detail, sizeof(detail), "%s%%  %s",
            n->signal, n->security);
        lumo_app_draw_text(px, w, h, PAD + 16, y + 22, 2,
            n->connected ? th.bg : th.text_dim, detail);

        /* connected badge */
        if (n->connected) {
            int bw = 12 * 9; /* "CONNECTED" width */
            lumo_app_draw_text(px, w, h,
                (int)w - PAD - bw - 8, y + 12, 2,
                th.bg, "CONNECTED");
        }

        y += 44;
    }
}

static void draw_slider(
    uint32_t *px, uint32_t w, uint32_t h,
    int y, const char *label, int pct
) {
    ensure_theme();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lumo_app_draw_text(px, w, h, PAD + 8, y, 2, th.text, label);
    int tx = PAD + 8;
    int tw = (int)w - PAD * 2 - 16;
    struct lumo_rect track = { tx, y + 20, tw, 10 };
    lumo_app_fill_rounded_rect(px, w, h, &track, 5, th.card_stroke);
    int fw = tw * pct / 100;
    if (fw > 0) {
        struct lumo_rect fill = { tx, y + 20, fw, 10 };
        lumo_app_fill_rounded_rect(px, w, h, &fill, 5, th.accent);
    }
    /* knob at current position */
    int kx = tx + fw - 7;
    if (kx < tx) kx = tx;
    struct lumo_rect knob = { kx, y + 16, 14, 18 };
    lumo_app_fill_rounded_rect(px, w, h, &knob, 7,
        lumo_app_argb(0xFF, 0xFF, 0xFF, 0xFF));
}

static const char *get_current_rotation(void) {
    static char rot[16] = "0";
    const char *home = getenv("HOME");
    if (!home) return rot;
    char path[256];
    snprintf(path, sizeof(path), "%s/.lumo-rotation", home);
    FILE *fp = fopen(path, "r");
    if (fp) {
        if (fgets(rot, sizeof(rot), fp)) {
            char *nl = strchr(rot, '\n'); if (nl) *nl = '\0';
        }
        fclose(fp);
    }
    if (strcmp(rot, "normal") == 0) snprintf(rot, sizeof(rot), "0");
    return rot;
}

static void draw_button(
    uint32_t *px, uint32_t w, uint32_t h,
    int x, int y, int bw, int bh, const char *label, uint32_t color
) {
    struct lumo_rect r = { x, y, bw, bh };
    lumo_app_fill_rounded_rect(px, w, h, &r, 10, color);
    lumo_app_draw_text_centered(px, w, h, &r, 2,
        lumo_app_argb(0xFF, 0xFF, 0xFF, 0xFF), label);
}

/* detect compositor renderer + GPU info */
static const char *get_renderer_info(void) {
    static char info[128];
    FILE *fp;
    /* check if WLR_RENDER_DRM_DEVICE is set (GPU mode) */
    const char *rdev = getenv("WLR_RENDER_DRM_DEVICE");
    if (rdev != NULL && rdev[0] != '\0') {
        /* GPU rendering — read GPU name from DRM */
        fp = fopen("/sys/class/drm/card0/device/driver/module/drivers/"
            "platform:pvrsrvkm/cac00000.imggpu/uevent", "r");
        if (fp) { fclose(fp); return "GLES 3.2 PVR BXE-2-32"; }
        return "GLES GPU";
    }
    /* check dmesg for renderer type */
    fp = popen("journalctl -b 2>/dev/null | grep 'Creating.*renderer' | "
        "tail -1 | sed 's/.*Creating //' | sed 's/ renderer//' 2>/dev/null",
        "r");
    if (fp) {
        info[0] = '\0';
        if (fgets(info, sizeof(info), fp)) {
            char *nl = strchr(info, '\n');
            if (nl) *nl = '\0';
        }
        pclose(fp);
        if (info[0] != '\0') {
            /* capitalize */
            for (int i = 0; info[i]; i++)
                if (info[i] >= 'a' && info[i] <= 'z')
                    info[i] -= 32;
            return info;
        }
    }
    return "PIXMAN SOFTWARE";
}

static const char *get_gpu_info(void) {
    /* check if PVR GPU is present */
    FILE *fp = fopen("/sys/class/drm/renderD128/device/uevent", "r");
    if (fp) {
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "OF_NAME=", 8) == 0) {
                fclose(fp);
                return "IMG BXE-2-32 (VULKAN 1.3)";
            }
        }
        fclose(fp);
        return "GPU DETECTED";
    }
    return "NONE";
}

static void render_display(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    draw_subheader(px, w, h, "Display");
    int y = SUB_Y;
    char buf[64];
    snprintf(buf, sizeof(buf), "%ux%u", w, h);
    draw_info(px, w, h, y, "RESOLUTION", buf); y += 28;
    draw_info(px, w, h, y, "REFRESH", "60 HZ"); y += 28;
    draw_info(px, w, h, y, "RENDERER", get_renderer_info()); y += 28;
    draw_info(px, w, h, y, "GPU", get_gpu_info()); y += 28;
    draw_toggle(px, w, h, y, "AUTO ROTATE",
        ctx->settings.auto_rotate); y += 34;
    {
        int bpct = get_brightness_pct();
        snprintf(buf, sizeof(buf), "BRIGHTNESS  %d%%", bpct);
        draw_slider(px, w, h, y, buf, bpct);
    }
    y += 34;
    {
        const char *rot = get_current_rotation();
        snprintf(buf, sizeof(buf), "%s DEG", rot);
        draw_info(px, w, h, y, "ROTATION", buf);
    }
    y += 28;
    ensure_theme();
    draw_button(px, w, h, PAD + 8, y, 120, 32, "ROTATE", th.accent);
}

static void render_sound(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    (void)ctx;
    draw_subheader(px, w, h, "Sound");
    int y = SUB_Y;
    draw_info(px, w, h, y, "OUTPUT", "PIPEWIRE"); y += 28;
    draw_info(px, w, h, y, "DEVICE", "DEFAULT SINK"); y += 28;
    {
        int vpct = get_volume_pct();
        char buf[64];
        snprintf(buf, sizeof(buf), "VOLUME  %d%%", vpct);
        draw_slider(px, w, h, y, buf, vpct);
    }
    y += 34;
    draw_info(px, w, h, y, "CONTROL", "PACTL / PIPEWIRE");
}

static void render_storage(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    (void)ctx;
    draw_subheader(px, w, h, "Storage");
    int y = SUB_Y;
    char buf[64];
    struct statvfs st;
    if (statvfs("/", &st) == 0) {
        unsigned long total_m = (unsigned long)(st.f_blocks *
            (st.f_frsize / 1024)) / 1024;
        unsigned long free_m = (unsigned long)(st.f_bavail *
            (st.f_frsize / 1024)) / 1024;
        unsigned long used_m = total_m - free_m;

        snprintf(buf, sizeof(buf), "%lu MB", total_m);
        draw_info(px, w, h, y, "TOTAL", buf); y += 28;
        snprintf(buf, sizeof(buf), "%lu MB", used_m);
        draw_info(px, w, h, y, "USED", buf); y += 28;
        snprintf(buf, sizeof(buf), "%lu MB", free_m);
        draw_info(px, w, h, y, "FREE", buf); y += 28;
        snprintf(buf, sizeof(buf), "%lu%%",
            total_m > 0 ? (used_m * 100) / total_m : 0);
        draw_info(px, w, h, y, "USAGE", buf); y += 34;
        draw_bar(px, w, h, y, used_m, total_m);
    }
}

/* read VmRSS (resident memory) from /proc/<pid>/status in kB */
static unsigned long get_pid_rss(pid_t pid) {
    char path[64], line[256];
    FILE *fp;
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    fp = fopen(path, "r");
    if (!fp) return 0;
    unsigned long rss = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "VmRSS: %lu kB", &rss) == 1) break;
    }
    fclose(fp);
    return rss;
}

/* find PID by command name via /proc scan */
static pid_t find_pid(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        int p = atoi(ent->d_name);
        if (p <= 0) continue;
        char path[64], cmd[256];
        snprintf(path, sizeof(path), "/proc/%d/comm", p);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        cmd[0] = '\0';
        if (fgets(cmd, sizeof(cmd), fp)) {
            char *nl = strchr(cmd, '\n');
            if (nl) *nl = '\0';
        }
        fclose(fp);
        if (strcmp(cmd, name) == 0) { closedir(d); return (pid_t)p; }
    }
    closedir(d);
    return 0;
}

static void render_memory(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    (void)ctx;
    draw_subheader(px, w, h, "Memory");
    int y = SUB_Y;
    char buf[64];
    unsigned long total, avail, buffers, cached;
    get_mem_info(&total, &avail, &buffers, &cached);
    unsigned long used = total > avail ? total - avail : 0;

    snprintf(buf, sizeof(buf), "%lu MB", total / 1024);
    draw_info(px, w, h, y, "TOTAL", buf); y += 28;
    snprintf(buf, sizeof(buf), "%lu MB", used / 1024);
    draw_info(px, w, h, y, "USED", buf); y += 28;
    snprintf(buf, sizeof(buf), "%lu MB", avail / 1024);
    draw_info(px, w, h, y, "AVAILABLE", buf); y += 28;
    snprintf(buf, sizeof(buf), "%lu MB", (buffers + cached) / 1024);
    draw_info(px, w, h, y, "CACHE", buf); y += 34;
    if (total > 0)
        draw_bar(px, w, h, y, used, total);
    y += 24;

    /* Lumo process memory breakdown */
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);
    lumo_app_fill_rect(px, w, h, 12, y, (int)w - 24, 1, theme.separator);
    y += 8;
    lumo_app_draw_text(px, w, h, 16, y, 2, theme.accent, "LUMO PROCESSES");
    y += 22;

    static const struct { const char *comm; const char *label; } procs[] = {
        {"lumo-composit", "COMPOSITOR"},
        {"lumo-shell",    "SHELL (x5)"},
        {"lumo-app",      "NATIVE APP"},
        {"lumo-webview",  "WEB VIEW"},
        {"lumo-browser",  "BROWSER GTK"},
        {"WebKitWebProce", "WEBKIT PROC"},
    };
    unsigned long lumo_total = 0;
    for (int i = 0; i < (int)(sizeof(procs) / sizeof(procs[0])) &&
            y + 22 < (int)h - 10; i++) {
        pid_t pid = find_pid(procs[i].comm);
        if (pid > 0) {
            unsigned long rss = get_pid_rss(pid);
            lumo_total += rss;
            if (rss >= 1024) {
                snprintf(buf, sizeof(buf), "%lu.%lu MB",
                    rss / 1024, (rss % 1024) * 10 / 1024);
            } else {
                snprintf(buf, sizeof(buf), "%lu KB", rss);
            }
            draw_info(px, w, h, y, procs[i].label, buf);
        } else {
            draw_info(px, w, h, y, procs[i].label, "--");
        }
        y += 22;
    }
    y += 6;
    if (lumo_total > 0) {
        snprintf(buf, sizeof(buf), "%lu.%lu MB",
            lumo_total / 1024, (lumo_total % 1024) * 10 / 1024);
        draw_info(px, w, h, y, "LUMO TOTAL", buf);
    }
    y += 28;

    /* LumoCache stats */
    lumo_app_fill_rect(px, w, h, 12, y, (int)w - 24, 1, theme.separator);
    y += 8;
    lumo_app_draw_text(px, w, h, 16, y, 2, theme.accent, "LUMO CACHE");
    y += 22;
    {
        struct stat st;
        if (stat("/data/lumo-cache", &st) == 0) {
            /* count cache entries */
            int entries = 0;
            unsigned long total_kb = 0;
            const char *dirs[] = {"warm", "surfaces", "webkit", "fonts", "state"};
            for (int d = 0; d < 5 && y + 22 < (int)h; d++) {
                char cpath[256];
                snprintf(cpath, sizeof(cpath), "/data/lumo-cache/%s", dirs[d]);
                DIR *cd = opendir(cpath);
                int cnt = 0;
                unsigned long dir_kb = 0;
                if (cd) {
                    struct dirent *ent;
                    while ((ent = readdir(cd)) != NULL) {
                        if (ent->d_name[0] == '.') continue;
                        char fp[512];
                        struct stat fs;
                        snprintf(fp, sizeof(fp), "%s/%s", cpath, ent->d_name);
                        if (stat(fp, &fs) == 0) {
                            cnt++;
                            dir_kb += (unsigned long)fs.st_size / 1024;
                        }
                    }
                    closedir(cd);
                }
                entries += cnt;
                total_kb += dir_kb;
                if (cnt > 0) {
                    snprintf(buf, sizeof(buf), "%d FILES (%lu KB)", cnt, dir_kb);
                } else {
                    snprintf(buf, sizeof(buf), "EMPTY");
                }
                char label[16];
                for (int i = 0; dirs[d][i] && i < 15; i++)
                    label[i] = (dirs[d][i] >= 'a' && dirs[d][i] <= 'z')
                        ? dirs[d][i] - 32 : dirs[d][i];
                label[strlen(dirs[d])] = '\0';
                draw_info(px, w, h, y, label, buf);
                y += 22;
            }
            snprintf(buf, sizeof(buf), "%d FILES / %lu KB", entries, total_kb);
            draw_info(px, w, h, y, "CACHE TOTAL", buf);
        } else {
            draw_info(px, w, h, y, "STATUS", "NOT INITIALIZED");
        }
    }
}

static void render_general(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    draw_subheader(px, w, h, "General");
    int y = SUB_Y;
    char buf[64];

    FILE *fp = fopen("/proc/uptime", "r");
    if (fp) {
        double up = 0;
        if (fscanf(fp, "%lf", &up) == 1) {
            int d = (int)(up / 86400);
            int hr = (int)((up - d * 86400) / 3600);
            int mn = (int)((up - d * 86400 - hr * 3600) / 60);
            snprintf(buf, sizeof(buf), "%dD %dH %dM", d, hr, mn);
        }
        fclose(fp);
    } else {
        snprintf(buf, sizeof(buf), "UNKNOWN");
    }
    draw_info(px, w, h, y, "UPTIME", buf); y += 28;

    fp = fopen("/proc/loadavg", "r");
    if (fp) {
        char load[64] = "?";
        if (fgets(load, sizeof(load), fp)) {
            char *sp = strchr(load, ' ');
            if (sp) { sp = strchr(sp + 1, ' ');
                if (sp) { sp = strchr(sp + 1, ' ');
                    if (sp) *sp = '\0'; } }
        }
        fclose(fp);
        draw_info(px, w, h, y, "LOAD AVG", load);
    }
    y += 28;

    draw_info(px, w, h, y, "CPU", "4 CORES RISCV64"); y += 28;

    draw_toggle(px, w, h, y, "AUTO UPDATES",
        ctx->settings.auto_updates); y += 34;
    draw_toggle(px, w, h, y, "PERSIST LOGS",
        ctx->settings.persist_logs);
}

static void render_about(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    (void)ctx;
    draw_subheader(px, w, h, "About Device");
    int y = SUB_Y;
    char hostname[64] = "UNKNOWN";
    gethostname(hostname, sizeof(hostname) - 1);

    char kernel[128] = "UNKNOWN";
    FILE *fp = fopen("/proc/version", "r");
    if (fp) {
        if (fgets(kernel, sizeof(kernel), fp)) {
            char *s = strchr(kernel, ' ');
            if (s) s = strchr(s + 1, ' ');
            if (s) { char *e = strchr(s + 1, ' '); if (e) *e = '\0'; }
            if (s && s[0]) memmove(kernel, s + 1, strlen(s + 1) + 1);
        }
        fclose(fp);
    }

    draw_info(px, w, h, y, "DEVICE", "ORANGEPI RV2"); y += 28;
    draw_info(px, w, h, y, "HOSTNAME", hostname); y += 28;
    draw_info(px, w, h, y, "KERNEL", kernel); y += 28;

    /* read arch from uname */
    {
        FILE *uf = fopen("/proc/sys/kernel/osrelease", "r");
        (void)uf; /* arch is always riscv64 on this device */
    }
    draw_info(px, w, h, y, "ARCH", "RISCV64"); y += 28;

    /* read OS name from os-release */
    {
        static char os_name[64] = "LUMO OS";
        static bool os_read = false;
        if (!os_read) {
            os_read = true;
            FILE *of = fopen("/etc/os-release", "r");
            if (of) {
                char line[128];
                while (fgets(line, sizeof(line), of)) {
                    if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                        char *start = strchr(line, '"');
                        if (start) {
                            start++;
                            char *end = strchr(start, '"');
                            if (end) *end = '\0';
                            snprintf(os_name, sizeof(os_name), "%s", start);
                        }
                        break;
                    }
                }
                fclose(of);
            }
        }
        draw_info(px, w, h, y, "OS", os_name);
    }
}

static void render_lumo(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    draw_subheader(px, w, h, "Lumo");
    int y = SUB_Y;
    draw_info(px, w, h, y, "VERSION", LUMO_VERSION_STRING); y += 28;
    draw_info(px, w, h, y, "BUILD", "MESON + NINJA"); y += 28;
    draw_info(px, w, h, y, "RENDERER", get_renderer_info()); y += 28;
    draw_info(px, w, h, y, "GPU", get_gpu_info()); y += 28;
    draw_info(px, w, h, y, "SHELL", "UNIFIED LAYER-SHELL"); y += 28;
    draw_info(px, w, h, y, "APPS", "14 NATIVE WAYLAND"); y += 28;
    draw_toggle(px, w, h, y, "DEBUG MODE",
        ctx->settings.debug_mode);
}

static void render_processor(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    (void)ctx;
    draw_subheader(px, w, h, "Processor");
    int y = SUB_Y;
    char cpu[64] = "RISCV64";
    int cores = 0;
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "processor")) cores++;
            char *val;
            if ((val = strstr(line, "isa")) != NULL) {
                val = strchr(val, ':');
                if (val) { val += 2;
                    char *nl = strchr(val, '\n'); if (nl) *nl = '\0';
                    snprintf(cpu, sizeof(cpu), "%s", val); }
            }
        }
        fclose(fp);
    }
    char buf[32];
    draw_info(px, w, h, y, "ISA", cpu); y += 28;
    snprintf(buf, sizeof(buf), "%d", cores);
    draw_info(px, w, h, y, "CORES", buf); y += 28;
    draw_info(px, w, h, y, "GOVERNOR", "PERFORMANCE"); y += 28;
    draw_info(px, w, h, y, "SOC", "SPACEMIT K1");
}

/* ── main renderer ────────────────────────────────────────────────── */

void lumo_app_render_settings(
    const struct lumo_app_render_context *ctx,
    uint32_t *px, uint32_t w, uint32_t h
) {
    int selected = ctx != NULL ? ctx->selected_row : -1;
    th_loaded = false;

    lumo_app_draw_background(px, w, h);
    ensure_theme();

    /* ── subpage ── */
    if (selected >= 0 && selected <= 8) {
        switch (selected) {
        case 0: render_network(ctx, px, w, h); break;
        case 1: render_display(ctx, px, w, h); break;
        case 2: render_sound(ctx, px, w, h); break;
        case 3: render_storage(ctx, px, w, h); break;
        case 4: render_memory(ctx, px, w, h); break;
        case 5: render_general(ctx, px, w, h); break;
        case 6: render_about(ctx, px, w, h); break;
        case 7: render_lumo(ctx, px, w, h); break;
        case 8: render_processor(ctx, px, w, h); break;
        }
        return;
    }

    /* ── main page ── */
    lumo_app_draw_text(px, w, h, PAD, 16, 2, th.text_dim, "LUMO");
    lumo_app_draw_text(px, w, h, PAD, 42, 3, th.text, "Settings");

    int y = HEADER_Y;
    char buf[128];

    /* ── CONNECTIVITY ── */
    draw_section(px, w, h, y, "CONNECTIVITY");
    y += SECTION_H;
    {
        char iface[64], ssid[64], wstatus[64], sig[32], ip[32];
        get_wifi_info(iface, sizeof(iface), ssid, sizeof(ssid),
                      wstatus, sizeof(wstatus), sig, sizeof(sig),
                      ip, sizeof(ip));
        if (ssid[0] && strcmp(ssid, "--") != 0)
            snprintf(buf, sizeof(buf), "%s - %s", wstatus, ssid);
        else
            snprintf(buf, sizeof(buf), "%s (%s)", wstatus, iface);
        draw_row(px, w, h, y, "WI-FI", buf);
    }
    y += ROW_H;

    /* ── DEVICE ── */
    draw_section(px, w, h, y, "DEVICE");
    y += SECTION_H;
    snprintf(buf, sizeof(buf), "%ux%u PIXMAN", w, h);
    draw_row(px, w, h, y, "DISPLAY", buf); y += ROW_H;
    draw_row(px, w, h, y, "SOUND", "PIPEWIRE"); y += ROW_H;
    {
        struct statvfs st; char s[32] = "?";
        if (statvfs("/", &st) == 0) {
            unsigned long f = (unsigned long)(st.f_bavail *
                (st.f_frsize / 1024)) / 1024;
            snprintf(s, sizeof(s), "%lu MB FREE", f);
        }
        draw_row(px, w, h, y, "STORAGE", s);
    }
    y += ROW_H;
    {
        unsigned long total, avail, buffers, cached;
        get_mem_info(&total, &avail, &buffers, &cached);
        unsigned long used = total > avail ? total - avail : 0;
        snprintf(buf, sizeof(buf), "%lu / %lu MB", used / 1024, total / 1024);
        draw_row(px, w, h, y, "MEMORY", buf);
    }
    y += ROW_H;

    /* ── SYSTEM ── */
    draw_section(px, w, h, y, "SYSTEM");
    y += SECTION_H;
    {
        char up[32] = "?";
        FILE *fp = fopen("/proc/uptime", "r");
        if (fp) {
            double u = 0;
            if (fscanf(fp, "%lf", &u) == 1) {
                int d = (int)(u / 86400);
                int hr = (int)((u - d * 86400) / 3600);
                int mn = (int)((u - d * 86400 - hr * 3600) / 60);
                if (d > 0)
                    snprintf(up, sizeof(up), "%dD %dH %dM", d, hr, mn);
                else
                    snprintf(up, sizeof(up), "%dH %dM", hr, mn);
            }
            fclose(fp);
        }
        draw_row(px, w, h, y, "GENERAL", up);
    }
    y += ROW_H;
    {
        char hn[32] = "?";
        gethostname(hn, sizeof(hn) - 1);
        draw_row(px, w, h, y, "ABOUT", hn);
    }
    y += ROW_H;
    draw_row(px, w, h, y, "LUMO", LUMO_VERSION_STRING); y += ROW_H;
    {
        int cores = 0;
        FILE *fp = fopen("/proc/cpuinfo", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp))
                if (strstr(line, "processor")) cores++;
            fclose(fp);
        }
        snprintf(buf, sizeof(buf), "RISCV64 %d CORES", cores);
        draw_row(px, w, h, y, "PROCESSOR", buf);
    }
}
