#define _DEFAULT_SOURCE
#include "lumo/app.h"
#include "lumo/app_render.h"
#include "lumo/lumo_term.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <png.h>
#include <jpeglib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

struct lumo_app_client;

struct lumo_app_buffer {
    struct lumo_app_client *client;
    struct wl_buffer *buffer;
    struct wl_shm_pool *pool;
    void *data;
    int fd;
    size_t size;
    uint32_t width;
    uint32_t height;
    bool busy;
    struct wl_buffer_listener release;
};

struct lumo_app_client {
    enum lumo_app_id app_id;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct lumo_app_buffer *buffer;
    struct lumo_app_buffer *buffers[2];
    struct zwp_text_input_manager_v3 *text_input_manager;
    struct zwp_text_input_v3 *text_input;
    bool text_input_enabled;
    uint32_t width;
    uint32_t height;
    uint32_t pending_width;
    uint32_t pending_height;
    bool configured;
    bool running;
    bool pointer_pressed;
    bool pointer_position_valid;
    bool touch_pressed;
    bool close_active;
    int32_t active_touch_id;
    double pointer_x;
    double pointer_y;
    char browse_path[1024];
    double touch_down_x;
    double touch_down_y;
    struct wl_keyboard *keyboard;
    bool shift_held;
    char term_lines[16][82];
    int term_line_count;
    int term_cursor;
    char term_input[82];
    int term_input_len;
    int scroll_offset;
    bool term_menu_open;
    double zoom_scale;
    double pinch_base_scale;
    bool stopwatch_running;
    uint64_t stopwatch_start_ms;
    uint64_t stopwatch_accumulated_ms;
    int clock_tab;
    uint32_t timer_total_sec;
    uint64_t timer_start_ms;
    bool timer_running;
    uint32_t alarm_hour;
    uint32_t alarm_min;
    bool alarm_enabled;
    bool alarm_sound_played; /* prevents repeating sound within same minute */
    int selected_row;
    bool file_info_visible;
    bool scroll_active;       /* set when swipe-scroll detected this touch */
    bool text_view_active;    /* text file viewer mode */
    char file_info_name[256];
    char file_info_path[1100];
    char text_view_content[4096];
    int text_view_scroll;
    struct lumo_settings settings;
    char notes[8][128];
    int note_count;
    int note_editing;
    int pty_fd;
    pid_t pty_pid;
    char pty_line_buf[256];
    int pty_line_len;
    struct lumo_term term_state;
    char pending_commit[256];
    int pending_commit_len;
    char media_files[LUMO_APP_MEDIA_MAX_FILES][64];
    int media_file_count;
    int media_selected;
    bool media_playing;
    pid_t media_pid;
    pid_t browser_pid;
    bool photo_viewing;
    uint32_t *photo_thumbnails[LUMO_APP_MEDIA_MAX_FILES];
    uint32_t photo_thumbnail_widths[LUMO_APP_MEDIA_MAX_FILES];
    uint32_t photo_thumbnail_heights[LUMO_APP_MEDIA_MAX_FILES];
    uint32_t *photo_pixels;
    uint32_t photo_width;
    uint32_t photo_height;
};

static bool lumo_app_client_redraw(struct lumo_app_client *client);
static void lumo_app_notes_save(const struct lumo_app_client *client);
static void lumo_app_notes_load(struct lumo_app_client *client);
static void lumo_app_clock_save(const struct lumo_app_client *client);
static void lumo_app_clock_load(struct lumo_app_client *client);
static void lumo_app_settings_save(const struct lumo_app_client *client);
static void lumo_app_settings_load(struct lumo_app_client *client);
static void lumo_app_contacts_save(const struct lumo_app_client *client);
static void lumo_app_contacts_load(struct lumo_app_client *client);
static void lumo_app_places_save(const struct lumo_app_client *client);
static void lumo_app_places_load(struct lumo_app_client *client);
/* new app hit-test functions */
int lumo_app_browser_button_at(uint32_t width, uint32_t height,
    double x, double y);
static void lumo_app_bookmarks_load(struct lumo_app_client *client);
static void lumo_app_bookmarks_save(const struct lumo_app_client *client);
static void lumo_app_browser_launch_url(const char *url);
int lumo_app_phone_button_at(uint32_t width, uint32_t height,
    double x, double y, int tab);
int lumo_app_camera_button_at(uint32_t width, uint32_t height,
    double x, double y, bool gallery_mode);
int lumo_app_maps_button_at(uint32_t width, uint32_t height,
    double x, double y, int tab);
static void lumo_app_sync_text_input_state(
    struct lumo_app_client *client,
    bool flush_pending
);

static void lumo_app_clear_photo_thumbnails(struct lumo_app_client *client) {
    if (client == NULL) {
        return;
    }

    for (size_t i = 0; i < LUMO_APP_MEDIA_MAX_FILES; i++) {
        if (client->photo_thumbnails[i] != NULL) {
            free(client->photo_thumbnails[i]);
            client->photo_thumbnails[i] = NULL;
        }
        client->photo_thumbnail_widths[i] = 0;
        client->photo_thumbnail_heights[i] = 0;
    }
}

static bool lumo_app_media_path(
    const char *directory,
    const char *filename,
    char *path,
    size_t path_size
) {
    const char *home = getenv("HOME");
    int written;

    if (directory == NULL || filename == NULL || path == NULL || path_size == 0) {
        return false;
    }

    if (home == NULL || home[0] == '\0') {
        home = "/home/orangepi";
    }

    written = snprintf(path, path_size, "%s/%s/%s", home, directory, filename);
    return written > 0 && (size_t)written < path_size;
}

static void lumo_app_scan_media(struct lumo_app_client *client,
    const char *dir, const char **exts, int ext_count)
{
    DIR *d;
    struct dirent *entry;
    char path[1200];
    const char *home = getenv("HOME");

    if (home == NULL) home = "/home/orangepi";
    snprintf(path, sizeof(path), "%s/%s", home, dir);
    client->media_file_count = 0;
    client->media_selected = -1;
    client->media_playing = false;
    if (client->app_id == LUMO_APP_PHOTOS) {
        lumo_app_clear_photo_thumbnails(client);
    }

    d = opendir(path);
    if (d == NULL) {
        /* create the directory if it doesn't exist */
        (void)mkdir(path, 0755);
        return;
    }

    while ((entry = readdir(d)) != NULL &&
            client->media_file_count < (int)LUMO_APP_MEDIA_MAX_FILES) {
        if (entry->d_name[0] == '.') continue;
        const char *dot = strrchr(entry->d_name, '.');
        if (dot == NULL) continue;
        bool match = false;
        for (int i = 0; i < ext_count; i++) {
            if (strcasecmp(dot, exts[i]) == 0) {
                match = true;
                break;
            }
        }
        if (match) {
            strncpy(client->media_files[client->media_file_count],
                entry->d_name,
                sizeof(client->media_files[0]) - 1);
            client->media_files[client->media_file_count]
                [sizeof(client->media_files[0]) - 1] = '\0';
            client->media_file_count++;
        }
    }
    closedir(d);
}

static void lumo_app_media_play(struct lumo_app_client *client,
    const char *dir)
{
    if (client->media_selected < 0 ||
            client->media_selected >= client->media_file_count) {
        return;
    }
    /* kill previous playback */
    if (client->media_pid > 0) {
        kill(client->media_pid, SIGTERM);
        waitpid(client->media_pid, NULL, WNOHANG);
        client->media_pid = 0;
    }
    char path[1300];
    const char *home = getenv("HOME");
    if (home == NULL) home = "/home/orangepi";
    snprintf(path, sizeof(path), "%s/%s/%s", home, dir,
        client->media_files[client->media_selected]);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        execlp("mpv", "mpv", "--no-video", "--really-quiet",
            path, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        client->media_pid = pid;
        client->media_playing = true;
    }
}

static void lumo_app_media_stop(struct lumo_app_client *client)
{
    if (client->media_pid > 0) {
        kill(client->media_pid, SIGTERM);
        /* Blocking wait: we just sent SIGTERM and must confirm the child has
         * exited before clearing media_pid to avoid leaving a zombie.
         * No race condition is possible here: the app client runs in a
         * single-threaded poll-based event loop, so media_pid cannot be
         * modified concurrently. */
        waitpid(client->media_pid, NULL, 0);
        client->media_pid = 0;
    }
    client->media_playing = false;
}

static bool lumo_app_load_png(const char *path, uint32_t **pixels_out,
    uint32_t *w_out, uint32_t *h_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
        NULL, NULL, NULL);
    if (!png) { fclose(fp); return false; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return false; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info);
    uint32_t w = png_get_image_width(png, info);
    uint32_t h = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_bgr(png); /* ARGB format */
    png_read_update_info(png, info);

    if (w > 4096 || h > 4096) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }

    uint32_t *pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!pixels) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }

    png_bytep *rows = malloc(sizeof(png_bytep) * h);
    if (!rows) { free(pixels); png_destroy_read_struct(&png, &info, NULL); fclose(fp); return false; }
    for (uint32_t y = 0; y < h; y++)
        rows[y] = (png_bytep)(pixels + y * w);
    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    *pixels_out = pixels;
    *w_out = w;
    *h_out = h;
    return true;
}

static bool lumo_app_load_jpeg(const char *path, uint32_t **pixels_out,
    uint32_t *w_out, uint32_t *h_out)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    uint32_t w = cinfo.output_width;
    uint32_t h = cinfo.output_height;
    if (w > 4096 || h > 4096) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return false;
    }

    uint32_t *pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!pixels) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return false;
    }

    unsigned char *row_buf = malloc((size_t)w * 3u);
    if (!row_buf) { free(pixels); jpeg_finish_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); fclose(fp); return false; }
    while (cinfo.output_scanline < h) {
        unsigned char *p = row_buf;
        jpeg_read_scanlines(&cinfo, &p, 1);
        uint32_t y = cinfo.output_scanline - 1;
        for (uint32_t x = 0; x < w; x++) {
            pixels[y * w + x] = 0xFF000000 |
                ((uint32_t)row_buf[x * 3] << 16) |
                ((uint32_t)row_buf[x * 3 + 1] << 8) |
                (uint32_t)row_buf[x * 3 + 2];
        }
    }
    free(row_buf);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    *pixels_out = pixels;
    *w_out = w;
    *h_out = h;
    return true;
}

static bool lumo_app_load_pixbuf(
    const char *path,
    int max_width,
    int max_height,
    uint32_t **pixels_out,
    uint32_t *w_out,
    uint32_t *h_out
) {
    GdkPixbuf *pixbuf = NULL;
    GdkPixbuf *oriented = NULL;
    GError *error = NULL;
    uint32_t *pixels = NULL;
    int width;
    int height;
    int rowstride;
    int channels;
    gboolean has_alpha;
    guchar *src;

    if (path == NULL || pixels_out == NULL || w_out == NULL || h_out == NULL) {
        return false;
    }

    if (max_width > 0 && max_height > 0) {
        pixbuf = gdk_pixbuf_new_from_file_at_scale(path, max_width, max_height,
            true, &error);
    } else {
        pixbuf = gdk_pixbuf_new_from_file(path, &error);
    }
    if (pixbuf == NULL) {
        if (error != NULL) {
            g_error_free(error);
        }
        return false;
    }

    oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
    if (oriented != NULL) {
        g_object_unref(pixbuf);
        pixbuf = oriented;
    }

    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        g_object_unref(pixbuf);
        return false;
    }

    channels = gdk_pixbuf_get_n_channels(pixbuf);
    has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    src = gdk_pixbuf_get_pixels(pixbuf);
    if (src == NULL || channels < 3 || gdk_pixbuf_get_bits_per_sample(pixbuf) != 8) {
        g_object_unref(pixbuf);
        return false;
    }

    pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (pixels == NULL) {
        g_object_unref(pixbuf);
        return false;
    }

    for (int y = 0; y < height; y++) {
        const guchar *row = src + (size_t)y * (size_t)rowstride;
        for (int x = 0; x < width; x++) {
            const guchar *p = row + (size_t)x * (size_t)channels;
            uint32_t alpha = has_alpha ? p[3] : 0xFFu;
            pixels[(size_t)y * (size_t)width + (size_t)x] =
                (alpha << 24) |
                ((uint32_t)p[0] << 16) |
                ((uint32_t)p[1] << 8) |
                (uint32_t)p[2];
        }
    }

    g_object_unref(pixbuf);
    *pixels_out = pixels;
    *w_out = (uint32_t)width;
    *h_out = (uint32_t)height;
    return true;
}

static void lumo_app_prepare_photo_thumbnails(struct lumo_app_client *client) {
    char path[1300];

    if (client == NULL) {
        return;
    }

    lumo_app_clear_photo_thumbnails(client);
    for (int i = 0; i < client->media_file_count &&
            i < (int)LUMO_APP_MEDIA_MAX_FILES; i++) {
        if (!lumo_app_media_path("Pictures", client->media_files[i], path,
                sizeof(path))) {
            continue;
        }
        (void)lumo_app_load_pixbuf(path, 192, 144,
            &client->photo_thumbnails[i],
            &client->photo_thumbnail_widths[i],
            &client->photo_thumbnail_heights[i]);
    }
}

static bool lumo_app_load_image(struct lumo_app_client *client)
{
    if (client->media_selected < 0 ||
            client->media_selected >= client->media_file_count) {
        return false;
    }

    char path[1300];
    if (!lumo_app_media_path("Pictures",
            client->media_files[client->media_selected], path,
            sizeof(path))) {
        return false;
    }

    /* free previous */
    if (client->photo_pixels) {
        free(client->photo_pixels);
        client->photo_pixels = NULL;
    }
    client->photo_width = 0;
    client->photo_height = 0;

    const char *dot = strrchr(path, '.');
    if (dot && (strcasecmp(dot, ".png") == 0)) {
        return lumo_app_load_png(path, &client->photo_pixels,
            &client->photo_width, &client->photo_height);
    }
    if (dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)) {
        return lumo_app_load_jpeg(path, &client->photo_pixels,
            &client->photo_width, &client->photo_height);
    }
    return lumo_app_load_pixbuf(path, 0, 0, &client->photo_pixels,
        &client->photo_width, &client->photo_height);
}

/* lumo_app_term_add_line removed — replaced by lumo_term_feed VT100 parser */

static void lumo_app_term_write(struct lumo_app_client *client,
    const char *data, size_t len)
{
    ssize_t ret;
    if (client->pty_fd < 0 || len == 0) return;
    ret = write(client->pty_fd, data, len);
    if (ret < 0 && errno != EAGAIN && errno != EINTR) {
        fprintf(stderr, "lumo-app: pty write failed: %s\n", strerror(errno));
    }
}

static bool lumo_app_pty_setup(struct lumo_app_client *client) {
    struct winsize ws = {
        .ws_row = 24,
        .ws_col = 80,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    pid_t pid;
    int master_fd;
    const char *shell;

    pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        fprintf(stderr, "lumo-app: forkpty failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        shell = getenv("SHELL");
        if (shell == NULL || shell[0] == '\0') shell = "/bin/bash";
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execlp(shell, shell, (char *)NULL);
        /* fallback to /bin/sh if user's shell fails */
        execlp("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }

    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    client->pty_fd = master_fd;
    client->pty_pid = pid;
    client->pty_line_len = 0;
    lumo_term_init(&client->term_state, 80, 24);
    return true;
}

static void lumo_app_pty_read(struct lumo_app_client *client) {
    char buf[4096];
    ssize_t n;

    if (client->pty_fd < 0) return;

    while ((n = read(client->pty_fd, buf, sizeof(buf))) > 0) {
        lumo_term_feed(&client->term_state, buf, (size_t)n);
    }

    /* drain any response bytes (DA, DSR) back to the PTY */
    char resp[64];
    int resp_len = lumo_term_drain_response(&client->term_state,
        resp, sizeof(resp));
    if (resp_len > 0) {
        (void)write(client->pty_fd, resp, (size_t)resp_len);
    }

    (void)lumo_app_client_redraw(client);
}

static void lumo_app_pty_cleanup(struct lumo_app_client *client) {
    if (client->pty_fd >= 0) {
        close(client->pty_fd);
        client->pty_fd = -1;
    }
    if (client->pty_pid > 0) {
        kill(client->pty_pid, SIGTERM);
        waitpid(client->pty_pid, NULL, WNOHANG);
        client->pty_pid = 0;
    }
}

static int lumo_app_create_shm_file(size_t size) {
    char template[] = "/tmp/lumo-app-XXXXXX";
    int fd = mkstemp(template);

    if (fd < 0) {
        return -1;
    }

    unlink(template);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void lumo_app_buffer_destroy(struct lumo_app_buffer *buffer) {
    if (buffer == NULL) return;
    if (buffer->buffer != NULL) wl_buffer_destroy(buffer->buffer);
    if (buffer->pool != NULL) wl_shm_pool_destroy(buffer->pool);
    if (buffer->data != NULL) munmap(buffer->data, buffer->size);
    if (buffer->fd >= 0) close(buffer->fd);
    free(buffer);
}

static void lumo_app_buffer_release(
    void *data,
    struct wl_buffer *wl_buffer
) {
    struct lumo_app_buffer *buffer = data;
    (void)wl_buffer;
    if (buffer != NULL)
        buffer->busy = false;
}

static bool lumo_app_client_close_contains(
    const struct lumo_app_client *client,
    double x,
    double y
) {
    struct lumo_rect rect = {0};

    if (client == NULL ||
            !lumo_app_close_rect(client->width, client->height, &rect)) {
        return false;
    }

    return lumo_rect_contains(&rect, x, y);
}

static void lumo_app_client_set_close_active(
    struct lumo_app_client *client,
    bool close_active
) {
    if (client == NULL || client->close_active == close_active) {
        return;
    }

    client->close_active = close_active;
    (void)lumo_app_client_redraw(client);
}

/* double-buffered SHM allocation — reuses buffers instead of
 * allocating per frame (eliminates mmap/munmap TLB flush overhead) */
static struct lumo_app_buffer *lumo_app_get_free_buffer(
    struct lumo_app_client *client
) {
    size_t stride, size;
    int fd;

    if (client == NULL || client->shm == NULL) return NULL;
    if (client->width > SIZE_MAX / 4u) return NULL;
    stride = (size_t)client->width * 4u;
    if (client->height > 0 && stride > SIZE_MAX / (size_t)client->height)
        return NULL;
    size = stride * (size_t)client->height;

    /* try to reuse an existing buffer */
    for (int i = 0; i < 2; i++) {
        struct lumo_app_buffer *b = client->buffers[i];
        if (b != NULL && !b->busy &&
                b->width == client->width && b->height == client->height)
            return b;
    }

    /* find a free slot and allocate a new buffer */
    int slot = -1;
    for (int i = 0; i < 2; i++) {
        if (client->buffers[i] == NULL) { slot = i; break; }
        if (!client->buffers[i]->busy) {
            /* wrong size — destroy and reuse slot */
            lumo_app_buffer_destroy(client->buffers[i]);
            client->buffers[i] = NULL;
            slot = i;
            break;
        }
    }
    if (slot < 0) return NULL; /* both busy */

    fd = lumo_app_create_shm_file(size);
    if (fd < 0) return NULL;

    struct lumo_app_buffer *buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) { close(fd); return NULL; }

    buffer->client = client;
    buffer->fd = fd;
    buffer->size = size;
    buffer->width = client->width;
    buffer->height = client->height;
    buffer->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->data == MAP_FAILED) {
        close(fd); free(buffer); return NULL;
    }

    buffer->pool = wl_shm_create_pool(client->shm, fd, (int)size);
    if (buffer->pool == NULL) {
        munmap(buffer->data, size); close(fd); free(buffer); return NULL;
    }

    buffer->buffer = wl_shm_pool_create_buffer(buffer->pool, 0,
        (int)client->width, (int)client->height, (int)stride,
        WL_SHM_FORMAT_ARGB8888);
    if (buffer->buffer == NULL) {
        wl_shm_pool_destroy(buffer->pool);
        munmap(buffer->data, size); close(fd); free(buffer); return NULL;
    }

    buffer->release.release = lumo_app_buffer_release;
    wl_buffer_add_listener(buffer->buffer, &buffer->release, buffer);
    client->buffers[slot] = buffer;
    return buffer;
}

static bool lumo_app_client_draw_buffer(struct lumo_app_client *client) {
    struct lumo_app_buffer *buffer;

    if (client == NULL || client->shm == NULL || client->surface == NULL ||
            client->width == 0 || client->height == 0) {
        return false;
    }

    buffer = lumo_app_get_free_buffer(client);
    if (buffer == NULL || buffer->data == NULL) return false;

    {
        uint64_t sw_elapsed = client->stopwatch_accumulated_ms;
        if (client->stopwatch_running && client->stopwatch_start_ms > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 +
                (uint64_t)ts.tv_nsec / 1000000;
            sw_elapsed += now_ms - client->stopwatch_start_ms;
        }
        /* compute timer remaining */
        uint32_t timer_remaining = client->timer_total_sec;
        if (client->timer_running && client->timer_start_ms > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 +
                (uint64_t)ts.tv_nsec / 1000000;
            uint64_t elapsed_sec = (now_ms - client->timer_start_ms) / 1000;
            if (elapsed_sec >= client->timer_total_sec) {
                client->timer_running = false;
                timer_remaining = 0;
                /* timer finished — flash accent to signal completion */
            } else {
                timer_remaining = client->timer_total_sec -
                    (uint32_t)elapsed_sec;
            }
        }
        /* check alarm: fire visual indicator when current time matches */
        bool alarm_firing = false;
        if (client->alarm_enabled) {
            time_t now_t = time(NULL);
            struct tm tm_now;
            localtime_r(&now_t, &tm_now);
            if ((uint32_t)tm_now.tm_hour == client->alarm_hour &&
                    (uint32_t)tm_now.tm_min == client->alarm_min) {
                alarm_firing = true;
                /* play alarm sound once per trigger */
                if (!client->alarm_sound_played) {
                    client->alarm_sound_played = true;
                    pid_t pid = fork();
                    if (pid == 0) {
                        /* try pw-play first, fall back to aplay */
                        execlp("pw-play", "pw-play",
                            "/usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga",
                            (char *)NULL);
                        execlp("aplay", "aplay", "-q",
                            "/usr/share/sounds/alsa/Front_Center.wav",
                            (char *)NULL);
                        _exit(0);
                    }
                }
            } else {
                client->alarm_sound_played = false;
            }
        }
        struct lumo_app_render_context ctx = {
            .app_id = client->app_id,
            .close_active = client->close_active,
            .browse_path = client->browse_path,
            .scroll_offset = client->scroll_offset,
            .stopwatch_running = client->stopwatch_running,
            .stopwatch_elapsed_ms = sw_elapsed,
            .clock_tab = client->clock_tab,
            .timer_total_sec = client->timer_total_sec,
            .timer_remaining_sec = timer_remaining,
            .timer_running = client->timer_running,
            .alarm_hour = client->alarm_hour,
            .alarm_min = client->alarm_min,
            .alarm_enabled = client->alarm_enabled,
            .alarm_firing = alarm_firing,
            .selected_row = client->selected_row,
            .file_info_visible = client->file_info_visible,
            .text_view_active = client->text_view_active,
            .settings = client->settings,
            .note_count = client->note_count,
            .note_editing = client->note_editing,
            .term_input_len = client->term_input_len,
            .term = (client->pty_fd >= 0) ? &client->term_state : NULL,
            .term_menu_open = client->term_menu_open,
            .zoom_scale = client->zoom_scale,
            .media_file_count = client->media_file_count,
            .media_selected = client->media_selected,
            .media_playing = client->media_playing,
            .photo_viewing = client->photo_viewing,
            .photo_pixels = client->photo_pixels,
            .photo_width = client->photo_width,
            .photo_height = client->photo_height,
        };
        memcpy(ctx.file_info_name, client->file_info_name,
            sizeof(ctx.file_info_name));
        memcpy(ctx.file_info_path, client->file_info_path,
            sizeof(ctx.file_info_path));
        if (client->text_view_active)
            memcpy(ctx.text_view_content, client->text_view_content,
                sizeof(ctx.text_view_content));
        memcpy(ctx.notes, client->notes, sizeof(ctx.notes));
        memcpy(ctx.term_input, client->term_input, sizeof(ctx.term_input));
        memcpy(ctx.media_files, client->media_files,
            sizeof(ctx.media_files));
        memcpy(ctx.photo_thumbnails, client->photo_thumbnails,
            sizeof(ctx.photo_thumbnails));
        memcpy(ctx.photo_thumbnail_widths, client->photo_thumbnail_widths,
            sizeof(ctx.photo_thumbnail_widths));
        memcpy(ctx.photo_thumbnail_heights, client->photo_thumbnail_heights,
            sizeof(ctx.photo_thumbnail_heights));
        lumo_app_render(&ctx, buffer->data, client->width, client->height);

        /* cache app surface to NVMe — throttled to once every 10 seconds
         * to avoid I/O stalls on every frame redraw */
        {
            static uint64_t last_cache_s = 0;
            struct timespec cache_ts;
            clock_gettime(CLOCK_MONOTONIC, &cache_ts);
            uint64_t now_s = (uint64_t)cache_ts.tv_sec;
            if (now_s >= last_cache_s + 10) {
                last_cache_s = now_s;
                char cache_path[256];
                snprintf(cache_path, sizeof(cache_path),
                    "/data/lumo-cache/surfaces/%s.lumosurf",
                    lumo_app_id_name(client->app_id));
                FILE *cfp = fopen(cache_path, "wb");
                if (cfp != NULL) {
                    fwrite(&client->width, 4, 1, cfp);
                    fwrite(&client->height, 4, 1, cfp);
                    fwrite(buffer->data, 4,
                        (size_t)client->width * client->height, cfp);
                    fclose(cfp);
                }
            }
        }
    }
    wl_surface_attach(client->surface, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(client->surface, 0, 0, (int)client->width,
        (int)client->height);
    wl_surface_commit(client->surface);
    buffer->busy = true;
    client->buffer = buffer;
    return true;
}

static bool lumo_app_client_redraw(struct lumo_app_client *client) {
    if (client == NULL || !client->configured) {
        return false;
    }

    return lumo_app_client_draw_buffer(client);
}

static void lumo_app_wm_base_handle_ping(
    void *data,
    struct xdg_wm_base *xdg_wm_base,
    uint32_t serial
) {
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener lumo_app_wm_base_listener = {
    .ping = lumo_app_wm_base_handle_ping,
};

static void lumo_app_xdg_surface_handle_configure(
    void *data,
    struct xdg_surface *xdg_surface,
    uint32_t serial
) {
    struct lumo_app_client *client = data;
    uint32_t width;
    uint32_t height;

    if (client == NULL) {
        return;
    }

    xdg_surface_ack_configure(xdg_surface, serial);
    width = client->pending_width != 0 ? client->pending_width : client->width;
    height = client->pending_height != 0 ? client->pending_height : client->height;
    if (width == 0) {
        width = 1280;
    }
    if (height == 0) {
        height = 800;
    }

    client->width = width;
    client->height = height;
    client->configured = true;
    if (!lumo_app_client_redraw(client)) {
        fprintf(stderr, "lumo-app: failed to render app surface\n");
    }
}

static const struct xdg_surface_listener lumo_app_xdg_surface_listener = {
    .configure = lumo_app_xdg_surface_handle_configure,
};

static void lumo_app_xdg_toplevel_handle_configure(
    void *data,
    struct xdg_toplevel *xdg_toplevel,
    int32_t width,
    int32_t height,
    struct wl_array *states
) {
    struct lumo_app_client *client = data;

    (void)xdg_toplevel;
    (void)states;
    if (client == NULL) {
        return;
    }

    if (width > 0) {
        client->pending_width = (uint32_t)width;
    }
    if (height > 0) {
        client->pending_height = (uint32_t)height;
    }
}

static void lumo_app_xdg_toplevel_handle_close(
    void *data,
    struct xdg_toplevel *xdg_toplevel
) {
    struct lumo_app_client *client = data;

    (void)xdg_toplevel;
    if (client != NULL) {
        fprintf(stderr, "lumo-app: xdg_toplevel close received\n");
        client->running = false;
    }
}

static const struct xdg_toplevel_listener lumo_app_xdg_toplevel_listener = {
    .configure = lumo_app_xdg_toplevel_handle_configure,
    .close = lumo_app_xdg_toplevel_handle_close,
};

static void lumo_app_pointer_handle_enter(
    void *data,
    struct wl_pointer *wl_pointer,
    uint32_t serial,
    struct wl_surface *surface,
    wl_fixed_t surface_x,
    wl_fixed_t surface_y
) {
    struct lumo_app_client *client = data;

    (void)wl_pointer;
    (void)serial;
    (void)surface;
    if (client == NULL) {
        return;
    }

    client->pointer_x = wl_fixed_to_double(surface_x);
    client->pointer_y = wl_fixed_to_double(surface_y);
    client->pointer_position_valid = true;
}

static void lumo_app_pointer_handle_leave(
    void *data,
    struct wl_pointer *wl_pointer,
    uint32_t serial,
    struct wl_surface *surface
) {
    struct lumo_app_client *client = data;

    (void)wl_pointer;
    (void)serial;
    (void)surface;
    if (client == NULL) {
        return;
    }

    client->pointer_position_valid = false;
    if (!client->touch_pressed) {
        lumo_app_client_set_close_active(client, false);
    }
}

static void lumo_app_pointer_handle_motion(
    void *data,
    struct wl_pointer *wl_pointer,
    uint32_t time,
    wl_fixed_t surface_x,
    wl_fixed_t surface_y
) {
    struct lumo_app_client *client = data;

    (void)wl_pointer;
    (void)time;
    if (client == NULL) {
        return;
    }

    client->pointer_x = wl_fixed_to_double(surface_x);
    client->pointer_y = wl_fixed_to_double(surface_y);
    client->pointer_position_valid = true;
    if (client->pointer_pressed) {
        lumo_app_client_set_close_active(client,
            lumo_app_client_close_contains(client, client->pointer_x,
                client->pointer_y));
    }
}

static void lumo_app_pointer_handle_button(
    void *data,
    struct wl_pointer *wl_pointer,
    uint32_t serial,
    uint32_t time,
    uint32_t button,
    uint32_t state
) {
    struct lumo_app_client *client = data;
    bool should_close = false;

    (void)wl_pointer;
    (void)serial;
    (void)time;
    (void)button;
    if (client == NULL || !client->pointer_position_valid) {
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        client->pointer_pressed = true;
        lumo_app_client_set_close_active(client,
            lumo_app_client_close_contains(client, client->pointer_x,
                client->pointer_y));
        return;
    }

    /* close button disabled — apps are dismissed via compositor gesture */
    (void)should_close;
    client->pointer_pressed = false;
    lumo_app_client_set_close_active(client, false);
}

static void lumo_app_pointer_handle_axis(
    void *data,
    struct wl_pointer *wl_pointer,
    uint32_t time,
    uint32_t axis,
    wl_fixed_t value
) {
    (void)data;
    (void)wl_pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static void lumo_app_pointer_handle_frame(
    void *data,
    struct wl_pointer *wl_pointer
) {
    (void)data;
    (void)wl_pointer;
}

static const struct wl_pointer_listener lumo_app_pointer_listener = {
    .enter = lumo_app_pointer_handle_enter,
    .leave = lumo_app_pointer_handle_leave,
    .motion = lumo_app_pointer_handle_motion,
    .button = lumo_app_pointer_handle_button,
    .axis = lumo_app_pointer_handle_axis,
    .frame = lumo_app_pointer_handle_frame,
};

static void lumo_app_touch_handle_down(
    void *data,
    struct wl_touch *wl_touch,
    uint32_t serial,
    uint32_t time,
    struct wl_surface *surface,
    int32_t id,
    wl_fixed_t x,
    wl_fixed_t y
) {
    struct lumo_app_client *client = data;

    (void)wl_touch;
    (void)serial;
    (void)time;
    (void)surface;
    if (client == NULL) {
        return;
    }

    client->touch_pressed = true;
    client->active_touch_id = id;
    client->scroll_active = false;
    client->touch_down_x = wl_fixed_to_double(x);
    client->touch_down_y = wl_fixed_to_double(y);
    lumo_app_client_set_close_active(client,
        lumo_app_client_close_contains(client, client->touch_down_x,
            client->touch_down_y));

    /* try to enable text-input on touch if not already enabled —
     * flush pending events first so the compositor's enter event
     * sets focused_surface before we call enable()+commit() */
    if (!client->text_input_enabled &&
            lumo_app_wants_osk(client->app_id, client->note_editing)) {
        lumo_app_sync_text_input_state(client, true);
    }
}

static void lumo_app_touch_handle_up(
    void *data,
    struct wl_touch *wl_touch,
    uint32_t serial,
    uint32_t time,
    int32_t id
) {
    struct lumo_app_client *client = data;
    bool should_close;

    (void)wl_touch;
    (void)serial;
    (void)time;
    if (client == NULL || !client->touch_pressed ||
            client->active_touch_id != id) {
        return;
    }

    should_close = client->close_active;
    client->touch_pressed = false;
    client->active_touch_id = -1;
    lumo_app_client_set_close_active(client, false);
    /* close button disabled — apps are dismissed via bottom-edge
     * swipe gesture in the compositor, not via a touch target */
    (void)should_close;

    /* terminal menu */
    if (client->app_id == LUMO_APP_MESSAGES) {
        if (client->term_menu_open) {
            /* centered menu hit test */
            int mx = (int)client->touch_down_x;
            int my = (int)client->touch_down_y;
            int menu_w = (int)client->width * 2 / 3;
            if (menu_w < 240) menu_w = 240;
            int menu_x = ((int)client->width - menu_w) / 2;
            int menu_y = ((int)client->height - 220) / 2;
            int item_h = 44;
            int iy = menu_y + 46;
            if (mx >= menu_x && mx <= menu_x + menu_w) {
                if (my >= iy && my < iy + item_h) {
                    /* New — reset terminal */
                    lumo_term_reset(&client->term_state);
                    client->term_menu_open = false;
                    (void)lumo_app_client_redraw(client);
                    return;
                } else if (my >= iy + item_h && my < iy + item_h * 2) {
                    /* Keyboard — toggle OSK via text-input */
                    if (client->text_input != NULL) {
                        /* dispatch pending to get enter event first */
                        wl_display_roundtrip(client->display);

                        if (client->text_input_enabled) {
                            zwp_text_input_v3_disable(client->text_input);
                            zwp_text_input_v3_commit(client->text_input);
                            client->text_input_enabled = false;
                        } else {
                            zwp_text_input_v3_enable(client->text_input);
                            zwp_text_input_v3_set_content_type(
                                client->text_input,
                                ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
                            zwp_text_input_v3_commit(client->text_input);
                            client->text_input_enabled = true;
                        }
                        wl_display_flush(client->display);
                        fprintf(stderr, "lumo-app: keyboard toggled %s\n",
                            client->text_input_enabled ? "on" : "off");
                    }
                    client->term_menu_open = false;
                    (void)lumo_app_client_redraw(client);
                    return;
                } else if (my >= iy + item_h * 2 && my < iy + item_h * 3) {
                    /* Settings — placeholder */
                    client->term_menu_open = false;
                    (void)lumo_app_client_redraw(client);
                    return;
                } else if (my >= iy + item_h * 3 && my < iy + item_h * 4) {
                    /* About — placeholder */
                    client->term_menu_open = false;
                    (void)lumo_app_client_redraw(client);
                    return;
                }
            }
            /* tap outside menu closes it */
            client->term_menu_open = false;
            (void)lumo_app_client_redraw(client);
            return;
        }
        /* tap on title "LUMO TERMINAL" opens menu */
        if (client->touch_down_y < 38 && client->touch_down_x < 200) {
            client->term_menu_open = true;
            (void)lumo_app_client_redraw(client);
            return;
        }
    }

    /* media apps: tap on list item to select, tap selected to play/pause */
    if ((client->app_id == LUMO_APP_MUSIC ||
            client->app_id == LUMO_APP_PHOTOS ||
            client->app_id == LUMO_APP_VIDEOS) &&
            client->media_file_count > 0) {
        int ty = (int)client->touch_down_y;
        int list_start = (client->media_selected >= 0 &&
            client->app_id == LUMO_APP_VIDEOS)
            ? 56 + (int)client->height / 3 + 72 : 144;
        if (client->app_id == LUMO_APP_PHOTOS) {
            if (client->photo_viewing) {
                /* tap while viewing = exit viewer */
                client->photo_viewing = false;
                if (client->photo_pixels) {
                    free(client->photo_pixels);
                    client->photo_pixels = NULL;
                }
                client->photo_width = 0;
                client->photo_height = 0;
                (void)lumo_app_client_redraw(client);
            } else {
                /* grid: compute cell from touch position */
                int cols = 3;
                int pad = 8;
                int cell_w = ((int)client->width - pad * (cols + 1)) / cols;
                int cell_h = cell_w * 3 / 4;
                int col = ((int)client->touch_down_x - pad) / (cell_w + pad);
                int row_idx = (ty - 56) / (cell_h + pad);
                int idx = client->scroll_offset + row_idx * cols + col;
                if (col >= 0 && col < cols && idx >= 0 &&
                        idx < client->media_file_count) {
                    if (idx == client->media_selected) {
                        /* double-tap selected = view photo */
                        if (lumo_app_load_image(client)) {
                            client->photo_viewing = true;
                        }
                    } else {
                        client->media_selected = idx;
                    }
                    (void)lumo_app_client_redraw(client);
                }
            }
        } else if (ty >= list_start) {
            int row_h = (client->app_id == LUMO_APP_MUSIC) ? 36 : 40;
            int idx = client->scroll_offset +
                (ty - list_start) / row_h;
            if (idx >= 0 && idx < client->media_file_count) {
                if (idx == client->media_selected) {
                    /* tap selected item: toggle play */
                    if (client->media_playing) {
                        lumo_app_media_stop(client);
                    } else {
                        const char *dir =
                            client->app_id == LUMO_APP_MUSIC ? "Music" :
                            "Videos";
                        lumo_app_media_play(client, dir);
                    }
                } else {
                    client->media_selected = idx;
                }
                (void)lumo_app_client_redraw(client);
            }
        } else if (client->media_selected >= 0 && ty < list_start) {
            /* tap on now playing / preview area: toggle */
            if (client->media_playing) {
                lumo_app_media_stop(client);
            } else {
                const char *dir =
                    client->app_id == LUMO_APP_MUSIC ? "Music" : "Videos";
                lumo_app_media_play(client, dir);
            }
            (void)lumo_app_client_redraw(client);
        }
    }

    if (client->app_id == LUMO_APP_CLOCK && client->width > 0 &&
            client->height > 0) {
        int ty = (int)client->touch_down_y;
        int tx = (int)client->touch_down_x;
        int tab_w = (int)client->width / 4;
        int cx = (int)client->width / 2;

        /* tab bar taps (y 48-84) */
        if (ty >= 48 && ty < 84) {
            int new_tab = tx / tab_w;
            if (new_tab >= 0 && new_tab <= 3 &&
                    new_tab != client->clock_tab) {
                client->clock_tab = new_tab;
                (void)lumo_app_client_redraw(client);
            }
            return;
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 +
            (uint64_t)ts.tv_nsec / 1000000;

        if (client->clock_tab == 2) {
            /* stopwatch: left half = start/stop, right half = reset */
            if (ty >= 240) {
                if (tx < cx) {
                    /* start/stop */
                    if (client->stopwatch_running) {
                        client->stopwatch_accumulated_ms +=
                            now_ms - client->stopwatch_start_ms;
                        client->stopwatch_running = false;
                    } else {
                        client->stopwatch_start_ms = now_ms;
                        client->stopwatch_running = true;
                    }
                } else {
                    /* reset */
                    client->stopwatch_accumulated_ms = 0;
                    client->stopwatch_running = false;
                    client->stopwatch_start_ms = 0;
                }
                (void)lumo_app_client_redraw(client);
            }
        } else if (client->clock_tab == 3) {
            /* timer controls */
            if (ty >= 230 && ty < 266) {
                /* +1M or +5M */
                if (tx < cx) {
                    client->timer_total_sec += 60;
                } else {
                    client->timer_total_sec += 300;
                }
                lumo_app_clock_save(client);
                (void)lumo_app_client_redraw(client);
            } else if (ty >= 280) {
                if (tx < cx) {
                    /* start/stop */
                    if (client->timer_running) {
                        client->timer_running = false;
                    } else if (client->timer_total_sec > 0) {
                        client->timer_start_ms = now_ms;
                        client->timer_running = true;
                    }
                } else {
                    /* reset */
                    client->timer_running = false;
                    client->timer_total_sec = 0;
                    client->timer_start_ms = 0;
                }
                (void)lumo_app_client_redraw(client);
            }
        } else if (client->clock_tab == 1) {
            /* alarm controls */
            if (ty >= 190 && ty < 226) {
                /* toggle alarm */
                client->alarm_enabled = !client->alarm_enabled;
                lumo_app_clock_save(client);
                (void)lumo_app_client_redraw(client);
            } else if (ty >= 242) {
                if (tx < cx) {
                    client->alarm_hour = (client->alarm_hour + 1) % 24;
                } else {
                    client->alarm_min = (client->alarm_min + 5) % 60;
                }
                lumo_app_clock_save(client);
                (void)lumo_app_client_redraw(client);
            }
        }

        /* save tab selection */
        if (ty >= 48 && ty < 84) {
            lumo_app_clock_save(client);
        }
    }

    /* GitHub app touch handling */
    if (client->app_id == LUMO_APP_GITHUB && client->width > 0 &&
            client->height > 0) {
        if (client->scroll_active) {
            client->scroll_active = false;
            (void)lumo_app_client_redraw(client);
            return;
        }
        int btn = lumo_app_github_button_at(client->width, client->height,
            client->touch_down_x, client->touch_down_y);
        if (btn >= 0) {
            /* send button press to GitHub app */
            extern void lumo_app_github_handle_tap(int btn);
            lumo_app_github_handle_tap(btn);
            (void)lumo_app_client_redraw(client);
        }
        return;
    }

    if (client->app_id == LUMO_APP_FILES && client->width > 0 &&
            client->height > 0) {
        /* dismiss text viewer on any tap */
        if (client->text_view_active) {
            /* scroll text viewer on swipe, dismiss on tap */
            if (client->scroll_active) {
                /* scroll was handled in motion */
            } else {
                client->text_view_active = false;
                client->text_view_scroll = 0;
            }
            (void)lumo_app_client_redraw(client);
            return;
        }

        /* dismiss file info overlay on any tap */
        if (client->file_info_visible) {
            client->file_info_visible = false;
            client->selected_row = -1;
            (void)lumo_app_client_redraw(client);
            return;
        }

        /* ignore taps that were actually scrolls */
        if (client->scroll_active) {
            client->scroll_active = false;
            return;
        }

        /* UP button */
        if (lumo_app_files_up_button_at(client->width, client->height,
                client->touch_down_x, client->touch_down_y) == -2) {
            char *last_slash = strrchr(client->browse_path, '/');
            if (last_slash != NULL && last_slash != client->browse_path) {
                *last_slash = '\0';
            } else if (last_slash == client->browse_path) {
                client->browse_path[1] = '\0';
            }
            client->scroll_offset = 0;
            client->selected_row = -1;
            (void)lumo_app_client_redraw(client);
            return;
        }

        /* path bar tap also goes up */
        if (client->touch_down_y < 130.0 && client->touch_down_y > 100.0 &&
                client->touch_down_x < (double)client->width - 130.0) {
            char *last_slash = strrchr(client->browse_path, '/');
            if (last_slash != NULL && last_slash != client->browse_path) {
                *last_slash = '\0';
            } else if (last_slash == client->browse_path) {
                client->browse_path[1] = '\0';
            }
            client->scroll_offset = 0;
            client->selected_row = -1;
            (void)lumo_app_client_redraw(client);
            return;
        }

        /* file/directory entry tap */
        int entry_idx = lumo_app_files_entry_at(client->width, client->height,
            client->touch_down_x, client->touch_down_y);
        if (entry_idx >= 0) {
            int adjusted = entry_idx + client->scroll_offset;
            DIR *dir = opendir(client->browse_path);
            if (dir != NULL) {
                struct dirent *entry;
                int visible = 0;
                while ((entry = readdir(dir)) != NULL) {
                    if (entry->d_name[0] == '.') continue;
                    if (strcmp(entry->d_name, "..") == 0) continue;
                    if (visible == adjusted) {
                        if (entry->d_type == DT_DIR) {
                            size_t plen = strlen(client->browse_path);
                            if (plen + 1 + strlen(entry->d_name) + 1 <
                                    sizeof(client->browse_path)) {
                                const char *sep = (plen > 1) ? "/" : "";
                                snprintf(client->browse_path + plen,
                                    sizeof(client->browse_path) - plen,
                                    "%s%s", sep, entry->d_name);
                                client->scroll_offset = 0;
                                client->selected_row = -1;
                                (void)lumo_app_client_redraw(client);
                            }
                        } else {
                            /* determine file type from extension */
                            char full_path[1100];
                            snprintf(full_path, sizeof(full_path),
                                "%s/%s", client->browse_path,
                                entry->d_name);
                            const char *ext = strrchr(entry->d_name, '.');
                            bool opened = false;

                            if (ext != NULL) {
                                /* text files → inline viewer */
                                if (strcasecmp(ext, ".txt") == 0 ||
                                        strcasecmp(ext, ".md") == 0 ||
                                        strcasecmp(ext, ".sh") == 0 ||
                                        strcasecmp(ext, ".c") == 0 ||
                                        strcasecmp(ext, ".h") == 0 ||
                                        strcasecmp(ext, ".py") == 0 ||
                                        strcasecmp(ext, ".conf") == 0 ||
                                        strcasecmp(ext, ".json") == 0 ||
                                        strcasecmp(ext, ".xml") == 0 ||
                                        strcasecmp(ext, ".log") == 0 ||
                                        strcasecmp(ext, ".csv") == 0 ||
                                        strcasecmp(ext, ".ini") == 0 ||
                                        strcasecmp(ext, ".yaml") == 0 ||
                                        strcasecmp(ext, ".yml") == 0 ||
                                        strcasecmp(ext, ".toml") == 0) {
                                    FILE *fp = fopen(full_path, "r");
                                    if (fp != NULL) {
                                        size_t n = fread(
                                            client->text_view_content,
                                            1, sizeof(client->text_view_content) - 1,
                                            fp);
                                        client->text_view_content[n] = '\0';
                                        fclose(fp);
                                        client->text_view_active = true;
                                        client->text_view_scroll = 0;
                                        snprintf(client->file_info_name,
                                            sizeof(client->file_info_name),
                                            "%s", entry->d_name);
                                        opened = true;
                                    }
                                }
                                /* image files → photos app */
                                else if (strcasecmp(ext, ".jpg") == 0 ||
                                        strcasecmp(ext, ".jpeg") == 0 ||
                                        strcasecmp(ext, ".png") == 0 ||
                                        strcasecmp(ext, ".bmp") == 0 ||
                                        strcasecmp(ext, ".gif") == 0) {
                                    char cmd[1200];
                                    snprintf(cmd, sizeof(cmd),
                                        "lumo-app:photos:%s", full_path);
                                    /* TODO: launch photos with path */
                                    opened = false; /* fall through to info */
                                }
                                /* audio files → music app via fork+exec
                                 * (no shell — prevents command injection
                                 * via crafted filenames) */
                                else if (strcasecmp(ext, ".mp3") == 0 ||
                                        strcasecmp(ext, ".wav") == 0 ||
                                        strcasecmp(ext, ".ogg") == 0 ||
                                        strcasecmp(ext, ".flac") == 0 ||
                                        strcasecmp(ext, ".m4a") == 0 ||
                                        strcasecmp(ext, ".aac") == 0) {
                                    pid_t pid = fork();
                                    if (pid == 0) {
                                        execlp("mpv", "mpv", "--no-video",
                                            full_path, (char *)NULL);
                                        _exit(1);
                                    }
                                    opened = true;
                                }
                                /* video files → video player */
                                else if (strcasecmp(ext, ".mp4") == 0 ||
                                        strcasecmp(ext, ".mkv") == 0 ||
                                        strcasecmp(ext, ".avi") == 0 ||
                                        strcasecmp(ext, ".mov") == 0 ||
                                        strcasecmp(ext, ".webm") == 0) {
                                    pid_t pid = fork();
                                    if (pid == 0) {
                                        execlp("mpv", "mpv",
                                            full_path, (char *)NULL);
                                        _exit(1);
                                    }
                                    opened = true;
                                }
                                /* PDF files → native PDF reader */
                                else if (strcasecmp(ext, ".pdf") == 0) {
                                    pid_t pid = fork();
                                    if (pid == 0) {
                                        setenv("LUMO_PDF_FILE", full_path, 1);
                                        execlp("lumo-app", "lumo-app",
                                            "--app", "pdf", (char *)NULL);
                                        _exit(1);
                                    }
                                    opened = true;
                                }
                            }

                            if (!opened) {
                                /* show file info overlay for unknown types */
                                client->selected_row = adjusted;
                                client->file_info_visible = true;
                                snprintf(client->file_info_name,
                                    sizeof(client->file_info_name),
                                    "%s", entry->d_name);
                                snprintf(client->file_info_path,
                                    sizeof(client->file_info_path),
                                    "%s/%s", client->browse_path,
                                    entry->d_name);
                            }
                            (void)lumo_app_client_redraw(client);
                        }
                        break;
                    }
                    visible++;
                }
                closedir(dir);
            }
        }
    }

    if (client->app_id == LUMO_APP_NOTES && client->width > 0 &&
            client->height > 0) {
        int row = lumo_app_notes_row_at(client->width, client->height,
            client->touch_down_x, client->touch_down_y);

        /* editor mode: done button or delete-in-editor */
        if (client->note_editing >= 0) {
            if (row == -4) {
                /* done — exit editor */
                client->note_editing = -1;
                lumo_app_notes_save(client);
                (void)lumo_app_client_redraw(client);
                lumo_app_sync_text_input_state(client, false);
                return;
            } else if (row == -5) {
                /* delete in editor */
                int del = client->note_editing;
                client->note_editing = -1;
                for (int i = del; i < client->note_count - 1; i++) {
                    memcpy(client->notes[i], client->notes[i + 1],
                        sizeof(client->notes[0]));
                }
                client->note_count--;
                client->notes[client->note_count][0] = '\0';
                client->selected_row = -1;
                lumo_app_notes_save(client);
                (void)lumo_app_client_redraw(client);
                lumo_app_sync_text_input_state(client, false);
                return;
            }
            /* any other tap in editor: ignore (let OSK handle input) */
            return;
        }

        if (row >= 0 && row < client->note_count) {
            if (client->selected_row == row) {
                /* tap on selected = start editing (open editor) */
                client->note_editing = row;
            } else {
                /* tap on unselected = select it */
                client->selected_row = row;
                client->note_editing = -1;
            }
            (void)lumo_app_client_redraw(client);
        } else if (row == -3) {
            /* delete selected note (list view) */
            if (client->selected_row >= 0 &&
                    client->selected_row < client->note_count) {
                int del = client->selected_row;
                for (int i = del; i < client->note_count - 1; i++) {
                    memcpy(client->notes[i], client->notes[i + 1],
                        sizeof(client->notes[0]));
                }
                client->note_count--;
                client->notes[client->note_count][0] = '\0';
                client->selected_row = -1;
                client->note_editing = -1;
                lumo_app_notes_save(client);
                (void)lumo_app_client_redraw(client);
            }
        } else if (row == -2) {
            if (client->note_count < 8) {
                client->notes[client->note_count][0] = '\0';
                client->note_count++;
                /* auto-select and edit the new note */
                client->selected_row = client->note_count - 1;
                client->note_editing = client->note_count - 1;
                lumo_app_notes_save(client);
                (void)lumo_app_client_redraw(client);
            }
        }
        lumo_app_sync_text_input_state(client, client->note_editing >= 0);
    }

    /* Phone app touch handling */
    if (client->app_id == LUMO_APP_PHONE && client->width > 0 &&
            client->height > 0) {
        int btn = lumo_app_phone_button_at(client->width, client->height,
            client->touch_down_x, client->touch_down_y, client->clock_tab);

        if (btn == 15) {
            /* dialer tab */
            client->clock_tab = 0;
            (void)lumo_app_client_redraw(client);
        } else if (btn == 14) {
            /* contacts tab */
            client->clock_tab = 1;
            (void)lumo_app_client_redraw(client);
        } else if (btn == 16) {
            /* log tab */
            client->clock_tab = 2;
            (void)lumo_app_client_redraw(client);
        } else if (client->clock_tab == 0) {
            /* dialer mode */
            if (btn >= 0 && btn <= 9) {
                /* digit */
                if (client->term_input_len < 20) {
                    client->term_input[client->term_input_len++] =
                        (char)('0' + btn);
                    client->term_input[client->term_input_len] = '\0';
                    (void)lumo_app_client_redraw(client);
                }
            } else if (btn == 10) {
                /* * */
                if (client->term_input_len < 20) {
                    client->term_input[client->term_input_len++] = '*';
                    client->term_input[client->term_input_len] = '\0';
                    (void)lumo_app_client_redraw(client);
                }
            } else if (btn == 11) {
                /* # */
                if (client->term_input_len < 20) {
                    client->term_input[client->term_input_len++] = '#';
                    client->term_input[client->term_input_len] = '\0';
                    (void)lumo_app_client_redraw(client);
                }
            } else if (btn == 13) {
                /* backspace */
                if (client->term_input_len > 0) {
                    client->term_input[--client->term_input_len] = '\0';
                    (void)lumo_app_client_redraw(client);
                }
            } else if (btn == 12) {
                /* call button — save number as contact if non-empty */
                if (client->term_input_len > 0 && client->note_count < 8) {
                    snprintf(client->notes[client->note_count],
                        sizeof(client->notes[0]), "%s", client->term_input);
                    client->note_count++;
                    lumo_app_contacts_save(client);
                }
                client->term_input_len = 0;
                client->term_input[0] = '\0';
                (void)lumo_app_client_redraw(client);
            }
        } else if (client->clock_tab == 1) {
            /* contacts tab */
            if (btn >= 100) {
                int row = btn - 100;
                if (row < client->note_count) {
                    client->selected_row = row;
                    (void)lumo_app_client_redraw(client);
                }
            }
        }
    }

    /* Camera app touch handling */
    if (client->app_id == LUMO_APP_CAMERA && client->width > 0 &&
            client->height > 0) {
        int btn = lumo_app_camera_button_at(client->width, client->height,
            client->touch_down_x, client->touch_down_y,
            client->photo_viewing);

        if (btn == 1) {
            /* toggle gallery mode */
            client->photo_viewing = !client->photo_viewing;
            if (client->photo_viewing) {
                /* scan Pictures directory for captured photos */
                static const char *img_exts[] = {".jpg", ".jpeg", ".png",
                    ".bmp"};
                lumo_app_scan_media(client, "Pictures",
                    img_exts, sizeof(img_exts) / sizeof(img_exts[0]));
            }
            (void)lumo_app_client_redraw(client);
        } else if (btn == 0 && !client->photo_viewing) {
            /* capture button — placeholder, increment count */
            (void)lumo_app_client_redraw(client);
        }
    }

    /* Maps app touch handling */
    if (client->app_id == LUMO_APP_MAPS && client->width > 0 &&
            client->height > 0) {
        int btn = lumo_app_maps_button_at(client->width, client->height,
            client->touch_down_x, client->touch_down_y, client->clock_tab);

        if (btn == 1) {
            client->clock_tab = 0; /* compass */
            client->note_editing = -1;
            (void)lumo_app_client_redraw(client);
        } else if (btn == 2) {
            client->clock_tab = 1; /* places */
            client->note_editing = -1;
            (void)lumo_app_client_redraw(client);
        } else if (btn == 3) {
            client->clock_tab = 2; /* info */
            client->note_editing = -1;
            (void)lumo_app_client_redraw(client);
        } else if (btn == 0 && client->clock_tab == 1) {
            /* add place — create and immediately start editing */
            if (client->note_count < 8) {
                client->notes[client->note_count][0] = '\0';
                client->note_count++;
                client->selected_row = client->note_count - 1;
                client->note_editing = client->note_count - 1;
                lumo_app_places_save(client);
                (void)lumo_app_client_redraw(client);
            }
        } else if (btn >= 100 && client->clock_tab == 1) {
            int row = btn - 100;
            if (row >= 0 && row < client->note_count) {
                if (client->note_editing == row) {
                    /* tap editing place = stop editing */
                    client->note_editing = -1;
                } else if (client->selected_row == row) {
                    /* tap selected = start editing */
                    client->note_editing = row;
                } else {
                    /* tap unselected = select, stop editing */
                    client->selected_row = row;
                    client->note_editing = -1;
                }
                (void)lumo_app_client_redraw(client);
            }
        }
        lumo_app_sync_text_input_state(client, client->note_editing >= 0);
    }

    /* Browser app touch handling */
    if (client->app_id == LUMO_APP_BROWSER && client->width > 0 &&
            client->height > 0) {
        int btn = lumo_app_browser_button_at(client->width, client->height,
            client->touch_down_x, client->touch_down_y);
        if (btn == 0) {
            /* URL bar tap — toggle editing */
            if (client->note_editing >= 0) {
                client->note_editing = -1;
            } else {
                client->note_editing = 0;
            }
            (void)lumo_app_client_redraw(client);
        } else if (btn == 3 && client->term_input_len > 0) {
            /* GO button — launch URL */
            lumo_app_browser_launch_url(client->term_input);
            client->note_editing = -1;
            (void)lumo_app_client_redraw(client);
        } else if (btn == 4) {
            /* bookmark — save current URL */
            if (client->term_input_len > 0 && client->note_count < 8) {
                snprintf(client->notes[client->note_count],
                    sizeof(client->notes[0]), "%s", client->term_input);
                client->note_count++;
                lumo_app_bookmarks_save(client);
                (void)lumo_app_client_redraw(client);
            }
        } else if (btn == 5) {
            /* home — clear URL, show start page */
            client->term_input_len = 0;
            client->term_input[0] = '\0';
            client->note_editing = -1;
            (void)lumo_app_client_redraw(client);
        } else if (btn >= 100) {
            int row = btn - 100;
            if (client->note_editing >= 0) {
                /* in editing mode: tap quick link → fill URL bar
                 * (user presses Enter/GO to actually navigate) */
                static const char *quick_urls[] = {
                    "https://duckduckgo.com/",
                    "https://en.m.wikipedia.org/",
                    "https://github.com/",
                    "https://m.youtube.com/",
                };
                if (row < 4) {
                    snprintf(client->term_input, sizeof(client->term_input),
                        "%s", quick_urls[row]);
                    client->term_input_len = (int)strlen(client->term_input);
                    (void)lumo_app_client_redraw(client);
                }
            } else if (row < client->note_count) {
                /* home mode: tap bookmark → fill URL bar and enter
                 * editing mode (user presses Enter/GO to navigate) */
                snprintf(client->term_input, sizeof(client->term_input),
                    "%s", client->notes[row]);
                client->term_input_len = (int)strlen(client->term_input);
                client->note_editing = 0;
                (void)lumo_app_client_redraw(client);
            }
        }
        lumo_app_sync_text_input_state(client, client->note_editing >= 0);
    }

    if (client->app_id == LUMO_APP_SETTINGS && client->width > 0 &&
            client->height > 0) {
        if (client->selected_row >= 0 &&
                client->touch_down_y >= 34.0 && client->touch_down_y < 60.0 &&
                client->touch_down_x < 200.0) {
            /* back button — y starts at 38 to avoid 32px top edge zone */
            client->selected_row = -1;
            (void)lumo_app_client_redraw(client);
        } else if (client->selected_row >= 0) {
            /* on a subpage — check for toggle taps */
            int toggle = lumo_app_settings_toggle_at(
                client->width, client->height,
                client->touch_down_x, client->touch_down_y,
                client->selected_row);
            if (toggle >= 0) {
                switch (toggle) {
                case 0: client->settings.wifi_enabled =
                    !client->settings.wifi_enabled; break;
                case 1: client->settings.auto_rotate =
                    !client->settings.auto_rotate; break;
                case 2: client->settings.auto_updates =
                    !client->settings.auto_updates; break;
                case 3: client->settings.debug_mode =
                    !client->settings.debug_mode; break;
                case 4: client->settings.persist_logs =
                    !client->settings.persist_logs; break;
                }
                lumo_app_settings_save(client);
                (void)lumo_app_client_redraw(client);
            }
            /* check for action buttons and sliders */
            int action = lumo_app_settings_action_at(
                client->width, client->height,
                client->touch_down_x, client->touch_down_y,
                client->selected_row);
            if (action == 100) {
                /* cycle rotation: read current, write next */
                const char *home = getenv("HOME");
                if (home != NULL) {
                    char path[256], cur[16] = "0";
                    snprintf(path, sizeof(path), "%s/.lumo-rotation", home);
                    FILE *fp = fopen(path, "r");
                    if (fp) {
                        if (fgets(cur, sizeof(cur), fp)) {
                            char *nl = strchr(cur, '\n');
                            if (nl) *nl = '\0';
                        }
                        fclose(fp);
                    }
                    const char *next = "90";
                    if (strcmp(cur, "normal") == 0 || strcmp(cur, "0") == 0)
                        next = "90";
                    else if (strcmp(cur, "90") == 0) next = "180";
                    else if (strcmp(cur, "180") == 0) next = "270";
                    else next = "normal";
                    fp = fopen(path, "w");
                    if (fp) { fprintf(fp, "%s\n", next); fclose(fp); }
                    fprintf(stderr,
                        "lumo-app: rotation set to %s (reload to apply)\n",
                        next);
                }
                (void)lumo_app_client_redraw(client);
            } else if (action == 101) {
                /* volume slider drag */
                int slider_x = 24 + 8;
                int slider_w = (int)client->width - 24 * 2 - 16;
                int pct = (int)((client->touch_down_x - slider_x) * 100.0
                    / slider_w);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                char cmd[128];
                snprintf(cmd, sizeof(cmd),
                    "pactl set-sink-volume @DEFAULT_SINK@ %d%%", pct);
                pid_t pid = fork();
                if (pid == 0) {
                    close(STDERR_FILENO);
                    execlp("sh", "sh", "-c", cmd, (char *)NULL);
                    _exit(127);
                }
                if (pid > 0) waitpid(pid, NULL, 0);
                (void)lumo_app_client_redraw(client);
            } else if (action == 102) {
                /* brightness slider drag */
                int slider_x = 24 + 8;
                int slider_w = (int)client->width - 24 * 2 - 16;
                int pct = (int)((client->touch_down_x - slider_x) * 100.0
                    / slider_w);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                char val[16];
                snprintf(val, sizeof(val), "%d",
                    pct * 255 / 100); /* assume max=255 */
                /* try common backlight sysfs paths */
                FILE *fp = fopen(
                    "/sys/class/backlight/backlight/brightness", "w");
                if (!fp) fp = fopen(
                    "/sys/class/backlight/0/brightness", "w");
                if (fp) { fprintf(fp, "%s\n", val); fclose(fp); }
                (void)lumo_app_client_redraw(client);
            }

            /* WiFi network tap on network subpage (row 0) */
            if (client->selected_row == 0 && toggle < 0 && action < 0) {
                /* network list starts at y ~ 240 (after header+info+toggle+label) */
                int net_y = (int)client->touch_down_y;
                int list_start = 240;  /* approx y where network rows begin */
                if (net_y >= list_start) {
                    int net_idx = (net_y - list_start) / 44;
                    extern void lumo_settings_wifi_connect(
                        int network_index, const char *password);
                    if (net_idx >= 0 && net_idx < 16) {
                        lumo_settings_wifi_connect(net_idx, NULL);
                        (void)lumo_app_client_redraw(client);
                    }
                }
            }
        } else {
            int row = lumo_app_settings_row_at(client->width, client->height,
                client->touch_down_x, client->touch_down_y);
            if (row >= 0) {
                client->selected_row = row;
                (void)lumo_app_client_redraw(client);
            }
        }
    }

    /* PDF reader touch handling */
    if (client->app_id == LUMO_APP_PDF && client->width > 0 &&
            client->height > 0) {
        if (client->scroll_active) {
            /* scroll finished — apply vertical scroll */
            double dy = client->touch_down_y -
                wl_fixed_to_double(wl_fixed_from_double(client->pointer_y));
            lumo_app_pdf_scroll(dy > 0 ? 1 : -1);
            client->scroll_active = false;
            (void)lumo_app_client_redraw(client);
            return;
        }
        int btn = lumo_app_pdf_button_at(client->width, client->height,
            client->touch_down_x, client->touch_down_y);
        if (btn >= 0) {
            lumo_app_pdf_handle_tap(btn);
            (void)lumo_app_client_redraw(client);
        }
    }

    /* Setup wizard touch handling */
    if (client->app_id == LUMO_APP_SETUP) {
        lumo_setup_handle_tap(client->touch_down_x, client->touch_down_y);
        if (lumo_setup_is_complete()) {
            client->running = false;
        }
        (void)lumo_app_client_redraw(client);
    }
}

static void lumo_app_touch_handle_motion(
    void *data,
    struct wl_touch *wl_touch,
    uint32_t time,
    int32_t id,
    wl_fixed_t x,
    wl_fixed_t y
) {
    struct lumo_app_client *client = data;

    (void)wl_touch;
    (void)time;
    if (client == NULL || !client->touch_pressed ||
            client->active_touch_id != id) {
        return;
    }

    {
        double cur_y = wl_fixed_to_double(y);
        double cur_x = wl_fixed_to_double(x);
        lumo_app_client_set_close_active(client,
            lumo_app_client_close_contains(client, cur_x, cur_y));

        /* GitHub content view scrolls internally */
        if (client->app_id == LUMO_APP_GITHUB) {
            extern void lumo_app_github_scroll(int direction);
            double dy = client->touch_down_y - cur_y;
            if (dy > 30.0) {
                lumo_app_github_scroll(1);
                client->scroll_active = true;
                client->touch_down_y = cur_y;
                (void)lumo_app_client_redraw(client);
            } else if (dy < -30.0) {
                lumo_app_github_scroll(-1);
                client->scroll_active = true;
                client->touch_down_y = cur_y;
                (void)lumo_app_client_redraw(client);
            }
        }

        if (client->app_id == LUMO_APP_FILES ||
                (client->app_id == LUMO_APP_PHOTOS && !client->photo_viewing)) {
            double dy = client->touch_down_y - cur_y;
            if (dy > 30.0) {
                client->scroll_offset++;
                client->scroll_active = true;
                client->touch_down_y = cur_y;
                (void)lumo_app_client_redraw(client);
            } else if (dy < -30.0 && client->scroll_offset > 0) {
                client->scroll_offset--;
                client->scroll_active = true;
                client->touch_down_y = cur_y;
                (void)lumo_app_client_redraw(client);
            }
        }
    }
}

static void lumo_app_touch_handle_frame(
    void *data,
    struct wl_touch *wl_touch
) {
    (void)data;
    (void)wl_touch;
}

static void lumo_app_touch_handle_cancel(
    void *data,
    struct wl_touch *wl_touch
) {
    struct lumo_app_client *client = data;

    (void)wl_touch;
    if (client == NULL) {
        return;
    }

    client->touch_pressed = false;
    client->active_touch_id = -1;
    lumo_app_client_set_close_active(client, false);
}

static void lumo_app_touch_handle_shape(
    void *data,
    struct wl_touch *wl_touch,
    int32_t id,
    wl_fixed_t major,
    wl_fixed_t minor
) {
    (void)data;
    (void)wl_touch;
    (void)id;
    (void)major;
    (void)minor;
}

static void lumo_app_touch_handle_orientation(
    void *data,
    struct wl_touch *wl_touch,
    int32_t id,
    wl_fixed_t orientation
) {
    (void)data;
    (void)wl_touch;
    (void)id;
    (void)orientation;
}

static const struct wl_touch_listener lumo_app_touch_listener = {
    .down = lumo_app_touch_handle_down,
    .up = lumo_app_touch_handle_up,
    .motion = lumo_app_touch_handle_motion,
    .frame = lumo_app_touch_handle_frame,
    .cancel = lumo_app_touch_handle_cancel,
    .shape = lumo_app_touch_handle_shape,
    .orientation = lumo_app_touch_handle_orientation,
};

/* --- text-input-v3 listener (receives OSK committed text) --- */

static uint32_t lumo_app_text_input_purpose(enum lumo_app_id app_id) {
    return app_id == LUMO_APP_MESSAGES
        ? ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL
        : ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
}

static void lumo_app_sync_text_input_state(
    struct lumo_app_client *client,
    bool flush_pending
) {
    bool wants_osk;

    if (client == NULL || client->text_input == NULL) {
        return;
    }

    wants_osk = lumo_app_wants_osk(client->app_id, client->note_editing);
    if (wants_osk == client->text_input_enabled) {
        return;
    }

    if (wants_osk) {
        if (flush_pending && client->display != NULL) {
            wl_display_dispatch_pending(client->display);
        }
        zwp_text_input_v3_enable(client->text_input);
        zwp_text_input_v3_set_content_type(client->text_input,
            ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
            lumo_app_text_input_purpose(client->app_id));
        zwp_text_input_v3_commit(client->text_input);
        client->text_input_enabled = true;
        fprintf(stderr, "lumo-app: text-input enabled for %s\n",
            lumo_app_id_name(client->app_id));
    } else {
        zwp_text_input_v3_disable(client->text_input);
        zwp_text_input_v3_commit(client->text_input);
        client->text_input_enabled = false;
        fprintf(stderr, "lumo-app: text-input disabled for %s\n",
            lumo_app_id_name(client->app_id));
    }

    if (client->display != NULL) {
        wl_display_flush(client->display);
    }
}

static void lumo_app_text_input_enter(void *data,
    struct zwp_text_input_v3 *ti, struct wl_surface *surface)
{
    struct lumo_app_client *client = data;
    (void)ti; (void)surface;
    if (client == NULL) return;

    lumo_app_sync_text_input_state(client, false);
}

static void lumo_app_text_input_leave(void *data,
    struct zwp_text_input_v3 *ti, struct wl_surface *surface)
{
    struct lumo_app_client *client = data;
    (void)ti; (void)surface;
    if (client == NULL) return;
    if (client->text_input_enabled && client->text_input != NULL) {
        zwp_text_input_v3_disable(client->text_input);
        zwp_text_input_v3_commit(client->text_input);
        client->text_input_enabled = false;
        fprintf(stderr, "lumo-app: text-input disabled\n");
    }
}

static void lumo_app_text_input_preedit(void *data,
    struct zwp_text_input_v3 *ti, const char *text,
    int32_t cursor_begin, int32_t cursor_end)
{
    (void)data; (void)ti; (void)text;
    (void)cursor_begin; (void)cursor_end;
}

static void lumo_app_text_input_commit_string(void *data,
    struct zwp_text_input_v3 *ti, const char *text)
{
    struct lumo_app_client *client = data;
    (void)ti;
    if (client == NULL || text == NULL) return;
    /* buffer the committed text until done event */
    size_t len = strlen(text);
    if (client->pending_commit_len + (int)len <
            (int)sizeof(client->pending_commit) - 1) {
        memcpy(client->pending_commit + client->pending_commit_len, text, len);
        client->pending_commit_len += (int)len;
        client->pending_commit[client->pending_commit_len] = '\0';
    }
}

static void lumo_app_text_input_delete_surrounding(void *data,
    struct zwp_text_input_v3 *ti, uint32_t before, uint32_t after)
{
    struct lumo_app_client *client = data;
    (void)ti; (void)after;
    if (client == NULL) return;
    /* handle backspace from OSK */
    if (before > 0) {
        if (client->app_id == LUMO_APP_MESSAGES && client->pty_fd >= 0) {
            for (uint32_t i = 0; i < before; i++) {
                lumo_app_term_write(client, "\x7f", 1);
            }
        } else if (client->app_id == LUMO_APP_BROWSER &&
                client->note_editing >= 0) {
            for (uint32_t i = 0; i < before && client->term_input_len > 0;
                    i++) {
                client->term_input[--client->term_input_len] = '\0';
            }
            (void)lumo_app_client_redraw(client);
        } else if ((client->app_id == LUMO_APP_NOTES ||
                client->app_id == LUMO_APP_MAPS) &&
                client->note_editing >= 0 &&
                client->note_editing < client->note_count) {
            size_t len = strlen(client->notes[client->note_editing]);
            for (uint32_t i = 0; i < before && len > 0; i++) {
                client->notes[client->note_editing][--len] = '\0';
            }
            if (client->app_id == LUMO_APP_MAPS)
                lumo_app_places_save(client);
            else
                lumo_app_notes_save(client);
            (void)lumo_app_client_redraw(client);
        }
    }
}

static void lumo_app_text_input_done(void *data,
    struct zwp_text_input_v3 *ti, uint32_t serial)
{
    struct lumo_app_client *client = data;
    (void)ti; (void)serial;
    if (client == NULL) return;

    if (client->pending_commit_len > 0) {
        if (client->app_id == LUMO_APP_MESSAGES && client->pty_fd >= 0) {
            /* forward OSK text to PTY */
            for (int i = 0; i < client->pending_commit_len; i++) {
                char ch = client->pending_commit[i];
                if (ch == '\n') {
                    lumo_app_term_write(client, "\n", 1);
                } else {
                    lumo_app_term_write(client, &ch, 1);
                }
            }
        } else if (client->app_id == LUMO_APP_BROWSER &&
                client->note_editing >= 0) {
            /* OSK text for browser URL bar */
            for (int i = 0; i < client->pending_commit_len; i++) {
                char ch = client->pending_commit[i];
                if (ch == '\n') {
                    /* enter = go */
                    if (client->term_input_len > 0) {
                        lumo_app_browser_launch_url(client->term_input);
                        client->note_editing = -1;
                        lumo_app_sync_text_input_state(client, false);
                    }
                    break;
                }
                if (client->term_input_len < (int)sizeof(client->term_input) - 1) {
                    client->term_input[client->term_input_len++] = ch;
                    client->term_input[client->term_input_len] = '\0';
                }
            }
            (void)lumo_app_client_redraw(client);
        } else if (client->app_id == LUMO_APP_SETUP) {
            /* forward OSK text to setup wizard */
            for (int i = 0; i < client->pending_commit_len; i++) {
                lumo_setup_handle_key(client->pending_commit[i]);
            }
            (void)lumo_app_client_redraw(client);
        } else if (client->app_id == LUMO_APP_NOTES ||
                client->app_id == LUMO_APP_MAPS) {
            /* OSK text for notes/maps app */
            if (client->note_editing >= 0 &&
                    client->note_editing < client->note_count) {
                size_t cur = strlen(client->notes[client->note_editing]);
                size_t add = (size_t)client->pending_commit_len;
                if (cur + add < sizeof(client->notes[0]) - 1) {
                    memcpy(client->notes[client->note_editing] + cur,
                        client->pending_commit, add);
                    client->notes[client->note_editing][cur + add] = '\0';
                    if (client->app_id == LUMO_APP_MAPS)
                        lumo_app_places_save(client);
                    else
                        lumo_app_notes_save(client);
                    (void)lumo_app_client_redraw(client);
                }
            }
        }
        client->pending_commit_len = 0;
        client->pending_commit[0] = '\0';
    }
}

static const struct zwp_text_input_v3_listener lumo_app_text_input_listener = {
    .enter = lumo_app_text_input_enter,
    .leave = lumo_app_text_input_leave,
    .preedit_string = lumo_app_text_input_preedit,
    .commit_string = lumo_app_text_input_commit_string,
    .delete_surrounding_text = lumo_app_text_input_delete_surrounding,
    .done = lumo_app_text_input_done,
};

/* --- keyboard handler (physical keys forwarded to PTY) --- */

static void lumo_app_keyboard_key(
    void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t time, uint32_t key, uint32_t state
) {
    struct lumo_app_client *client = data;
    (void)kb; (void)serial; (void)time;

    if (client == NULL) return;

    /* pinch-to-zoom: handle KEY_ZOOMIN / KEY_ZOOMOUT from compositor */
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
            (key == 0x1a2 || key == 0x1a3)) {
        if (key == 0x1a2) { /* KEY_ZOOMIN */
            client->zoom_scale *= 1.1;
            if (client->zoom_scale > 4.0) client->zoom_scale = 4.0;
        } else { /* KEY_ZOOMOUT */
            client->zoom_scale /= 1.1;
            if (client->zoom_scale < 0.5) client->zoom_scale = 0.5;
        }
        (void)lumo_app_client_redraw(client);
        return;
    }

    /* browser: forward keys to URL bar when editing */
    if (client->app_id == LUMO_APP_BROWSER && client->note_editing >= 0) {
        if (key == 42 || key == 54) {
            client->shift_held = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
            return;
        }
        if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
        if (key == 14) { /* backspace */
            if (client->term_input_len > 0) {
                client->term_input[--client->term_input_len] = '\0';
                (void)lumo_app_client_redraw(client);
            }
        } else if (key == 28) { /* enter → launch URL */
            if (client->term_input_len > 0) {
                lumo_app_browser_launch_url(client->term_input);
                client->note_editing = -1;
                lumo_app_sync_text_input_state(client, false);
                (void)lumo_app_client_redraw(client);
            }
        } else if (key >= 2 && key <= 53) {
            static const char keymap[] =
                "1234567890-="
                "\0\0qwertyuiop[]\0"
                "\0asdfghjkl;'\0\0"
                "\0zxcvbnm,./";
            int ki = (int)key - 2;
            char ch = (ki >= 0 && ki < (int)sizeof(keymap) - 1)
                ? keymap[ki] : '\0';
            if (ch != '\0' && client->shift_held && ch >= 'a' && ch <= 'z')
                ch -= 32;
            if (ch != '\0' &&
                    client->term_input_len < (int)sizeof(client->term_input) - 1) {
                client->term_input[client->term_input_len++] = ch;
                client->term_input[client->term_input_len] = '\0';
                (void)lumo_app_client_redraw(client);
            }
        } else if (key == 57) { /* space */
            if (client->term_input_len < (int)sizeof(client->term_input) - 1) {
                client->term_input[client->term_input_len++] = ' ';
                client->term_input[client->term_input_len] = '\0';
                (void)lumo_app_client_redraw(client);
            }
        }
        return;
    }

    if (client->app_id != LUMO_APP_MESSAGES) return;

    /* track shift key state */
    if (key == 42 || key == 54) { /* KEY_LEFTSHIFT / KEY_RIGHTSHIFT */
        client->shift_held = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
        return;
    }

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    /* PTY mode: forward keystrokes to the shell */
    if (client->pty_fd >= 0) {
        const char *prefix = client->term_state.app_cursor_keys
            ? "\x1bO" : "\x1b[";
        if (key == 14) {
            lumo_app_term_write(client, "\x7f", 1); /* backspace */
        } else if (key == 28) {
            lumo_app_term_write(client, "\r", 1); /* enter (CR) */
        } else if (key == 57) {
            lumo_app_term_write(client, " ", 1); /* space */
        } else if (key == 1) {
            lumo_app_term_write(client, "\x1b", 1); /* escape */
        } else if (key == 15) {
            lumo_app_term_write(client, "\t", 1); /* tab */
        } else if (key == 103) { /* up arrow */
            char seq[4]; seq[0] = prefix[0]; seq[1] = prefix[1];
            seq[2] = 'A'; lumo_app_term_write(client, seq, 3);
        } else if (key == 108) { /* down arrow */
            char seq[4]; seq[0] = prefix[0]; seq[1] = prefix[1];
            seq[2] = 'B'; lumo_app_term_write(client, seq, 3);
        } else if (key == 106) { /* right arrow */
            char seq[4]; seq[0] = prefix[0]; seq[1] = prefix[1];
            seq[2] = 'C'; lumo_app_term_write(client, seq, 3);
        } else if (key == 105) { /* left arrow */
            char seq[4]; seq[0] = prefix[0]; seq[1] = prefix[1];
            seq[2] = 'D'; lumo_app_term_write(client, seq, 3);
        } else if (key == 102) { /* home */
            lumo_app_term_write(client, "\x1b[H", 3);
        } else if (key == 107) { /* end */
            lumo_app_term_write(client, "\x1b[F", 3);
        } else if (key == 104) { /* page up */
            lumo_app_term_write(client, "\x1b[5~", 4);
        } else if (key == 109) { /* page down */
            lumo_app_term_write(client, "\x1b[6~", 4);
        } else if (key == 111) { /* delete */
            lumo_app_term_write(client, "\x1b[3~", 4);
        } else if (key >= 2 && key <= 53) {
            static const char keymap[] =
                "1234567890-="
                "\0\0qwertyuiop[]\0"
                "\0asdfghjkl;'\0\0"
                "\0zxcvbnm,./";
            static const char shiftmap[] =
                "!@#$%^&*()_+"
                "\0\0QWERTYUIOP{}\0"
                "\0ASDFGHJKL:\"\0\0"
                "\0ZXCVBNM<>?";
            int idx = (int)key - 2;
            const char *km = client->shift_held ? shiftmap : keymap;
            if (idx >= 0 && idx < (int)sizeof(keymap) - 1 &&
                    km[idx] != '\0') {
                char ch = km[idx];
                /* ctrl modifier: Ctrl+A=1, Ctrl+C=3, etc. */
                if (client->shift_held && ch >= 'A' && ch <= 'Z') {
                    /* shift is already handled above */
                } else if (!client->shift_held && ch >= 'a' &&
                        ch <= 'z') {
                    /* check for ctrl (key 29/97 held) — we reuse
                     * shift_held as ctrl for now via OSK ctrl toggle */
                }
                lumo_app_term_write(client, &ch, 1);
            }
        }
        return;
    }

    /* fallback: no PTY — should not happen, terminal always has PTY */
    (void)key;
}

static void lumo_app_keyboard_keymap(void *d, struct wl_keyboard *k,
    uint32_t fmt, int32_t fd, uint32_t sz) {
    (void)d; (void)k; (void)fmt; close(fd); (void)sz;
}
static void lumo_app_keyboard_enter(void *d, struct wl_keyboard *k,
    uint32_t s, struct wl_surface *sf, struct wl_array *keys) {
    (void)d; (void)k; (void)s; (void)sf; (void)keys;
}
static void lumo_app_keyboard_leave(void *d, struct wl_keyboard *k,
    uint32_t s, struct wl_surface *sf) {
    (void)d; (void)k; (void)s; (void)sf;
}
static void lumo_app_keyboard_modifiers(void *d, struct wl_keyboard *k,
    uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t g) {
    (void)d; (void)k; (void)s; (void)dep; (void)lat; (void)lock; (void)g;
}
static void lumo_app_keyboard_repeat(void *d, struct wl_keyboard *k,
    int32_t rate, int32_t delay) {
    (void)d; (void)k; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener lumo_app_keyboard_listener = {
    .keymap = lumo_app_keyboard_keymap,
    .enter = lumo_app_keyboard_enter,
    .leave = lumo_app_keyboard_leave,
    .key = lumo_app_keyboard_key,
    .modifiers = lumo_app_keyboard_modifiers,
    .repeat_info = lumo_app_keyboard_repeat,
};

static void lumo_app_seat_handle_capabilities(
    void *data,
    struct wl_seat *seat,
    uint32_t capabilities
) {
    struct lumo_app_client *client = data;

    if (client == NULL) {
        return;
    }

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
        if (client->pointer == NULL) {
            client->pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(client->pointer, &lumo_app_pointer_listener,
                client);
        }
    } else if (client->pointer != NULL) {
        wl_pointer_release(client->pointer);
        client->pointer = NULL;
        client->pointer_pressed = false;
        client->pointer_position_valid = false;
        if (!client->touch_pressed) {
            lumo_app_client_set_close_active(client, false);
        }
    }

    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0) {
        if (client->keyboard == NULL) {
            client->keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(client->keyboard,
                &lumo_app_keyboard_listener, client);
        }
    } else if (client->keyboard != NULL) {
        wl_keyboard_release(client->keyboard);
        client->keyboard = NULL;
    }

    if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) != 0) {
        if (client->touch == NULL) {
            client->touch = wl_seat_get_touch(seat);
            wl_touch_add_listener(client->touch, &lumo_app_touch_listener,
                client);
        }
    } else if (client->touch != NULL) {
        wl_touch_release(client->touch);
        client->touch = NULL;
        client->touch_pressed = false;
        client->active_touch_id = -1;
        if (!client->pointer_pressed) {
            lumo_app_client_set_close_active(client, false);
        }
    }
}

static void lumo_app_seat_handle_name(
    void *data,
    struct wl_seat *seat,
    const char *name
) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener lumo_app_seat_listener = {
    .capabilities = lumo_app_seat_handle_capabilities,
    .name = lumo_app_seat_handle_name,
};

static void lumo_app_registry_add(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version
) {
    struct lumo_app_client *client = data;

    if (client == NULL) {
        return;
    }

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        client->compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, version < 4 ? version : 4);
        return;
    }

    if (strcmp(interface, wl_shm_interface.name) == 0) {
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        return;
    }

    if (strcmp(interface, wl_seat_interface.name) == 0) {
        client->seat = wl_registry_bind(registry, name, &wl_seat_interface,
            version < 5 ? version : 5);
        if (client->seat != NULL) {
            wl_seat_add_listener(client->seat, &lumo_app_seat_listener, client);
        }
        return;
    }

    if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        client->wm_base = wl_registry_bind(registry, name,
            &xdg_wm_base_interface, version < 4 ? version : 4);
        if (client->wm_base != NULL) {
            xdg_wm_base_add_listener(client->wm_base,
                &lumo_app_wm_base_listener, client);
        }
        return;
    }

    if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        client->text_input_manager = wl_registry_bind(registry, name,
            &zwp_text_input_manager_v3_interface, 1);
    }
}

static void lumo_app_registry_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name
) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener lumo_app_registry_listener = {
    .global = lumo_app_registry_add,
    .global_remove = lumo_app_registry_remove,
};

static bool lumo_app_client_create_surface(struct lumo_app_client *client) {
    char title[96];
    char app_id[64];
    const char *app_name;

    if (client == NULL || client->compositor == NULL || client->wm_base == NULL) {
        return false;
    }

    app_name = lumo_app_id_name(client->app_id);
    client->surface = wl_compositor_create_surface(client->compositor);
    if (client->surface == NULL) {
        return false;
    }

    client->xdg_surface = xdg_wm_base_get_xdg_surface(client->wm_base,
        client->surface);
    if (client->xdg_surface == NULL) {
        return false;
    }

    client->xdg_toplevel = xdg_surface_get_toplevel(client->xdg_surface);
    if (client->xdg_toplevel == NULL) {
        return false;
    }

    xdg_surface_add_listener(client->xdg_surface,
        &lumo_app_xdg_surface_listener, client);
    xdg_toplevel_add_listener(client->xdg_toplevel,
        &lumo_app_xdg_toplevel_listener, client);

    snprintf(title, sizeof(title), "Lumo %s", lumo_app_title(client->app_id));
    snprintf(app_id, sizeof(app_id), "lumo-%s",
        app_name != NULL ? app_name : "app");
    xdg_toplevel_set_title(client->xdg_toplevel, title);
    xdg_toplevel_set_app_id(client->xdg_toplevel, app_id);
    xdg_toplevel_set_fullscreen(client->xdg_toplevel, NULL);

    wl_surface_commit(client->surface);
    return true;
}

static void lumo_app_client_destroy(struct lumo_app_client *client) {
    if (client == NULL) {
        return;
    }

    /* Ensure any active media playback is stopped and its child process is
     * reaped before tearing down the rest of the client state. */
    lumo_app_media_stop(client);
    lumo_app_clear_photo_thumbnails(client);
    if (client->photo_pixels != NULL) {
        free(client->photo_pixels);
        client->photo_pixels = NULL;
    }

    lumo_app_pty_cleanup(client);

    if (client->text_input != NULL) {
        zwp_text_input_v3_destroy(client->text_input);
        client->text_input = NULL;
    }
    if (client->text_input_manager != NULL) {
        zwp_text_input_manager_v3_destroy(client->text_input_manager);
        client->text_input_manager = NULL;
    }
    if (client->pointer != NULL) {
        wl_pointer_release(client->pointer);
        client->pointer = NULL;
    }
    if (client->touch != NULL) {
        wl_touch_release(client->touch);
        client->touch = NULL;
    }
    if (client->keyboard != NULL) {
        wl_keyboard_release(client->keyboard);
        client->keyboard = NULL;
    }
    if (client->seat != NULL) {
        wl_seat_release(client->seat);
        client->seat = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (client->buffers[i] != NULL) {
            lumo_app_buffer_destroy(client->buffers[i]);
            client->buffers[i] = NULL;
        }
    }
    client->buffer = NULL;
    if (client->xdg_toplevel != NULL) {
        xdg_toplevel_destroy(client->xdg_toplevel);
        client->xdg_toplevel = NULL;
    }
    if (client->xdg_surface != NULL) {
        xdg_surface_destroy(client->xdg_surface);
        client->xdg_surface = NULL;
    }
    if (client->surface != NULL) {
        wl_surface_destroy(client->surface);
        client->surface = NULL;
    }
    if (client->wm_base != NULL) {
        xdg_wm_base_destroy(client->wm_base);
        client->wm_base = NULL;
    }
    if (client->shm != NULL) {
        wl_shm_destroy(client->shm);
        client->shm = NULL;
    }
    if (client->compositor != NULL) {
        wl_compositor_destroy(client->compositor);
        client->compositor = NULL;
    }
    if (client->registry != NULL) {
        wl_registry_destroy(client->registry);
        client->registry = NULL;
    }
    if (client->display != NULL) {
        wl_display_disconnect(client->display);
        client->display = NULL;
    }
}

static void lumo_app_notes_load(struct lumo_app_client *client) {
    char path[1100];
    FILE *fp;

    /* Path safety: browse_path is initialised from $HOME (a trusted
     * environment variable) and the filename ".lumo-notes" is
     * hardcoded — no user-controlled input reaches this path. */
    snprintf(path, sizeof(path), "%s/.lumo-notes", client->browse_path);
    fp = fopen(path, "r");
    if (fp == NULL) {
        return;
    }

    client->note_count = 0;
    while (client->note_count < 8 &&
            fgets(client->notes[client->note_count],
                sizeof(client->notes[0]), fp) != NULL) {
        char *nl = strchr(client->notes[client->note_count], '\n');
        if (nl) *nl = '\0';
        if (client->notes[client->note_count][0] != '\0') {
            client->note_count++;
        }
    }
    fclose(fp);
}

static void lumo_app_notes_save(const struct lumo_app_client *client) {
    char path[1100];
    FILE *fp;

    /* Path safety: browse_path is initialised from $HOME (a trusted
     * environment variable) and the filename ".lumo-notes" is
     * hardcoded — no user-controlled input reaches this path. */
    snprintf(path, sizeof(path), "%s/.lumo-notes", client->browse_path);
    fp = fopen(path, "w");
    if (fp == NULL) {
        return;
    }

    for (int i = 0; i < client->note_count; i++) {
        fprintf(fp, "%s\n", client->notes[i]);
    }
    fclose(fp);
}

static void lumo_app_clock_save(const struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-clock", home);
    fp = fopen(path, "w");
    if (fp == NULL) return;
    fprintf(fp, "alarm_hour=%u\n", client->alarm_hour);
    fprintf(fp, "alarm_min=%u\n", client->alarm_min);
    fprintf(fp, "alarm_enabled=%d\n", client->alarm_enabled ? 1 : 0);
    fprintf(fp, "timer_total=%u\n", client->timer_total_sec);
    fprintf(fp, "clock_tab=%d\n", client->clock_tab);
    fclose(fp);
}

static void lumo_app_clock_load(struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100], line[128];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-clock", home);
    fp = fopen(path, "r");
    if (fp == NULL) return;
    while (fgets(line, sizeof(line), fp)) {
        unsigned val;
        int ival;
        if (sscanf(line, "alarm_hour=%u", &val) == 1 && val < 24) client->alarm_hour = val;
        else if (sscanf(line, "alarm_min=%u", &val) == 1 && val < 60) client->alarm_min = val;
        else if (sscanf(line, "alarm_enabled=%d", &ival) == 1) client->alarm_enabled = ival != 0;
        else if (sscanf(line, "timer_total=%u", &val) == 1 && val <= 86400) client->timer_total_sec = val;
        else if (sscanf(line, "clock_tab=%d", &ival) == 1 && ival >= 0 && ival <= 3) client->clock_tab = ival;
    }
    fclose(fp);
}

static void lumo_app_settings_save(const struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-settings", home);
    fp = fopen(path, "w");
    if (fp == NULL) return;
    fprintf(fp, "wifi_enabled=%d\n", client->settings.wifi_enabled ? 1 : 0);
    fprintf(fp, "auto_rotate=%d\n", client->settings.auto_rotate ? 1 : 0);
    fprintf(fp, "auto_updates=%d\n", client->settings.auto_updates ? 1 : 0);
    fprintf(fp, "debug_mode=%d\n", client->settings.debug_mode ? 1 : 0);
    fprintf(fp, "persist_logs=%d\n", client->settings.persist_logs ? 1 : 0);
    fclose(fp);
}

/* Phone contacts — stored in ~/.lumo-contacts, reuses notes[] array */
static void lumo_app_contacts_load(struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-contacts", home);
    fp = fopen(path, "r");
    if (fp == NULL) return;
    client->note_count = 0;
    while (client->note_count < 8 &&
            fgets(client->notes[client->note_count],
                sizeof(client->notes[0]), fp) != NULL) {
        char *nl = strchr(client->notes[client->note_count], '\n');
        if (nl) *nl = '\0';
        if (client->notes[client->note_count][0] != '\0')
            client->note_count++;
    }
    fclose(fp);
}

static void lumo_app_contacts_save(const struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-contacts", home);
    fp = fopen(path, "w");
    if (fp == NULL) return;
    for (int i = 0; i < client->note_count; i++)
        fprintf(fp, "%s\n", client->notes[i]);
    fclose(fp);
}

/* Maps places — stored in ~/.lumo-places, reuses notes[] array */
static void lumo_app_places_load(struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-places", home);
    fp = fopen(path, "r");
    if (fp == NULL) return;
    client->note_count = 0;
    while (client->note_count < 8 &&
            fgets(client->notes[client->note_count],
                sizeof(client->notes[0]), fp) != NULL) {
        char *nl = strchr(client->notes[client->note_count], '\n');
        if (nl) *nl = '\0';
        if (client->notes[client->note_count][0] != '\0')
            client->note_count++;
    }
    fclose(fp);
}

static void lumo_app_places_save(const struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-places", home);
    fp = fopen(path, "w");
    if (fp == NULL) return;
    for (int i = 0; i < client->note_count; i++)
        fprintf(fp, "%s\n", client->notes[i]);
    fclose(fp);
}

/* Browser bookmarks — stored in ~/.lumo-browser-bookmarks */
static void lumo_app_bookmarks_load(struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-browser-bookmarks", home);
    fp = fopen(path, "r");
    if (fp == NULL) {
        /* defaults */
        snprintf(client->notes[0], sizeof(client->notes[0]),
            "https://duckduckgo.com/");
        snprintf(client->notes[1], sizeof(client->notes[1]),
            "https://en.m.wikipedia.org/");
        snprintf(client->notes[2], sizeof(client->notes[2]),
            "https://github.com/");
        client->note_count = 3;
        return;
    }
    client->note_count = 0;
    while (client->note_count < 8 &&
            fgets(client->notes[client->note_count],
                sizeof(client->notes[0]), fp) != NULL) {
        char *nl = strchr(client->notes[client->note_count], '\n');
        if (nl) *nl = '\0';
        if (client->notes[client->note_count][0] != '\0')
            client->note_count++;
    }
    fclose(fp);
}

static void lumo_app_bookmarks_save(const struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100];
    FILE *fp;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-browser-bookmarks", home);
    fp = fopen(path, "w");
    if (fp == NULL) return;
    for (int i = 0; i < client->note_count; i++)
        fprintf(fp, "%s\n", client->notes[i]);
    fclose(fp);
}

/* Launch URL via pre-warmed webview (file-based IPC) or subprocess fallback */
static void lumo_app_browser_launch_url(const char *url) {
    if (url == NULL || url[0] == '\0') return;
    char resolved[4096];

    /* resolve bare domains and search queries */
    if (strstr(url, "://") != NULL) {
        snprintf(resolved, sizeof(resolved), "%s", url);
    } else if (strstr(url, "localhost") != NULL) {
        snprintf(resolved, sizeof(resolved), "http://%s", url);
    } else if (strchr(url, '.') != NULL && strchr(url, ' ') == NULL) {
        snprintf(resolved, sizeof(resolved), "https://%s", url);
    } else {
        /* URL-encode the search query to prevent injection */
        char encoded[2048];
        size_t j = 0;
        static const unsigned char safe[256] = {
            ['A']=1,['B']=1,['C']=1,['D']=1,['E']=1,['F']=1,['G']=1,
            ['H']=1,['I']=1,['J']=1,['K']=1,['L']=1,['M']=1,['N']=1,
            ['O']=1,['P']=1,['Q']=1,['R']=1,['S']=1,['T']=1,['U']=1,
            ['V']=1,['W']=1,['X']=1,['Y']=1,['Z']=1,
            ['a']=1,['b']=1,['c']=1,['d']=1,['e']=1,['f']=1,['g']=1,
            ['h']=1,['i']=1,['j']=1,['k']=1,['l']=1,['m']=1,['n']=1,
            ['o']=1,['p']=1,['q']=1,['r']=1,['s']=1,['t']=1,['u']=1,
            ['v']=1,['w']=1,['x']=1,['y']=1,['z']=1,
            ['0']=1,['1']=1,['2']=1,['3']=1,['4']=1,['5']=1,['6']=1,
            ['7']=1,['8']=1,['9']=1,['-']=1,['_']=1,['.']=1,['~']=1,
        };
        for (size_t i = 0; url[i] && j + 3 < sizeof(encoded); i++) {
            unsigned char c = (unsigned char)url[i];
            if (safe[c]) {
                encoded[j++] = (char)c;
            } else if (c == ' ') {
                encoded[j++] = '+';
            } else {
                snprintf(encoded + j, sizeof(encoded) - j,
                    "%%%02X", c);
                j += 3;
            }
        }
        encoded[j] = '\0';
        snprintf(resolved, sizeof(resolved),
            "https://duckduckgo.com/?q=%s", encoded);
    }

    /* spawn webview subprocess with the URL */
    static pid_t browser_pid;
    /* reap previous browser if it exited */
    if (browser_pid > 0)
        waitpid(browser_pid, NULL, WNOHANG);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        /* ensure WAYLAND_DISPLAY is set for the compositor socket */
        const char *wl = getenv("WAYLAND_DISPLAY");
        if (wl == NULL || wl[0] == '\0')
            setenv("WAYLAND_DISPLAY", "lumo-shell", 1);
        /* use cairo for GTK — GL renderer triggers protocol errors
         * with the Lumo compositor's current xdg-shell support */
        setenv("GSK_RENDERER", "cairo", 1);
        setenv("GTK_USE_PORTAL", "0", 1);
        if (access("/data/lumo-cache/webkit", W_OK) == 0)
            setenv("XDG_CACHE_HOME", "/data/lumo-cache/webkit", 1);
        else
            setenv("XDG_CACHE_HOME", "/tmp/lumo-webkit-cache", 1);
        execlp("lumo-webview", "lumo-webview", resolved, (char *)NULL);
        /* fallback to cairo renderer if GL fails */
        setenv("GSK_RENDERER", "cairo", 1);
        execlp("lumo-webview", "lumo-webview", resolved, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        browser_pid = pid;
        fprintf(stderr, "lumo-app: launched browser for %s (pid=%d)\n",
            resolved, (int)pid);
    }
}

static void lumo_app_settings_load(struct lumo_app_client *client) {
    const char *home = getenv("HOME");
    char path[1100], line[128];
    FILE *fp;
    /* defaults */
    client->settings.wifi_enabled = true;
    client->settings.auto_rotate = false;
    client->settings.auto_updates = false;
    client->settings.debug_mode = false;
    client->settings.persist_logs = false;
    if (home == NULL) return;
    snprintf(path, sizeof(path), "%s/.lumo-settings", home);
    fp = fopen(path, "r");
    if (fp == NULL) return;
    while (fgets(line, sizeof(line), fp)) {
        int val;
        if (sscanf(line, "wifi_enabled=%d", &val) == 1)
            client->settings.wifi_enabled = val != 0;
        else if (sscanf(line, "auto_rotate=%d", &val) == 1)
            client->settings.auto_rotate = val != 0;
        else if (sscanf(line, "auto_updates=%d", &val) == 1)
            client->settings.auto_updates = val != 0;
        else if (sscanf(line, "debug_mode=%d", &val) == 1)
            client->settings.debug_mode = val != 0;
        else if (sscanf(line, "persist_logs=%d", &val) == 1)
            client->settings.persist_logs = val != 0;
    }
    fclose(fp);
}

static void lumo_app_print_usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--app phone|messages|browser|camera|maps|music|photos|videos|clock|notes|files|settings]\n",
        argv0);
}

int main(int argc, char **argv) {
    struct lumo_app_client client = {
        .app_id = LUMO_APP_PHONE,
        .running = true,
        .active_touch_id = -1,
        .selected_row = -1,
        .note_editing = -1,
        .pty_fd = -1,
        .pty_pid = 0,
        .alarm_hour = 6,
        .alarm_min = 30,
        .zoom_scale = 1.0,
        .pinch_base_scale = 1.0,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lumo_app_print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
            if (!lumo_app_id_parse(argv[++i], &client.app_id)) {
                fprintf(stderr, "lumo-app: invalid app id '%s'\n", argv[i]);
                lumo_app_print_usage(argv[0]);
                return 1;
            }
            continue;
        }

        fprintf(stderr, "lumo-app: unknown argument '%s'\n", argv[i]);
        lumo_app_print_usage(argv[0]);
        return 1;
    }

    {
        const char *home = getenv("HOME");
        if (home != NULL && strlen(home) < sizeof(client.browse_path)) {
            strncpy(client.browse_path, home, sizeof(client.browse_path) - 1);
        } else {
            strncpy(client.browse_path, "/home", sizeof(client.browse_path) - 1);
        }
        client.browse_path[sizeof(client.browse_path) - 1] = '\0';
    }

    if (client.app_id == LUMO_APP_PDF) {
        const char *pdf_path = getenv("LUMO_PDF_FILE");
        if (pdf_path && pdf_path[0])
            lumo_pdf_open(pdf_path);
    }
    if (client.app_id == LUMO_APP_NOTES) {
        lumo_app_notes_load(&client);
    }
    if (client.app_id == LUMO_APP_PHONE) {
        lumo_app_contacts_load(&client);
    }
    if (client.app_id == LUMO_APP_MAPS) {
        lumo_app_places_load(&client);
    }
    if (client.app_id == LUMO_APP_BROWSER) {
        lumo_app_bookmarks_load(&client);
    }
    if (client.app_id == LUMO_APP_CAMERA) {
        static const char *img_exts[] = {".jpg", ".jpeg", ".png", ".bmp"};
        lumo_app_scan_media(&client, "Pictures",
            img_exts, sizeof(img_exts) / sizeof(img_exts[0]));
    }
    if (client.app_id == LUMO_APP_CLOCK) {
        lumo_app_clock_load(&client);
    }
    if (client.app_id == LUMO_APP_SETTINGS) {
        lumo_app_settings_load(&client);
    }
    if (client.app_id == LUMO_APP_MUSIC) {
        static const char *audio_exts[] = {".mp3", ".wav", ".ogg", ".flac",
            ".m4a", ".aac"};
        lumo_app_scan_media(&client, "Music",
            audio_exts, sizeof(audio_exts) / sizeof(audio_exts[0]));
    }
    if (client.app_id == LUMO_APP_PHOTOS) {
        static const char *image_exts[] = {".jpg", ".jpeg", ".png", ".bmp",
            ".gif", ".webp"};
        lumo_app_scan_media(&client, "Pictures",
            image_exts, sizeof(image_exts) / sizeof(image_exts[0]));
        lumo_app_prepare_photo_thumbnails(&client);
    }
    if (client.app_id == LUMO_APP_VIDEOS) {
        static const char *video_exts[] = {".mp4", ".mkv", ".avi", ".mov",
            ".webm"};
        lumo_app_scan_media(&client, "Videos",
            video_exts, sizeof(video_exts) / sizeof(video_exts[0]));
    }

    client.display = wl_display_connect(NULL);
    if (client.display == NULL) {
        fprintf(stderr, "lumo-app: failed to connect to Wayland display\n");
        return 1;
    }

    client.registry = wl_display_get_registry(client.display);
    wl_registry_add_listener(client.registry, &lumo_app_registry_listener,
        &client);
    wl_display_roundtrip(client.display);

    if (client.compositor == NULL || client.shm == NULL || client.wm_base == NULL) {
        fprintf(stderr, "lumo-app: missing compositor, shm, or xdg-shell global\n");
        lumo_app_client_destroy(&client);
        return 1;
    }

    if (!lumo_app_client_create_surface(&client)) {
        fprintf(stderr, "lumo-app: failed to create app surface\n");
        lumo_app_client_destroy(&client);
        return 1;
    }

    /* create text-input-v3 for OSK support */
    if (client.text_input_manager != NULL && client.seat != NULL) {
        client.text_input = zwp_text_input_manager_v3_get_text_input(
            client.text_input_manager, client.seat);
        if (client.text_input != NULL) {
            zwp_text_input_v3_add_listener(client.text_input,
                &lumo_app_text_input_listener, &client);
            /* do NOT enable here — wait for the compositor to send the
             * enter event (which arrives after wl_display_roundtrip).
             * enable()+commit() must happen after enter, otherwise
             * wlroots ignores the commit and current_enabled stays false */
            fprintf(stderr, "lumo-app: text-input-v3 ready\n");
        }
    }

    /* roundtrip so the compositor's enter event arrives before we
     * proceed — the enter callback will enable text-input for
     * apps that want the OSK (terminal, notes) */
    wl_display_roundtrip(client.display);

    /* set up PTY for terminal app */
    if (client.app_id == LUMO_APP_MESSAGES) {
        if (!lumo_app_pty_setup(&client)) {
            fprintf(stderr, "lumo-app: PTY setup failed, running in echo mode\n");
        } else {
            fprintf(stderr, "lumo-app: PTY shell started (pid=%d)\n",
                (int)client.pty_pid);
        }
    }

    {
        int display_fd = wl_display_get_fd(client.display);
        bool is_terminal = client.app_id == LUMO_APP_MESSAGES &&
            client.pty_fd >= 0;
        /* Clock must redraw every second to keep the displayed time current.
         * Settings polls every 5 s so status values (battery, wifi, etc.) stay
         * reasonably fresh without hammering the compositor. */
        bool needs_periodic = client.app_id == LUMO_APP_CLOCK ||
            client.app_id == LUMO_APP_SETTINGS ||
            client.app_id == LUMO_APP_NOTES ||
            client.app_id == LUMO_APP_MAPS ||
            client.app_id == LUMO_APP_BROWSER ||
            client.app_id == LUMO_APP_SYSMON ||
            client.app_id == LUMO_APP_GITHUB ||
            client.app_id == LUMO_APP_WEATHER ||
            client.app_id == LUMO_APP_RECORDER ||
            client.app_id == LUMO_APP_CALENDAR ||
            client.app_id == LUMO_APP_CONTACTS ||
            client.app_id == LUMO_APP_TASKS ||
            client.app_id == LUMO_APP_PDF ||
            is_terminal;
        int timeout_ms =
            client.app_id == LUMO_APP_SYSMON ? 1000 :
            client.app_id == LUMO_APP_CLOCK ? 1000 :
            client.app_id == LUMO_APP_SETTINGS ? 5000 :
            client.app_id == LUMO_APP_GITHUB ? 2000 :
            client.app_id == LUMO_APP_WEATHER ? 2000 :
            client.app_id == LUMO_APP_RECORDER ? 100 :
            (is_terminal || client.app_id == LUMO_APP_NOTES ||
             client.app_id == LUMO_APP_MAPS ||
             client.app_id == LUMO_APP_CALENDAR ||
             client.app_id == LUMO_APP_CONTACTS ||
             client.app_id == LUMO_APP_TASKS ||
             client.app_id == LUMO_APP_BROWSER) ? 500 : -1;

        while (client.running) {
            struct pollfd pfds[2];
            int nfds = 1;
            int ret;

            pfds[0].fd = display_fd;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            if (is_terminal) {
                pfds[1].fd = client.pty_fd;
                pfds[1].events = POLLIN;
                pfds[1].revents = 0;
                nfds = 2;
            }

            if (wl_display_dispatch_pending(client.display) == -1) {
                fprintf(stderr, "lumo-app: dispatch_pending failed\n");
                break;
            }
            if (wl_display_flush(client.display) < 0 && errno != EAGAIN) {
                fprintf(stderr, "lumo-app: display flush failed: %s\n",
                    strerror(errno));
                break;
            }

            ret = poll(pfds, (nfds_t)nfds, timeout_ms);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "lumo-app: poll failed: %s\n",
                    strerror(errno));
                break;
            }

            if (ret == 0 && needs_periodic) {
                if (is_terminal) {
                    lumo_app_pty_read(&client);
                }
                /* always redraw on timeout for cursor blink and clock updates */
                (void)lumo_app_client_redraw(&client);
                continue;
            }

            if (pfds[0].revents & POLLIN) {
                if (wl_display_dispatch(client.display) == -1) {
                    fprintf(stderr, "lumo-app: display dispatch failed\n");
                    break;
                }
            }
            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "lumo-app: display fd error (revents=0x%x)\n",
                    pfds[0].revents);
                break;
            }
            if (is_terminal && (pfds[1].revents & POLLIN)) {
                lumo_app_pty_read(&client);
            }
            if (is_terminal && (pfds[1].revents & POLLHUP)) {
                fprintf(stderr, "lumo-app: PTY shell exited, closing app\n");
                close(client.pty_fd);
                client.pty_fd = -1;
                client.running = false;
            }
        }
    }

    lumo_app_client_destroy(&client);
    return 0;
}
