/* lumo-webview — minimal fullscreen WebKitGTK web renderer.
 * Two modes:
 *   lumo-webview <url>       — load URL immediately
 *   lumo-webview --warm      — pre-warm WebKit, wait for URL via file watch
 *
 * Pre-warm mode: starts hidden, initializes WebKit (15s cold start happens
 * in the background), then watches /tmp/lumo-webview-url for a URL to load.
 * When a URL appears, the window presents itself and navigates.
 * When the window is closed (gesture handle), it hides and waits for the
 * next URL — no re-initialization needed. */

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <gio/gio.h>

typedef struct {
    GtkApplication *app;
    GtkWindow *window;
    WebKitWebView *web_view;
    char initial_url[4096];
    gboolean warm_mode;
    gboolean window_visible;
} LumoWebView;

#define URL_FILE "/tmp/lumo-webview-url"

static void on_load_changed(WebKitWebView *wv,
    WebKitLoadEvent event, gpointer data)
{
    LumoWebView *v = data;
    if (event == WEBKIT_LOAD_FINISHED) {
        const char *title = webkit_web_view_get_title(wv);
        if (title != NULL) {
            char win_title[256];
            snprintf(win_title, sizeof(win_title), "%s - Lumo", title);
            gtk_window_set_title(v->window, win_title);
        }
    }
}

static WebKitWebView *on_create(WebKitWebView *wv,
    WebKitNavigationAction *nav, gpointer data)
{
    (void)data;
    WebKitURIRequest *req = webkit_navigation_action_get_request(nav);
    const char *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    if (uri) webkit_web_view_load_uri(wv, uri);
    return NULL;
}

/* warm mode: load URL from file and present window */
static void load_url_from_file(LumoWebView *v) {
    FILE *fp = fopen(URL_FILE, "r");
    if (!fp) return;

    char url[4096] = {0};
    if (fgets(url, sizeof(url), fp)) {
        char *nl = strchr(url, '\n');
        if (nl) *nl = '\0';
    }
    fclose(fp);
    unlink(URL_FILE);

    if (url[0] && v->web_view) {
        fprintf(stderr, "lumo-webview: loading %s\n", url);
        webkit_web_view_load_uri(v->web_view, url);
        if (!v->window_visible) {
            gtk_window_fullscreen(v->window);
            gtk_window_present(v->window);
            gtk_widget_set_visible(GTK_WIDGET(v->window), TRUE);
            v->window_visible = TRUE;
        }
    }
}

/* inotify callback: triggered when URL file is created */
static void on_url_file_changed(GFileMonitor *mon, GFile *file,
    GFile *other, GFileMonitorEvent event, gpointer data)
{
    (void)mon; (void)file; (void)other;
    if (event == G_FILE_MONITOR_EVENT_CREATED ||
            event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        load_url_from_file(data);
}

static gboolean on_close_request(GtkWindow *window, gpointer data) {
    LumoWebView *v = data;
    if (v->warm_mode) {
        /* in warm mode, hide instead of closing — stay ready */
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
        v->window_visible = FALSE;
        webkit_web_view_load_html(v->web_view,
            "<html><body style='background:#2C001E'></body></html>",
            "about:blank");
        fprintf(stderr, "lumo-webview: hidden (warm standby)\n");
        return TRUE; /* prevent actual close */
    }
    return FALSE; /* allow close in normal mode */
}

static void activate(GtkApplication *app, gpointer user_data) {
    LumoWebView *v = user_data;
    WebKitSettings *settings;
    WebKitNetworkSession *session;

    v->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(v->window, "Lumo");
    gtk_window_set_default_size(v->window, 1280, 800);

    g_signal_connect(v->window, "close-request",
        G_CALLBACK(on_close_request), v);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window { background: #2C001E; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    session = webkit_network_session_get_default();

    /* configure network session for performance */
    {
        WebKitWebsiteDataManager *dm =
            webkit_network_session_get_website_data_manager(session);
        if (dm != NULL) {
            /* enable persistent cache on tmpfs */
            webkit_website_data_manager_set_favicons_enabled(dm, FALSE);
        }
        /* Note: DNS prefetching not available in WebKitGTK 2.50 */
    }

    v->web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "network-session", session, NULL));

    settings = webkit_web_view_get_settings(v->web_view);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    /* GPU: allow hardware acceleration — PowerVR BXE-2-32 supports
     * GLES2 and Vulkan, so WebKit can use GPU compositing */
    webkit_settings_set_hardware_acceleration_policy(settings,
        WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
    webkit_settings_set_enable_webgl(settings, TRUE);

    /* back-forward navigation gestures — may not be available in all
     * WebKitGTK versions, using the web_view method if it exists */

    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36");

    g_signal_connect(v->web_view, "load-changed",
        G_CALLBACK(on_load_changed), v);
    g_signal_connect(v->web_view, "create",
        G_CALLBACK(on_create), v);

    gtk_widget_set_vexpand(GTK_WIDGET(v->web_view), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(v->web_view), TRUE);
    gtk_window_set_child(v->window, GTK_WIDGET(v->web_view));

    if (v->warm_mode) {
        /* warm mode: don't show window yet, just initialize WebKit
         * in the background. The window will be created but not
         * presented until a URL file appears. */
        webkit_web_view_load_html(v->web_view,
            "<html><body style='background:#2C001E'></body></html>",
            "about:blank");
        v->window_visible = FALSE;
        fprintf(stderr, "lumo-webview: warm init complete, watching %s\n",
            URL_FILE);
        /* use inotify via GFileMonitor instead of 500ms polling */
        GFile *dir = g_file_new_for_path("/tmp");
        GFileMonitor *mon = g_file_monitor_directory(dir,
            G_FILE_MONITOR_NONE, NULL, NULL);
        if (mon) {
            g_signal_connect(mon, "changed",
                G_CALLBACK(on_url_file_changed), v);
        } else {
            /* fallback: check once on startup */
            load_url_from_file(v);
        }
        g_object_unref(dir);
        /* also check if file already exists from before warm start */
        load_url_from_file(v);
    } else if (v->initial_url[0]) {
        webkit_web_view_load_uri(v->web_view, v->initial_url);
        gtk_window_fullscreen(v->window);
        gtk_window_present(v->window);
        v->window_visible = TRUE;
    }
}

int main(int argc, char **argv) {
    GtkApplication *app;
    LumoWebView view = {0};
    int status;

    /* performance tuning */
    setenv("GTK_USE_PORTAL", "0", 0);
    /* use LumoCache on NVMe for persistent WebKit disk cache.
     * btrfs zstd compression gives ~3:1 ratio on web resources.
     * Falls back to tmpfs if NVMe not available. */
    if (access("/data/lumo-cache/webkit", W_OK) == 0) {
        setenv("XDG_CACHE_HOME", "/data/lumo-cache/webkit", 0);
    } else {
        setenv("XDG_CACHE_HOME", "/tmp/lumo-webkit-cache", 0);
    }
    /* limit WebKit web processes */
    setenv("WEBKIT_PROCESS_COUNT_LIMIT", "1", 0);
    /* GPU: let GTK/WebKit use the PowerVR GPU for rendering.
     * The Imagination BXE-2-32 supports Vulkan 1.3 and GLES2. */

    if (argc >= 2 && strcmp(argv[1], "--warm") == 0) {
        view.warm_mode = TRUE;
        fprintf(stderr, "lumo-webview: starting in warm mode\n");
    } else if (argc >= 2) {
        snprintf(view.initial_url, sizeof(view.initial_url), "%s", argv[1]);
    }

    app = gtk_application_new("com.lumo.webview",
        G_APPLICATION_NON_UNIQUE);
    view.app = app;
    g_signal_connect(app, "activate", G_CALLBACK(activate), &view);
    status = g_application_run(G_APPLICATION(app), 1,
        (char*[]){"lumo-webview", NULL});
    g_object_unref(app);
    return status;
}
