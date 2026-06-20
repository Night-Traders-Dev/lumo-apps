/* Lumo Browser — touch-first WebKitGTK browser for Lumo Compositor.
 * Full custom UI: tabbed browsing, address bar, navigation, bookmarks.
 * Runs as a standalone Wayland client using GTK4 + WebKitGTK 6.0. */

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>
#include <stdio.h>
#include "browser_url.h"
#include "lumo/version.h"

#define LUMO_BROWSER_TITLE "Lumo Browser"
#define LUMO_MAX_TABS 8
#define LUMO_MAX_BOOKMARKS 16

/* ── Lumo theme colors ─────────────────────────────────────────────── */
#define LUMO_BG       "#2C001E"
#define LUMO_SURFACE  "#3D0028"
#define LUMO_ACCENT   "#E95420"
#define LUMO_TEXT     "#FFFFFF"
#define LUMO_DIM      "#AEA79F"
#define LUMO_BORDER   "#77216F"

/* ── Data structures ───────────────────────────────────────────────── */

typedef struct {
    WebKitWebView *web_view;
    char title[128];
    char uri[2048];
} LumoTab;

typedef struct {
    char title[64];
    char uri[2048];
} LumoBookmark;

typedef struct {
    GtkWindow *window;
    GtkBox *main_box;
    GtkBox *toolbar;
    GtkBox *tab_bar;
    GtkStack *view_stack;
    GtkEntry *url_bar;
    GtkButton *back_btn;
    GtkButton *forward_btn;
    GtkButton *reload_btn;
    GtkButton *new_tab_btn;
    GtkButton *close_tab_btn;
    GtkButton *bookmark_btn;
    GtkButton *find_btn;
    GtkButton *zoom_in_btn;
    GtkButton *zoom_out_btn;
    GtkWidget *progress_bar;
    GtkBox *find_bar;
    GtkEntry *find_entry;
    gboolean find_visible;
    double zoom_level;

    LumoTab tabs[LUMO_MAX_TABS];
    int tab_count;
    int active_tab;

    LumoBookmark bookmarks[LUMO_MAX_BOOKMARKS];
    int bookmark_count;
} LumoBrowser;

/* URL handling: see browser_url.h for lumo_url_encode / lumo_resolve_url */

/* ── Bookmark persistence ──────────────────────────────────────────── */

static void bookmarks_load(LumoBrowser *b) {
    char path[512];
    const char *home = g_get_home_dir();
    FILE *fp;
    snprintf(path, sizeof(path), "%s/.lumo-bookmarks", home);
    fp = fopen(path, "r");
    if (!fp) {
        snprintf(b->bookmarks[0].title, 64, "DuckDuckGo");
        snprintf(b->bookmarks[0].uri, 2048, "https://duckduckgo.com/");
        snprintf(b->bookmarks[1].title, 64, "Wikipedia");
        snprintf(b->bookmarks[1].uri, 2048, "https://en.m.wikipedia.org/");
        snprintf(b->bookmarks[2].title, 64, "GitHub");
        snprintf(b->bookmarks[2].uri, 2048, "https://github.com/");
        b->bookmark_count = 3;
        return;
    }
    b->bookmark_count = 0;
    char line[4096];
    while (b->bookmark_count < LUMO_MAX_BOOKMARKS &&
            fgets(line, sizeof(line), fp)) {
        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';
        char *nl = strchr(sep + 1, '\n');
        if (nl) *nl = '\0';
        snprintf(b->bookmarks[b->bookmark_count].title, 64, "%s", line);
        snprintf(b->bookmarks[b->bookmark_count].uri, 2048, "%s", sep + 1);
        b->bookmark_count++;
    }
    fclose(fp);
}

static void bookmarks_save(LumoBrowser *b) {
    char path[512];
    const char *home = g_get_home_dir();
    FILE *fp;
    snprintf(path, sizeof(path), "%s/.lumo-bookmarks", home);
    fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 0; i < b->bookmark_count; i++)
        fprintf(fp, "%s|%s\n", b->bookmarks[i].title, b->bookmarks[i].uri);
    fclose(fp);
}

/* ── Forward declarations ──────────────────────────────────────────── */

static void switch_to_tab(LumoBrowser *b, int idx);
static void update_tab_bar(LumoBrowser *b);
static void update_nav_sensitivity(LumoBrowser *b);
static int add_tab(LumoBrowser *b, const char *uri);

/* ── Tab bar ───────────────────────────────────────────────────────── */

static void on_tab_clicked(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "tab-index"));
    switch_to_tab(b, idx);
}

static void update_tab_bar(LumoBrowser *b) {
    /* count existing dynamic tab buttons */
    int existing = 0;
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(b->tab_bar));
    while (child) {
        if (child != GTK_WIDGET(b->new_tab_btn) &&
                child != GTK_WIDGET(b->close_tab_btn))
            existing++;
        child = gtk_widget_get_next_sibling(child);
    }

    /* if tab count changed, do a full rebuild (rare: only on add/close) */
    if (existing != b->tab_count) {
        child = gtk_widget_get_first_child(GTK_WIDGET(b->tab_bar));
        while (child) {
            GtkWidget *next = gtk_widget_get_next_sibling(child);
            if (child != GTK_WIDGET(b->new_tab_btn) &&
                    child != GTK_WIDGET(b->close_tab_btn))
                gtk_box_remove(b->tab_bar, child);
            child = next;
        }
        for (int i = b->tab_count - 1; i >= 0; i--) {
            char label[48];
            const char *title = b->tabs[i].title[0]
                ? b->tabs[i].title : "New Tab";
            snprintf(label, sizeof(label), "%.18s", title);
            GtkWidget *btn = gtk_button_new_with_label(label);
            gtk_widget_set_hexpand(btn, TRUE);
            g_object_set_data(G_OBJECT(btn), "tab-index",
                GINT_TO_POINTER(i));
            g_signal_connect(btn, "clicked",
                G_CALLBACK(on_tab_clicked), b);
            gtk_box_insert_child_after(b->tab_bar, btn, NULL);
        }
    }

    /* update labels and active/inactive CSS (fast path for tab switches) */
    child = gtk_widget_get_first_child(GTK_WIDGET(b->tab_bar));
    while (child) {
        if (child != GTK_WIDGET(b->new_tab_btn) &&
                child != GTK_WIDGET(b->close_tab_btn) &&
                GTK_IS_BUTTON(child)) {
            int idx = GPOINTER_TO_INT(
                g_object_get_data(G_OBJECT(child), "tab-index"));
            if (idx >= 0 && idx < b->tab_count) {
                char label[48];
                const char *title = b->tabs[idx].title[0]
                    ? b->tabs[idx].title : "New Tab";
                snprintf(label, sizeof(label), "%.18s", title);
                gtk_button_set_label(GTK_BUTTON(child), label);
                if (idx == b->active_tab) {
                    gtk_widget_add_css_class(child, "tab-active");
                    gtk_widget_remove_css_class(child, "tab-inactive");
                } else {
                    gtk_widget_add_css_class(child, "tab-inactive");
                    gtk_widget_remove_css_class(child, "tab-active");
                }
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }
}

/* ── WebView creation ──────────────────────────────────────────────── */

static WebKitWebView *create_web_view(void) {
    WebKitWebView *wv;
    WebKitSettings *settings;
    WebKitNetworkSession *session;

    /* share a single network session across all tabs */
    session = webkit_network_session_get_default();
    {
        WebKitWebsiteDataManager *dm =
            webkit_network_session_get_website_data_manager(session);
        if (dm != NULL) {
            webkit_website_data_manager_set_favicons_enabled(dm, TRUE);
        }
        /* enable persistent cookies for login sessions */
        WebKitCookieManager *cookies =
            webkit_network_session_get_cookie_manager(session);
        if (cookies) {
            webkit_cookie_manager_set_persistent_storage(cookies,
                "/tmp/lumo-browser-cookies.db",
                WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
            webkit_cookie_manager_set_accept_policy(cookies,
                WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
        }
    }
    wv = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "network-session", session, NULL));

    settings = webkit_web_view_get_settings(wv);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(settings,
        WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
    webkit_settings_set_enable_media(settings, TRUE);
    webkit_settings_set_enable_webaudio(settings, TRUE);
    webkit_settings_set_enable_media_stream(settings, FALSE);
    webkit_settings_set_enable_webgl(settings, TRUE);

    /* performance: enable back-forward cache for instant navigation */
    webkit_settings_set_enable_back_forward_navigation_gestures(settings,
        TRUE);

    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36");

    gtk_widget_set_vexpand(GTK_WIDGET(wv), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(wv), TRUE);

    return wv;
}

/* ── New window / target=_blank → open in new tab ──────────────────── */

static WebKitWebView *on_create_web_view(WebKitWebView *wv,
    WebKitNavigationAction *nav, gpointer data)
{
    LumoBrowser *b = data;
    (void)wv;
    WebKitURIRequest *req = webkit_navigation_action_get_request(nav);
    const char *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    int idx = add_tab(b, uri);
    if (idx >= 0) {
        switch_to_tab(b, idx);
        return b->tabs[idx].web_view;
    }
    return NULL;
}

/* ── TLS error handling — allow self-signed certs on local network ── */

static gboolean on_load_failed_with_tls(WebKitWebView *wv,
    const char *failing_uri, GTlsCertificate *cert,
    GTlsCertificateFlags errors, gpointer data)
{
    (void)data; (void)cert; (void)errors;
    /* allow local network TLS errors */
    if (failing_uri && (strstr(failing_uri, "localhost") ||
            strstr(failing_uri, "192.168.") ||
            strstr(failing_uri, "10.") ||
            strstr(failing_uri, "127.0."))) {
        WebKitNetworkSession *session =
            webkit_web_view_get_network_session(wv);
        webkit_network_session_allow_tls_certificate_for_host(session,
            cert, g_uri_parse(failing_uri, G_URI_FLAGS_NONE, NULL)
                ? g_uri_get_host(g_uri_parse(failing_uri,
                    G_URI_FLAGS_NONE, NULL))
                : "localhost");
        webkit_web_view_load_uri(wv, failing_uri);
        return TRUE;
    }
    return FALSE;
}

/* ── Download handling ─────────────────────────────────────────────── */

static void on_download_decide_destination(WebKitDownload *download,
    const char *suggested_filename, gpointer data)
{
    (void)data;
    char path[2048];
    const char *home = g_get_home_dir();
    snprintf(path, sizeof(path), "file://%s/Downloads/%s",
        home, suggested_filename);

    /* create Downloads dir if needed */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/Downloads", home);
    g_mkdir_with_parents(dir, 0755);

    webkit_download_set_destination(download, path);
    fprintf(stderr, "lumo-browser: downloading to %s\n", path);
}

static void on_download_started(WebKitWebView *wv,
    WebKitDownload *download, gpointer data)
{
    (void)wv;
    g_signal_connect(download, "decide-destination",
        G_CALLBACK(on_download_decide_destination), data);
}

/* ── Load progress ─────────────────────────────────────────────────── */

static void on_estimated_load_progress(WebKitWebView *wv,
    GParamSpec *pspec, gpointer data)
{
    LumoBrowser *b = data;
    (void)pspec;
    double progress = webkit_web_view_get_estimated_load_progress(wv);

    /* only update for active tab */
    if (b->active_tab >= 0 && b->active_tab < b->tab_count &&
            b->tabs[b->active_tab].web_view == wv) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->progress_bar),
            progress);
        gtk_widget_set_visible(b->progress_bar, progress < 1.0);
    }
}

/* ── Tab load events ───────────────────────────────────────────────── */

static void on_tab_load_changed(WebKitWebView *wv,
    WebKitLoadEvent event, gpointer data)
{
    LumoBrowser *b = data;
    int idx = -1;
    for (int i = 0; i < b->tab_count; i++) {
        if (b->tabs[i].web_view == wv) { idx = i; break; }
    }
    if (idx < 0) return;

    if (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED) {
        const char *uri = webkit_web_view_get_uri(wv);
        if (uri) snprintf(b->tabs[idx].uri, sizeof(b->tabs[idx].uri),
            "%s", uri);
        if (idx == b->active_tab && uri)
            gtk_editable_set_text(GTK_EDITABLE(b->url_bar), uri);
    }
    if (event == WEBKIT_LOAD_FINISHED) {
        const char *title = webkit_web_view_get_title(wv);
        if (title) {
            snprintf(b->tabs[idx].title, sizeof(b->tabs[idx].title),
                "%s", title);
            update_tab_bar(b);
        }
        if (idx == b->active_tab)
            update_nav_sensitivity(b);
    }
}

/* ── Navigation sensitivity ────────────────────────────────────────── */

static void update_nav_sensitivity(LumoBrowser *b) {
    if (b->active_tab < 0 || b->active_tab >= b->tab_count) return;
    WebKitWebView *wv = b->tabs[b->active_tab].web_view;
    gtk_widget_set_sensitive(GTK_WIDGET(b->back_btn),
        webkit_web_view_can_go_back(wv));
    gtk_widget_set_sensitive(GTK_WIDGET(b->forward_btn),
        webkit_web_view_can_go_forward(wv));
}

/* ── Tab management ────────────────────────────────────────────────── */

static const char *start_page_html =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{background:" LUMO_BG ";color:" LUMO_TEXT ";font-family:sans-serif;"
    "display:flex;flex-direction:column;align-items:center;"
    "justify-content:center;height:90vh;margin:0}"
    "h1{color:" LUMO_ACCENT ";font-size:2em;margin-bottom:0.2em}"
    "form{width:80%;max-width:400px}"
    "input{width:100%;padding:12px;font-size:1.1em;border:2px solid " LUMO_BORDER ";"
    "border-radius:24px;background:#1d0014;color:" LUMO_TEXT ";text-align:center}"
    "input:focus{border-color:" LUMO_ACCENT ";outline:none}"
    ".bookmarks{display:flex;flex-wrap:wrap;gap:12px;margin-top:24px;"
    "justify-content:center}"
    ".bm{background:" LUMO_SURFACE ";color:" LUMO_DIM ";padding:10px 18px;"
    "border-radius:16px;text-decoration:none;font-size:0.9em;"
    "border:1px solid " LUMO_BORDER "}"
    ".bm:hover{border-color:" LUMO_ACCENT ";color:" LUMO_TEXT "}"
    "p{color:" LUMO_DIM ";font-size:0.8em;margin-top:2em}"
    "</style></head><body>"
    "<h1>Lumo Browser</h1>"
    "<form action='https://duckduckgo.com/' method='get'>"
    "<input name='q' placeholder='Search the web...' autofocus>"
    "</form>"
    "<div class='bookmarks'>"
    "<a class='bm' href='https://duckduckgo.com/'>DuckDuckGo</a>"
    "<a class='bm' href='https://en.m.wikipedia.org/'>Wikipedia</a>"
    "<a class='bm' href='https://github.com/'>GitHub</a>"
    "</div>"
    "<p>Lumo Browser v" LUMO_VERSION_STRING " | WebKit</p>"
    "</body></html>";

static int add_tab(LumoBrowser *b, const char *uri) {
    if (b->tab_count >= LUMO_MAX_TABS) return -1;
    int idx = b->tab_count++;
    LumoTab *tab = &b->tabs[idx];

    tab->web_view = create_web_view();
    tab->title[0] = '\0';
    snprintf(tab->uri, sizeof(tab->uri), "%s",
        uri ? uri : "about:blank");

    g_signal_connect(tab->web_view, "load-changed",
        G_CALLBACK(on_tab_load_changed), b);
    g_signal_connect(tab->web_view, "notify::estimated-load-progress",
        G_CALLBACK(on_estimated_load_progress), b);
    g_signal_connect(tab->web_view, "create",
        G_CALLBACK(on_create_web_view), b);
    g_signal_connect(tab->web_view, "load-failed-with-tls-errors",
        G_CALLBACK(on_load_failed_with_tls), b);
    /* download-started is on the WebKitNetworkSession, not the WebView */

    char stack_name[16];
    snprintf(stack_name, sizeof(stack_name), "tab%d", idx);
    gtk_stack_add_named(b->view_stack, GTK_WIDGET(tab->web_view),
        stack_name);

    if (uri && uri[0])
        webkit_web_view_load_uri(tab->web_view, uri);
    else
        webkit_web_view_load_html(tab->web_view, start_page_html,
            "about:lumo");

    return idx;
}

static void switch_to_tab(LumoBrowser *b, int idx) {
    if (idx < 0 || idx >= b->tab_count) return;
    b->active_tab = idx;

    char stack_name[16];
    snprintf(stack_name, sizeof(stack_name), "tab%d", idx);
    gtk_stack_set_visible_child_name(b->view_stack, stack_name);

    const char *uri = webkit_web_view_get_uri(b->tabs[idx].web_view);
    if (uri)
        gtk_editable_set_text(GTK_EDITABLE(b->url_bar), uri);
    else
        gtk_editable_set_text(GTK_EDITABLE(b->url_bar), "about:lumo");

    update_nav_sensitivity(b);
    update_tab_bar(b);
}

/* ── UI callbacks ──────────────────────────────────────────────────── */

static void on_url_activate(GtkEntry *entry, gpointer data) {
    LumoBrowser *b = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!text || !text[0]) return;

    char url[4096];
    lumo_resolve_url(text, url, sizeof(url));

    if (b->active_tab >= 0 && b->active_tab < b->tab_count)
        webkit_web_view_load_uri(b->tabs[b->active_tab].web_view, url);
}

static void on_back(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    if (b->active_tab >= 0 && b->active_tab < b->tab_count &&
            webkit_web_view_can_go_back(b->tabs[b->active_tab].web_view))
        webkit_web_view_go_back(b->tabs[b->active_tab].web_view);
}

static void on_forward(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    if (b->active_tab >= 0 && b->active_tab < b->tab_count &&
            webkit_web_view_can_go_forward(b->tabs[b->active_tab].web_view))
        webkit_web_view_go_forward(b->tabs[b->active_tab].web_view);
}

static void on_reload(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    if (b->active_tab >= 0 && b->active_tab < b->tab_count)
        webkit_web_view_reload(b->tabs[b->active_tab].web_view);
}

static void on_new_tab(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    int idx = add_tab(b, NULL);
    if (idx >= 0) switch_to_tab(b, idx);
}

static void on_close_tab(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    if (b->tab_count <= 1) {
        gtk_window_close(b->window);
        return;
    }
    int idx = b->active_tab;

    /* remove and destroy the web view from stack */
    char stack_name[16];
    snprintf(stack_name, sizeof(stack_name), "tab%d", idx);
    GtkWidget *child = gtk_stack_get_child_by_name(b->view_stack, stack_name);
    if (child) {
        /* disconnect signals before removal to prevent use-after-free */
        if (b->tabs[idx].web_view)
            g_signal_handlers_disconnect_by_data(b->tabs[idx].web_view, b);
        gtk_stack_remove(b->view_stack, child);
    }
    b->tabs[idx].web_view = NULL;

    /* shift tabs down */
    for (int i = idx; i < b->tab_count - 1; i++)
        b->tabs[i] = b->tabs[i + 1];
    b->tab_count--;

    /* fix stack names for shifted tabs — re-add with correct names */
    for (int i = idx; i < b->tab_count; i++) {
        char old_name[16], new_name[16];
        snprintf(old_name, sizeof(old_name), "tab%d", i + 1);
        snprintf(new_name, sizeof(new_name), "tab%d", i);
        GtkWidget *w = gtk_stack_get_child_by_name(b->view_stack, old_name);
        if (w) {
            g_object_ref(w);
            gtk_stack_remove(b->view_stack, w);
            gtk_stack_add_named(b->view_stack, w, new_name);
            g_object_unref(w);
        }
    }

    if (b->active_tab >= b->tab_count)
        b->active_tab = b->tab_count - 1;
    if (b->active_tab < 0) b->active_tab = 0;

    switch_to_tab(b, b->active_tab);
}

static void on_add_bookmark(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    if (b->bookmark_count >= LUMO_MAX_BOOKMARKS) return;
    if (b->active_tab < 0 || b->active_tab >= b->tab_count) return;

    LumoTab *tab = &b->tabs[b->active_tab];
    const char *title = webkit_web_view_get_title(tab->web_view);
    const char *uri = webkit_web_view_get_uri(tab->web_view);
    if (!uri || !uri[0]) return;

    for (int i = 0; i < b->bookmark_count; i++) {
        if (strcmp(b->bookmarks[i].uri, uri) == 0) return;
    }

    snprintf(b->bookmarks[b->bookmark_count].title, 64, "%s",
        title ? title : "Untitled");
    snprintf(b->bookmarks[b->bookmark_count].uri, 2048, "%s", uri);
    b->bookmark_count++;
    bookmarks_save(b);
}

/* ── Find in page ──────────────────────────────────────────────────── */

static void on_find_text_changed(GtkEditable *editable, gpointer data) {
    LumoBrowser *b = data;
    const char *text = gtk_editable_get_text(editable);
    if (b->active_tab < 0 || b->active_tab >= b->tab_count) return;

    WebKitWebView *wv = b->tabs[b->active_tab].web_view;
    WebKitFindController *fc = webkit_web_view_get_find_controller(wv);

    if (text && text[0]) {
        webkit_find_controller_search(fc, text,
            WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE |
            WEBKIT_FIND_OPTIONS_WRAP_AROUND, 0);
    } else {
        webkit_find_controller_search_finish(fc);
    }
}

static void on_find_activate(GtkEntry *entry, gpointer data) {
    LumoBrowser *b = data;
    (void)entry;
    if (b->active_tab < 0 || b->active_tab >= b->tab_count) return;
    WebKitFindController *fc = webkit_web_view_get_find_controller(
        b->tabs[b->active_tab].web_view);
    webkit_find_controller_search_next(fc);
}

static void on_toggle_find(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    b->find_visible = !b->find_visible;
    gtk_widget_set_visible(GTK_WIDGET(b->find_bar), b->find_visible);
    if (b->find_visible) {
        gtk_widget_grab_focus(GTK_WIDGET(b->find_entry));
    } else if (b->active_tab >= 0 && b->active_tab < b->tab_count) {
        WebKitFindController *fc = webkit_web_view_get_find_controller(
            b->tabs[b->active_tab].web_view);
        webkit_find_controller_search_finish(fc);
    }
}

/* ── Zoom controls ─────────────────────────────────────────────────── */

static void on_zoom_in(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    if (b->active_tab < 0 || b->active_tab >= b->tab_count) return;
    b->zoom_level += 0.1;
    if (b->zoom_level > 3.0) b->zoom_level = 3.0;
    webkit_web_view_set_zoom_level(b->tabs[b->active_tab].web_view,
        b->zoom_level);
}

static void on_zoom_out(GtkButton *btn, gpointer data) {
    LumoBrowser *b = data; (void)btn;
    if (b->active_tab < 0 || b->active_tab >= b->tab_count) return;
    b->zoom_level -= 0.1;
    if (b->zoom_level < 0.5) b->zoom_level = 0.5;
    webkit_web_view_set_zoom_level(b->tabs[b->active_tab].web_view,
        b->zoom_level);
}

/* ── CSS theming ───────────────────────────────────────────────────── */

static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "window { background: " LUMO_BG "; }\n"
        "box { background: transparent; }\n"
        ".toolbar { background: " LUMO_SURFACE "; padding: 4px; }\n"
        ".tab-bar { background: " LUMO_BG "; padding: 2px 4px; }\n"
        ".tab-active { background: " LUMO_ACCENT "; color: " LUMO_TEXT ";"
        "  border-radius: 8px; padding: 6px 12px; font-weight: bold;"
        "  border: none; min-height: 28px; }\n"
        ".tab-inactive { background: " LUMO_SURFACE "; color: " LUMO_DIM ";"
        "  border-radius: 8px; padding: 6px 12px;"
        "  border: 1px solid " LUMO_BORDER "; min-height: 28px; }\n"
        ".tab-inactive:hover { color: " LUMO_TEXT "; border-color: " LUMO_ACCENT "; }\n"
        ".nav-btn { background: " LUMO_SURFACE "; color: " LUMO_DIM ";"
        "  border-radius: 8px; padding: 4px 10px;"
        "  border: 1px solid " LUMO_BORDER "; min-width: 36px; min-height: 36px; }\n"
        ".nav-btn:hover { color: " LUMO_ACCENT "; border-color: " LUMO_ACCENT "; }\n"
        ".nav-btn:disabled { opacity: 0.3; }\n"
        ".accent-btn { background: " LUMO_ACCENT "; color: " LUMO_TEXT ";"
        "  border-radius: 8px; padding: 4px 10px;"
        "  border: none; min-width: 36px; min-height: 36px; }\n"
        ".url-entry { background: #1D0014; color: " LUMO_TEXT ";"
        "  border: 2px solid " LUMO_BORDER "; border-radius: 20px;"
        "  padding: 6px 16px; font-size: 14px; }\n"
        ".url-entry:focus { border-color: " LUMO_ACCENT "; }\n"
        "progressbar trough { background: " LUMO_SURFACE ";"
        "  min-height: 3px; }\n"
        "progressbar progress { background: " LUMO_ACCENT ";"
        "  min-height: 3px; }\n"
        ".find-bar { background: " LUMO_SURFACE "; padding: 4px; }\n"
        ".find-entry { background: #1D0014; color: " LUMO_TEXT ";"
        "  border: 2px solid " LUMO_BORDER "; border-radius: 12px;"
        "  padding: 4px 12px; font-size: 13px; }\n"
        ".find-entry:focus { border-color: " LUMO_ACCENT "; }\n"
    );
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ── App activation ────────────────────────────────────────────────── */

static void activate(GtkApplication *app, gpointer user_data) {
    LumoBrowser *b = user_data;

    apply_css();
    bookmarks_load(b);

    b->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(b->window, LUMO_BROWSER_TITLE);
    gtk_window_set_default_size(b->window, 1280, 800);
    gtk_window_fullscreen(b->window);

    b->main_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_window_set_child(b->window, GTK_WIDGET(b->main_box));

    /* ── Tab bar ── */
    b->tab_bar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    gtk_widget_add_css_class(GTK_WIDGET(b->tab_bar), "tab-bar");

    b->new_tab_btn = GTK_BUTTON(gtk_button_new_with_label("+"));
    gtk_widget_add_css_class(GTK_WIDGET(b->new_tab_btn), "accent-btn");
    g_signal_connect(b->new_tab_btn, "clicked", G_CALLBACK(on_new_tab), b);

    b->close_tab_btn = GTK_BUTTON(gtk_button_new_with_label("X"));
    gtk_widget_add_css_class(GTK_WIDGET(b->close_tab_btn), "nav-btn");
    g_signal_connect(b->close_tab_btn, "clicked", G_CALLBACK(on_close_tab), b);

    /* + and X are appended to tab_bar; tab buttons are inserted before them */
    gtk_box_append(b->tab_bar, GTK_WIDGET(b->new_tab_btn));
    gtk_box_append(b->tab_bar, GTK_WIDGET(b->close_tab_btn));

    /* ── Toolbar ── */
    b->toolbar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    gtk_widget_add_css_class(GTK_WIDGET(b->toolbar), "toolbar");

    b->back_btn = GTK_BUTTON(gtk_button_new_with_label("<"));
    b->forward_btn = GTK_BUTTON(gtk_button_new_with_label(">"));
    b->reload_btn = GTK_BUTTON(gtk_button_new_with_label("R"));
    b->bookmark_btn = GTK_BUTTON(gtk_button_new_with_label("*"));

    gtk_widget_add_css_class(GTK_WIDGET(b->back_btn), "nav-btn");
    gtk_widget_add_css_class(GTK_WIDGET(b->forward_btn), "nav-btn");
    gtk_widget_add_css_class(GTK_WIDGET(b->reload_btn), "nav-btn");
    gtk_widget_add_css_class(GTK_WIDGET(b->bookmark_btn), "nav-btn");

    /* start disabled until page loads */
    gtk_widget_set_sensitive(GTK_WIDGET(b->back_btn), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(b->forward_btn), FALSE);

    b->url_bar = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(b->url_bar), "url-entry");
    gtk_editable_set_text(GTK_EDITABLE(b->url_bar), "about:lumo");
    gtk_widget_set_hexpand(GTK_WIDGET(b->url_bar), TRUE);

    g_signal_connect(b->back_btn, "clicked", G_CALLBACK(on_back), b);
    g_signal_connect(b->forward_btn, "clicked", G_CALLBACK(on_forward), b);
    g_signal_connect(b->reload_btn, "clicked", G_CALLBACK(on_reload), b);
    g_signal_connect(b->bookmark_btn, "clicked",
        G_CALLBACK(on_add_bookmark), b);
    g_signal_connect(b->url_bar, "activate", G_CALLBACK(on_url_activate), b);

    b->find_btn = GTK_BUTTON(gtk_button_new_with_label("F"));
    b->zoom_in_btn = GTK_BUTTON(gtk_button_new_with_label("A+"));
    b->zoom_out_btn = GTK_BUTTON(gtk_button_new_with_label("A-"));

    gtk_widget_add_css_class(GTK_WIDGET(b->find_btn), "nav-btn");
    gtk_widget_add_css_class(GTK_WIDGET(b->zoom_in_btn), "nav-btn");
    gtk_widget_add_css_class(GTK_WIDGET(b->zoom_out_btn), "nav-btn");

    g_signal_connect(b->find_btn, "clicked", G_CALLBACK(on_toggle_find), b);
    g_signal_connect(b->zoom_in_btn, "clicked", G_CALLBACK(on_zoom_in), b);
    g_signal_connect(b->zoom_out_btn, "clicked", G_CALLBACK(on_zoom_out), b);

    gtk_box_append(b->toolbar, GTK_WIDGET(b->back_btn));
    gtk_box_append(b->toolbar, GTK_WIDGET(b->forward_btn));
    gtk_box_append(b->toolbar, GTK_WIDGET(b->reload_btn));
    gtk_box_append(b->toolbar, GTK_WIDGET(b->url_bar));
    gtk_box_append(b->toolbar, GTK_WIDGET(b->find_btn));
    gtk_box_append(b->toolbar, GTK_WIDGET(b->zoom_out_btn));
    gtk_box_append(b->toolbar, GTK_WIDGET(b->zoom_in_btn));
    gtk_box_append(b->toolbar, GTK_WIDGET(b->bookmark_btn));

    /* ── Find bar (hidden by default) ── */
    b->find_bar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    gtk_widget_add_css_class(GTK_WIDGET(b->find_bar), "find-bar");
    gtk_widget_set_visible(GTK_WIDGET(b->find_bar), FALSE);
    b->find_visible = FALSE;
    b->find_entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(b->find_entry), "find-entry");
    gtk_widget_set_hexpand(GTK_WIDGET(b->find_entry), TRUE);
    gtk_entry_set_placeholder_text(b->find_entry, "Find in page...");
    g_signal_connect(b->find_entry, "changed",
        G_CALLBACK(on_find_text_changed), b);
    g_signal_connect(b->find_entry, "activate",
        G_CALLBACK(on_find_activate), b);
    gtk_box_append(b->find_bar, GTK_WIDGET(b->find_entry));

    /* ── Progress bar ── */
    b->progress_bar = gtk_progress_bar_new();
    gtk_widget_set_visible(b->progress_bar, FALSE);

    /* ── View stack (tab content) — first so it fills the main area ── */
    b->view_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(b->view_stack,
        GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(b->view_stack, 100);
    gtk_widget_set_vexpand(GTK_WIDGET(b->view_stack), TRUE);
    gtk_box_append(b->main_box, GTK_WIDGET(b->view_stack));

    /* Progress bar between content and toolbar */
    gtk_box_append(b->main_box, b->progress_bar);

    /* Toolbar, find bar, and tab bar at the bottom of portrait buffer =
     * right side of rotated display, away from the bottom-edge gesture zone */
    gtk_box_append(b->main_box, GTK_WIDGET(b->find_bar));
    gtk_box_append(b->main_box, GTK_WIDGET(b->toolbar));
    gtk_box_append(b->main_box, GTK_WIDGET(b->tab_bar));

    /* ── First tab ── */
    b->zoom_level = 1.0;
    b->active_tab = 0;
    on_new_tab(NULL, b);

    gtk_window_present(b->window);
}

int main(int argc, char **argv) {
    GtkApplication *app;
    LumoBrowser browser = {0};
    int status;

    /* cairo renderer for GTK4 — GL triggers Wayland protocol errors
     * with the current compositor xdg-shell implementation.
     * WebKit still uses GPU internally for page compositing. */
    setenv("GSK_RENDERER", "cairo", 0);
    /* limit web processes for memory (riscv64 has limited RAM) */
    setenv("WEBKIT_PROCESS_COUNT_LIMIT", "2", 0);
    /* disable portal timeout stall */
    setenv("GTK_USE_PORTAL", "0", 0);
    /* persistent cache on NVMe if available */
    if (access("/data/lumo-cache/webkit", W_OK) == 0)
        setenv("XDG_CACHE_HOME", "/data/lumo-cache/webkit", 0);
    else
        setenv("XDG_CACHE_HOME", "/tmp/lumo-webkit-cache", 0);

    app = gtk_application_new("com.lumo.browser",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &browser);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
