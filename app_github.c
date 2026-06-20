/*
 * app_github.c — Lumo GitHub client (native SHM app)
 *
 * Connects to the GitHub REST API via libcurl to display repositories,
 * commits, issues, and profile info.  Uses a personal access token
 * stored in ~/.lumo-github-token for authentication.
 *
 * Views:
 *   0 = login/token entry
 *   1 = profile + repository list
 *   2 = repository detail (README, recent commits)
 *   3 = issues list
 */
#define _DEFAULT_SOURCE
#include "lumo/app_render.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

/* ── minimal JSON field extractor ──────────────────────────────── */

/* extract a string value for a given key from JSON text.
 * returns pointer into buf (null-terminated), or NULL. */
static const char *json_get_string(const char *json, const char *key,
    char *buf, size_t buf_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return NULL;
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return NULL;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos == '"') {
        pos++;
        size_t i = 0;
        while (*pos && *pos != '"' && i < buf_size - 1) {
            if (*pos == '\\' && pos[1]) { pos++; }
            buf[i++] = *pos++;
        }
        buf[i] = '\0';
        return buf;
    }
    /* number or bool */
    size_t i = 0;
    while (*pos && *pos != ',' && *pos != '}' && *pos != ']' &&
            i < buf_size - 1) {
        buf[i++] = *pos++;
    }
    buf[i] = '\0';
    return buf;
}

/* extract array of objects — returns pointers to each '{' */
static int json_get_array_objects(const char *json,
    const char **out, int max_out)
{
    const char *p = strchr(json, '[');
    if (!p) return 0;
    p++;
    int count = 0;
    int depth = 0;
    while (*p && count < max_out) {
        if (*p == '{') {
            if (depth == 0) out[count++] = p;
            depth++;
        } else if (*p == '}') {
            depth--;
        } else if (*p == ']' && depth == 0) {
            break;
        }
        p++;
    }
    return count;
}

/* ── curl helpers ──────────────────────────────────────────────── */

struct curl_buf {
    char *data;
    size_t size;
    size_t capacity;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb,
    void *userdata)
{
    struct curl_buf *buf = userdata;
    size_t total = size * nmemb;
    if (buf->size + total >= buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap < buf->size + total + 1) new_cap = buf->size + total + 1;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static char *github_api_get(const char *token, const char *endpoint) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char url[512];
    snprintf(url, sizeof(url), "https://api.github.com%s", endpoint);

    struct curl_buf buf = {0};
    buf.data = malloc(4096);
    buf.capacity = 4096;
    buf.data[0] = '\0';

    char auth[256];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "User-Agent: Lumo-OS/0.0.76");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ── GitHub data model ─────────────────────────────────────────── */

#define GH_MAX_REPOS 20
#define GH_MAX_FILES 30

struct gh_repo {
    char name[64];
    char full_name[128];
    char description[256];
    char language[32];
    int stars;
    int forks;
    bool is_private;
};

struct gh_file {
    char name[128];
    char path[256];
    char type[16]; /* "file" or "dir" */
    int size;
};

struct gh_state {
    /* auth */
    char token[128];
    bool authenticated;
    char username[64];
    char avatar_url[256];
    int public_repos;
    int followers;

    /* repos */
    struct gh_repo repos[GH_MAX_REPOS];
    int repo_count;

    /* file tree */
    struct gh_file files[GH_MAX_FILES];
    int file_count;
    char current_path[512];
    char current_repo[128];

    /* file content viewer */
    char *file_content;
    size_t file_content_len;
    char file_name[128];
    int content_scroll;

    /* README */
    char *readme_content;
    size_t readme_len;
    bool readme_loaded;

    /* UI state */
    int view;          /* 0=login, 1=repos, 2=files/readme, 3=content */
    int selected_repo;
    int scroll_offset;

    /* async loading */
    bool loading;
    char status_msg[128];
};

static struct gh_state gh = {0};
static bool gh_initialized = false;

/* ── token persistence ─────────────────────────────────────────── */

static void gh_load_token(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.lumo-github-token", home);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    if (fgets(gh.token, sizeof(gh.token), fp)) {
        char *nl = strchr(gh.token, '\n');
        if (nl) *nl = '\0';
        if (strlen(gh.token) > 10) {
            gh.authenticated = true;
        }
    }
    fclose(fp);
}

static void gh_save_token(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.lumo-github-token", home);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "%s\n", gh.token);
    fclose(fp);
    /* restrict permissions */
    chmod(path, 0600);
}

/* ── API calls (run in background thread) ──────────────────────── */

static void gh_fetch_profile(void) {
    char *json = github_api_get(gh.token, "/user");
    if (!json) {
        snprintf(gh.status_msg, sizeof(gh.status_msg), "API ERROR");
        gh.loading = false;
        return;
    }

    char buf[256];
    if (json_get_string(json, "login", buf, sizeof(buf)))
        snprintf(gh.username, sizeof(gh.username), "%s", buf);
    if (json_get_string(json, "public_repos", buf, sizeof(buf)))
        gh.public_repos = atoi(buf);
    if (json_get_string(json, "followers", buf, sizeof(buf)))
        gh.followers = atoi(buf);
    if (json_get_string(json, "avatar_url", buf, sizeof(buf)))
        snprintf(gh.avatar_url, sizeof(gh.avatar_url), "%s", buf);

    gh.authenticated = (gh.username[0] != '\0');
    free(json);
}

static void gh_fetch_repos(void) {
    char *json = github_api_get(gh.token,
        "/user/repos?sort=updated&per_page=20");
    if (!json) {
        snprintf(gh.status_msg, sizeof(gh.status_msg), "REPOS ERROR");
        gh.loading = false;
        return;
    }

    const char *objects[GH_MAX_REPOS];
    int count = json_get_array_objects(json, objects, GH_MAX_REPOS);
    gh.repo_count = 0;

    for (int i = 0; i < count && i < GH_MAX_REPOS; i++) {
        struct gh_repo *r = &gh.repos[gh.repo_count];
        char buf[256];
        char chunk[4096];
        /* extract this object's JSON (up to next '}' at depth 0) */
        const char *start = objects[i];
        int depth = 0;
        size_t len = 0;
        for (const char *p = start; *p && len < sizeof(chunk) - 1; p++) {
            chunk[len++] = *p;
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) break; }
        }
        chunk[len] = '\0';

        if (json_get_string(chunk, "name", buf, sizeof(buf)))
            snprintf(r->name, sizeof(r->name), "%s", buf);
        if (json_get_string(chunk, "full_name", buf, sizeof(buf)))
            snprintf(r->full_name, sizeof(r->full_name), "%s", buf);
        if (json_get_string(chunk, "description", buf, sizeof(buf)))
            snprintf(r->description, sizeof(r->description), "%s", buf);
        if (json_get_string(chunk, "language", buf, sizeof(buf)))
            snprintf(r->language, sizeof(r->language), "%s", buf);
        if (json_get_string(chunk, "stargazers_count", buf, sizeof(buf)))
            r->stars = atoi(buf);
        if (json_get_string(chunk, "forks_count", buf, sizeof(buf)))
            r->forks = atoi(buf);
        if (json_get_string(chunk, "private", buf, sizeof(buf)))
            r->is_private = (strcmp(buf, "true") == 0);

        if (r->name[0] != '\0')
            gh.repo_count++;
    }

    free(json);
}

static void gh_fetch_files(const char *repo, const char *path) {
    char endpoint[512];
    if (path[0] != '\0')
        snprintf(endpoint, sizeof(endpoint), "/repos/%s/contents/%s",
            repo, path);
    else
        snprintf(endpoint, sizeof(endpoint), "/repos/%s/contents", repo);

    char *json = github_api_get(gh.token, endpoint);
    if (!json) {
        snprintf(gh.status_msg, sizeof(gh.status_msg), "FETCH ERROR");
        return;
    }

    const char *objects[GH_MAX_FILES];
    int count = json_get_array_objects(json, objects, GH_MAX_FILES);
    gh.file_count = 0;

    for (int i = 0; i < count && i < GH_MAX_FILES; i++) {
        struct gh_file *f = &gh.files[gh.file_count];
        char buf[256];
        char chunk[2048];
        const char *start = objects[i];
        int depth = 0;
        size_t len = 0;
        for (const char *p = start; *p && len < sizeof(chunk) - 1; p++) {
            chunk[len++] = *p;
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) break; }
        }
        chunk[len] = '\0';

        if (json_get_string(chunk, "name", buf, sizeof(buf)))
            snprintf(f->name, sizeof(f->name), "%s", buf);
        if (json_get_string(chunk, "path", buf, sizeof(buf)))
            snprintf(f->path, sizeof(f->path), "%s", buf);
        if (json_get_string(chunk, "type", buf, sizeof(buf)))
            snprintf(f->type, sizeof(f->type), "%s", buf);
        if (json_get_string(chunk, "size", buf, sizeof(buf)))
            f->size = atoi(buf);

        if (f->name[0] != '\0')
            gh.file_count++;
    }
    free(json);
    snprintf(gh.status_msg, sizeof(gh.status_msg),
        "%d ITEMS", gh.file_count);
}

static void gh_fetch_file_content(const char *repo, const char *path) {
    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint),
        "/repos/%s/contents/%s", repo, path);

    char *json = github_api_get(gh.token, endpoint);
    if (!json) return;

    /* extract download_url and fetch raw content */
    char url_buf[1024];
    if (json_get_string(json, "download_url", url_buf, sizeof(url_buf))) {
        free(json);
        /* fetch raw content */
        CURL *curl = curl_easy_init();
        if (!curl) return;

        struct curl_buf buf = {0};
        buf.data = malloc(65536);
        buf.capacity = 65536;
        buf.data[0] = '\0';

        curl_easy_setopt(curl, CURLOPT_URL, url_buf);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lumo-OS/0.0.77");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK && buf.size > 0) {
            if (gh.file_content) free(gh.file_content);
            gh.file_content = buf.data;
            gh.file_content_len = buf.size;
        } else {
            free(buf.data);
        }
    } else {
        free(json);
    }
}

/* ── syntax highlighting colors ────────────────────────────────── */

static uint32_t syntax_color_for_token(const char *line, int pos,
    const char *ext)
{
    /* simple keyword-based highlighting */
    static const char *c_keywords[] = {
        "int", "char", "void", "bool", "if", "else", "for", "while",
        "return", "struct", "enum", "static", "const", "switch", "case",
        "break", "continue", "typedef", "sizeof", "NULL", "true", "false",
        "#include", "#define", "#ifdef", "#ifndef", "#endif", "#if", NULL
    };
    static const char *py_keywords[] = {
        "def", "class", "import", "from", "return", "if", "else", "elif",
        "for", "while", "in", "not", "and", "or", "True", "False", "None",
        "with", "as", "try", "except", "raise", "self", NULL
    };
    static const char *js_keywords[] = {
        "function", "const", "let", "var", "if", "else", "for", "while",
        "return", "class", "import", "export", "from", "async", "await",
        "true", "false", "null", "undefined", "this", "new", NULL
    };

    const char **keywords = c_keywords;
    if (ext && (strcmp(ext, ".py") == 0)) keywords = py_keywords;
    if (ext && (strcmp(ext, ".js") == 0 || strcmp(ext, ".ts") == 0))
        keywords = js_keywords;

    /* check if current position starts a keyword */
    for (int k = 0; keywords[k]; k++) {
        size_t klen = strlen(keywords[k]);
        if (strncmp(line + pos, keywords[k], klen) == 0) {
            char after = line[pos + klen];
            char before = pos > 0 ? line[pos - 1] : ' ';
            if ((after == '\0' || after == ' ' || after == '(' ||
                    after == ')' || after == ';' || after == ',' ||
                    after == '{' || after == '}' || after == ':' ||
                    after == '[' || after == ']' || after == '\n') &&
                    (before == ' ' || before == '\t' || before == '(' ||
                     before == '{' || before == ',' || before == ';' ||
                     pos == 0)) {
                return 0; /* keyword marker */
            }
        }
    }
    return 1; /* normal */
}

static void *gh_fetch_thread(void *arg) {
    (void)arg;
    gh.loading = true;
    snprintf(gh.status_msg, sizeof(gh.status_msg), "LOADING...");

    gh_fetch_profile();
    if (gh.authenticated) {
        gh_fetch_repos();
        gh.view = 1;
        snprintf(gh.status_msg, sizeof(gh.status_msg),
            "%d REPOS", gh.repo_count);
    } else {
        snprintf(gh.status_msg, sizeof(gh.status_msg), "AUTH FAILED");
        gh.view = 0;
    }

    gh.loading = false;
    return NULL;
}

/* ── render ────────────────────────────────────────────────────── */

void lumo_app_render_github(
    const struct lumo_app_render_context *ctx,
    uint32_t *pixels, uint32_t width, uint32_t height
) {
    (void)ctx;
    struct lumo_app_theme theme;
    lumo_app_theme_get(&theme);

    struct lumo_rect full = {0, 0, (int)width, (int)height};
    memset(pixels, 0, (size_t)width * height * 4);
    lumo_app_fill_gradient(pixels, width, height, &full,
        theme.header_bg, theme.bg);

    int y = 16;
    int pad = 20;
    int col_w = (int)width - pad * 2;
    char buf[256];

    /* init on first render */
    if (!gh_initialized) {
        gh_initialized = true;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        gh_load_token();
        if (gh.authenticated && !gh.loading) {
            pthread_t tid;
            pthread_create(&tid, NULL, gh_fetch_thread, NULL);
            pthread_detach(tid);
        }
    }

    /* header */
    lumo_app_draw_text(pixels, width, height, pad, y, 2,
        theme.text_dim, "GITHUB");
    y += 20;
    lumo_app_draw_text(pixels, width, height, pad, y, 4,
        theme.accent, "LUMO GITHUB");
    y += 36;
    lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
        theme.separator);
    y += 12;

    /* loading indicator */
    if (gh.loading) {
        lumo_app_draw_text(pixels, width, height, pad, y, 2,
            theme.text_dim, gh.status_msg);
        return;
    }

    /* ── view 0: login / token entry ──────────────────────────── */
    if (gh.view == 0 || !gh.authenticated) {
        lumo_app_draw_text(pixels, width, height, pad, y, 3,
            theme.text, "SIGN IN");
        y += 28;

        lumo_app_draw_text(pixels, width, height, pad, y, 1,
            theme.text_dim,
            "CREATE A PERSONAL ACCESS TOKEN AT");
        y += 14;
        lumo_app_draw_text(pixels, width, height, pad, y, 1,
            theme.accent, "GITHUB.COM/SETTINGS/TOKENS");
        y += 14;
        lumo_app_draw_text(pixels, width, height, pad, y, 1,
            theme.text_dim,
            "SAVE IT TO ~/.lumo-github-token");
        y += 14;
        lumo_app_draw_text(pixels, width, height, pad, y, 1,
            theme.text_dim,
            "THEN REOPEN THIS APP");
        y += 24;

        if (gh.token[0] != '\0') {
            snprintf(buf, sizeof(buf), "TOKEN: %c%c%c%c...%c%c%c%c",
                gh.token[0], gh.token[1], gh.token[2], gh.token[3],
                gh.token[strlen(gh.token)-4], gh.token[strlen(gh.token)-3],
                gh.token[strlen(gh.token)-2], gh.token[strlen(gh.token)-1]);
            lumo_app_draw_text(pixels, width, height, pad, y, 1,
                theme.text_dim, buf);
            y += 14;
        }

        if (gh.status_msg[0] != '\0') {
            lumo_app_draw_text(pixels, width, height, pad, y, 2,
                lumo_app_argb(0xFF, 0xFF, 0x66, 0x66), gh.status_msg);
        }
        return;
    }

    /* ── view 1: profile + repos ──────────────────────────────── */
    if (gh.view == 1) {
        /* profile header */
        snprintf(buf, sizeof(buf), "@%s", gh.username);
        lumo_app_draw_text(pixels, width, height, pad, y, 3,
            theme.accent, buf);
        y += 26;
        snprintf(buf, sizeof(buf), "%d REPOS  %d FOLLOWERS",
            gh.public_repos, gh.followers);
        lumo_app_draw_text(pixels, width, height, pad, y, 1,
            theme.text_dim, buf);
        y += 16;
        lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
            theme.separator);
        y += 12;

        /* repo list */
        int row_h = 56;
        int scroll = gh.scroll_offset;
        int max_visible = ((int)height - y - 20) / row_h;
        if (max_visible < 1) max_visible = 1;

        for (int i = scroll; i < gh.repo_count && i < scroll + max_visible;
                i++) {
            struct gh_repo *r = &gh.repos[i];
            struct lumo_rect row = {pad, y, col_w, row_h - 4};

            lumo_app_fill_rounded_rect(pixels, width, height, &row,
                10, theme.card_bg);

            /* repo name */
            lumo_app_draw_text(pixels, width, height,
                row.x + 12, row.y + 8, 2, theme.text, r->name);

            /* description (truncated) */
            if (r->description[0] != '\0') {
                char desc[60];
                snprintf(desc, sizeof(desc), "%s", r->description);
                lumo_app_draw_text(pixels, width, height,
                    row.x + 12, row.y + 26, 1, theme.text_dim, desc);
            }

            /* language + stars + forks */
            snprintf(buf, sizeof(buf), "%s  * %d  Y %d",
                r->language[0] ? r->language : "—",
                r->stars, r->forks);
            lumo_app_draw_text(pixels, width, height,
                row.x + 12, row.y + 38, 1,
                theme.text_dim, buf);

            /* private badge */
            if (r->is_private) {
                lumo_app_draw_text(pixels, width, height,
                    row.x + row.width - 60, row.y + 8, 1,
                    lumo_app_argb(0xFF, 0xFF, 0xAA, 0x44), "PRIVATE");
            }

            y += row_h;
        }

        /* scroll indicator */
        if (gh.repo_count > max_visible) {
            snprintf(buf, sizeof(buf), "%d-%d / %d",
                scroll + 1,
                scroll + max_visible > gh.repo_count
                    ? gh.repo_count : scroll + max_visible,
                gh.repo_count);
            lumo_app_draw_text(pixels, width, height,
                (int)width - 120, (int)height - 20, 1,
                theme.text_dim, buf);
        }
        return;
    }

    /* ── view 2: file tree ────────────────────────────────────── */
    if (gh.view == 2) {
        /* back button */
        {
            struct lumo_rect back_btn = {pad, y, 60, 24};
            lumo_app_fill_rounded_rect(pixels, width, height, &back_btn,
                8, theme.card_bg);
            lumo_app_draw_text_centered(pixels, width, height, &back_btn,
                2, theme.accent, "< BACK");
        }

        /* repo + path */
        lumo_app_draw_text(pixels, width, height, pad + 70, y, 2,
            theme.text, gh.current_repo);
        y += 22;
        if (gh.current_path[0] != '\0') {
            snprintf(buf, sizeof(buf), "/%s", gh.current_path);
            lumo_app_draw_text(pixels, width, height, pad, y, 1,
                theme.text_dim, buf);
            y += 14;
        }
        lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
            theme.separator);
        y += 8;

        /* README.md rendered at top when at repo root — full content,
         * no fixed height cap.  Scrolls with the file list below. */
        if (gh.current_path[0] == '\0' && gh.readme_loaded &&
                gh.readme_content != NULL) {
            /* README header label */
            lumo_app_draw_text(pixels, width, height,
                pad + 4, y, 1, theme.accent, "README.md");
            y += 14;
            lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
                theme.separator);
            y += 4;

            /* render full markdown content (scrolled by scroll_offset) */
            const char *p = gh.readme_content;
            int line_idx = 0;
            int scroll = gh.scroll_offset;
            int max_y = (int)height - 50;
            bool in_code_block = false;

            while (*p && y < max_y) {
                const char *eol = strchr(p, '\n');
                int len = eol ? (int)(eol - p) : (int)strlen(p);
                int max_chars = (col_w - 24) / 6;
                if (len > max_chars) len = max_chars;

                char line_buf[512];
                if (len > (int)sizeof(line_buf) - 1)
                    len = (int)sizeof(line_buf) - 1;
                memcpy(line_buf, p, (size_t)len);
                line_buf[len] = '\0';

                /* track fenced code blocks */
                if (strncmp(line_buf, "```", 3) == 0) {
                    in_code_block = !in_code_block;
                    p = eol ? eol + 1 : p + strlen(p);
                    line_idx++;
                    if (line_idx > scroll) y += 4;
                    continue;
                }

                if (line_idx >= scroll) {
                    uint32_t line_color = theme.text;
                    int scale = 1;
                    const char *display = line_buf;
                    int x_indent = pad + 8;

                    if (in_code_block) {
                        line_color = lumo_app_argb(0xFF, 0x66, 0xCC, 0x66);
                    } else if (line_buf[0] == '#' && line_buf[1] == '#' &&
                            line_buf[2] == '#' && line_buf[3] == '#') {
                        display = line_buf + 4;
                        while (*display == ' ') display++;
                        line_color = theme.accent;
                        scale = 1;
                    } else if (line_buf[0] == '#' && line_buf[1] == '#' &&
                            line_buf[2] == '#') {
                        display = line_buf + 3;
                        while (*display == ' ') display++;
                        line_color = theme.accent;
                        scale = 1;
                    } else if (line_buf[0] == '#' && line_buf[1] == '#') {
                        display = line_buf + 2;
                        while (*display == ' ') display++;
                        line_color = theme.accent;
                        scale = 2;
                    } else if (line_buf[0] == '#') {
                        display = line_buf + 1;
                        while (*display == ' ') display++;
                        line_color = theme.accent;
                        scale = 2;
                    } else if ((line_buf[0] == '-' || line_buf[0] == '*') &&
                            line_buf[1] == ' ') {
                        /* bullet: indent and show dot */
                        display = line_buf + 2;
                        x_indent = pad + 16;
                        lumo_app_fill_rect(pixels, width, height,
                            pad + 10, y + 4, 3, 3, theme.text);
                    } else if (line_buf[0] == '>' && line_buf[1] == ' ') {
                        display = line_buf + 2;
                        line_color = theme.text_dim;
                        /* blockquote bar */
                        lumo_app_fill_rect(pixels, width, height,
                            pad + 4, y, 2, 12, theme.accent);
                        x_indent = pad + 16;
                    } else if (len == 0) {
                        p = eol ? eol + 1 : p + strlen(p);
                        line_idx++;
                        y += 6;
                        continue;
                    }

                    lumo_app_draw_text(pixels, width, height,
                        x_indent, y, scale, line_color, display);
                    y += (scale > 1) ? 18 : 12;
                }
                line_idx++;
                p = eol ? eol + 1 : p + strlen(p);
            }
            y += 8;
            lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
                theme.separator);
            y += 6;
        }

        /* file list */
        int row_h = 40;
        int scroll = gh.scroll_offset;
        int max_visible = ((int)height - y - 20) / row_h;
        if (max_visible < 1) max_visible = 1;

        for (int i = scroll; i < gh.file_count && i < scroll + max_visible;
                i++) {
            struct gh_file *f = &gh.files[i];
            struct lumo_rect row = {pad, y, col_w, row_h - 4};
            bool is_dir = (strcmp(f->type, "dir") == 0);

            lumo_app_fill_rounded_rect(pixels, width, height, &row,
                8, theme.card_bg);

            /* icon: folder or file */
            lumo_app_draw_text(pixels, width, height,
                row.x + 8, row.y + 8, 2,
                is_dir ? theme.accent
                    : lumo_app_argb(0xFF, 0x77, 0x21, 0x6F),
                is_dir ? ">" : "*");

            /* name */
            lumo_app_draw_text(pixels, width, height,
                row.x + 28, row.y + 8, 2, theme.text, f->name);

            /* size for files */
            if (!is_dir && f->size > 0) {
                if (f->size < 1024)
                    snprintf(buf, sizeof(buf), "%dB", f->size);
                else if (f->size < 1024 * 1024)
                    snprintf(buf, sizeof(buf), "%dK", f->size / 1024);
                else
                    snprintf(buf, sizeof(buf), "%dM",
                        f->size / (1024 * 1024));
                lumo_app_draw_text(pixels, width, height,
                    row.x + row.width - 50, row.y + 8, 1,
                    theme.text_dim, buf);
            }

            y += row_h;
        }
        return;
    }

    /* ── view 3: file content viewer with syntax highlighting ─── */
    if (gh.view == 3 && gh.file_content != NULL) {
        /* back button */
        {
            struct lumo_rect back_btn = {pad, y, 60, 24};
            lumo_app_fill_rounded_rect(pixels, width, height, &back_btn,
                8, theme.card_bg);
            lumo_app_draw_text_centered(pixels, width, height, &back_btn,
                2, theme.accent, "< BACK");
        }

        /* file name */
        {
            const char *base = strrchr(gh.file_name, '/');
            const char *display = base ? base + 1 : gh.file_name;
            lumo_app_draw_text(pixels, width, height, pad + 70, y, 2,
                theme.text, display);
        }
        y += 24;
        lumo_app_fill_rect(pixels, width, height, pad, y, col_w, 1,
            theme.separator);
        y += 6;

        /* determine file extension for syntax highlighting */
        const char *ext = strrchr(gh.file_name, '.');
        bool is_md = ext && (strcmp(ext, ".md") == 0 ||
            strcmp(ext, ".MD") == 0 || strcmp(ext, ".markdown") == 0);

        int line_h = 14;
        int max_y = (int)height - 20;
        int line_num = 0;
        int scroll = gh.content_scroll;
        const char *p = gh.file_content;

        if (is_md) {
            /* ── markdown rendered view (no line numbers) ─────── */
            bool in_code_block = false;
            while (*p && y < max_y) {
                const char *eol = strchr(p, '\n');
                int len = eol ? (int)(eol - p) : (int)strlen(p);
                int max_chars = (col_w - 24) / 6;
                if (len > max_chars) len = max_chars;

                char line_buf[512];
                if (len > (int)sizeof(line_buf) - 1)
                    len = (int)sizeof(line_buf) - 1;
                memcpy(line_buf, p, (size_t)len);
                line_buf[len] = '\0';

                /* fenced code blocks */
                if (strncmp(line_buf, "```", 3) == 0) {
                    in_code_block = !in_code_block;
                    p = eol ? eol + 1 : p + strlen(p);
                    line_num++;
                    if (line_num > scroll) y += 4;
                    continue;
                }

                if (line_num >= scroll) {
                    uint32_t line_color = theme.text;
                    int scale = 1;
                    const char *display = line_buf;
                    int x_indent = pad + 4;

                    if (in_code_block) {
                        line_color = lumo_app_argb(0xFF, 0x66, 0xCC, 0x66);
                    } else if (line_buf[0] == '#' && line_buf[1] == '#' &&
                            line_buf[2] == '#' && line_buf[3] == '#') {
                        display = line_buf + 4;
                        while (*display == ' ') display++;
                        line_color = theme.accent;
                        scale = 1;
                    } else if (line_buf[0] == '#' && line_buf[1] == '#' &&
                            line_buf[2] == '#') {
                        display = line_buf + 3;
                        while (*display == ' ') display++;
                        line_color = theme.accent;
                        scale = 1;
                    } else if (line_buf[0] == '#' && line_buf[1] == '#') {
                        display = line_buf + 2;
                        while (*display == ' ') display++;
                        line_color = theme.accent;
                        scale = 2;
                    } else if (line_buf[0] == '#') {
                        display = line_buf + 1;
                        while (*display == ' ') display++;
                        line_color = theme.accent;
                        scale = 2;
                    } else if ((line_buf[0] == '-' || line_buf[0] == '*') &&
                            line_buf[1] == ' ') {
                        display = line_buf + 2;
                        x_indent = pad + 16;
                        lumo_app_fill_rect(pixels, width, height,
                            pad + 10, y + 4, 3, 3, theme.text);
                    } else if (line_buf[0] == '>' && line_buf[1] == ' ') {
                        display = line_buf + 2;
                        line_color = theme.text_dim;
                        lumo_app_fill_rect(pixels, width, height,
                            pad + 4, y, 2, 12, theme.accent);
                        x_indent = pad + 16;
                    } else if (len == 0) {
                        p = eol ? eol + 1 : p + strlen(p);
                        line_num++;
                        y += 6;
                        continue;
                    }

                    lumo_app_draw_text(pixels, width, height,
                        x_indent, y, scale, line_color, display);
                    y += (scale > 1) ? 18 : 12;
                }
                line_num++;
                p = eol ? eol + 1 : p + strlen(p);
            }
        } else {
            /* ── source code view with syntax highlighting ────── */
            uint32_t color_keyword = lumo_app_argb(0xFF, 0xCC, 0x77, 0xFF);
            uint32_t color_string = lumo_app_argb(0xFF, 0x66, 0xCC, 0x66);
            uint32_t color_comment = lumo_app_argb(0xFF, 0x66, 0x88, 0x88);
            uint32_t color_number = lumo_app_argb(0xFF, 0xFF, 0xAA, 0x44);
            uint32_t color_normal = theme.text;

            while (*p && y < max_y) {
                const char *eol = strchr(p, '\n');
                int len = eol ? (int)(eol - p) : (int)strlen(p);
                int max_chars = (col_w - 36) / 6;
                if (len > max_chars) len = max_chars;

                if (line_num >= scroll) {
                    char line_buf[512];
                    if (len > (int)sizeof(line_buf) - 1)
                        len = (int)sizeof(line_buf) - 1;
                    memcpy(line_buf, p, (size_t)len);
                    line_buf[len] = '\0';

                    uint32_t line_color = color_normal;

                    if (line_buf[0] == '/' && line_buf[1] == '/') {
                        line_color = color_comment;
                    } else if (line_buf[0] == '#' && ext &&
                            (strcmp(ext, ".py") == 0 ||
                             strcmp(ext, ".sh") == 0 ||
                             strcmp(ext, ".yml") == 0 ||
                             strcmp(ext, ".yaml") == 0)) {
                        line_color = color_comment;
                    } else if (strstr(line_buf, "/*") ||
                            strstr(line_buf, "*/")) {
                        line_color = color_comment;
                    } else {
                        int start = 0;
                        while (line_buf[start] == ' ' ||
                                line_buf[start] == '\t') start++;
                        if (start < len &&
                                syntax_color_for_token(line_buf, start,
                                    ext) == 0)
                            line_color = color_keyword;
                        else if (strchr(line_buf, '"') ||
                                strchr(line_buf, '\''))
                            line_color = color_string;
                        else if (start < len &&
                                line_buf[start] >= '0' &&
                                line_buf[start] <= '9')
                            line_color = color_number;
                    }

                    snprintf(buf, sizeof(buf), "%3d", line_num + 1);
                    lumo_app_draw_text(pixels, width, height,
                        pad, y, 1,
                        lumo_app_argb(0x60, 0x80, 0x80, 0x80), buf);

                    lumo_app_draw_text(pixels, width, height,
                        pad + 28, y, 1, line_color, line_buf);
                    y += line_h;
                }
                line_num++;
                p = eol ? eol + 1 : p + strlen(p);
            }
        }

        /* scroll indicator */
        snprintf(buf, sizeof(buf), "%s  LINE %d  SIZE %zuB",
            is_md ? "MARKDOWN" : "SOURCE",
            scroll + 1, gh.file_content_len);
        lumo_app_draw_text(pixels, width, height,
            pad, (int)height - 16, 1, theme.text_dim, buf);
        return;
    }
}

static void gh_fetch_readme(const char *repo) {
    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint), "/repos/%s/readme", repo);

    char *json = github_api_get(gh.token, endpoint);
    if (!json) return;

    char url_buf[1024];
    if (json_get_string(json, "download_url", url_buf, sizeof(url_buf))) {
        free(json);
        CURL *curl = curl_easy_init();
        if (!curl) return;

        struct curl_buf buf = {0};
        buf.data = malloc(32768);
        buf.capacity = 32768;
        buf.data[0] = '\0';

        curl_easy_setopt(curl, CURLOPT_URL, url_buf);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lumo-OS/0.0.77");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK && buf.size > 0) {
            if (gh.readme_content) free(gh.readme_content);
            gh.readme_content = buf.data;
            gh.readme_len = buf.size;
            gh.readme_loaded = true;
        } else {
            free(buf.data);
        }
    } else {
        free(json);
    }
}

/* ── file tree fetch thread ────────────────────────────────────── */

static void *gh_fetch_files_thread(void *arg) {
    (void)arg;
    gh.loading = true;
    snprintf(gh.status_msg, sizeof(gh.status_msg), "LOADING FILES...");
    gh_fetch_files(gh.current_repo, gh.current_path);
    /* fetch README when at repo root */
    if (gh.current_path[0] == '\0' && !gh.readme_loaded) {
        snprintf(gh.status_msg, sizeof(gh.status_msg), "LOADING README...");
        gh_fetch_readme(gh.current_repo);
    }
    gh.loading = false;
    return NULL;
}

static void *gh_fetch_content_thread(void *arg) {
    (void)arg;
    gh.loading = true;
    snprintf(gh.status_msg, sizeof(gh.status_msg), "LOADING...");
    gh_fetch_file_content(gh.current_repo, gh.file_name);
    gh.view = 3;
    gh.content_scroll = 0;
    gh.loading = false;
    return NULL;
}

/* ── touch handling ────────────────────────────────────────────── */

void lumo_app_github_handle_tap(int btn) {
    if (gh.loading) return;

    if (gh.view == 0) {
        /* retry auth */
        gh_load_token();
        if (gh.token[0] != '\0') {
            pthread_t tid;
            pthread_create(&tid, NULL, gh_fetch_thread, NULL);
            pthread_detach(tid);
        }
        return;
    }

    if (gh.view == 1 && btn >= 100) {
        /* repo tapped — open file tree + README */
        int idx = btn - 100;
        if (idx >= 0 && idx < gh.repo_count) {
            snprintf(gh.current_repo, sizeof(gh.current_repo),
                "%s", gh.repos[idx].full_name);
            gh.current_path[0] = '\0';
            gh.scroll_offset = 0;
            gh.view = 2;
            /* reset README */
            if (gh.readme_content) { free(gh.readme_content); gh.readme_content = NULL; }
            gh.readme_len = 0;
            gh.readme_loaded = false;
            pthread_t tid;
            pthread_create(&tid, NULL, gh_fetch_files_thread, NULL);
            pthread_detach(tid);
        }
        return;
    }

    if (gh.view == 1 && btn == 50) {
        /* back to login — shouldn't normally happen */
        return;
    }

    if (gh.view == 2 && btn == 1) {
        /* back button — go up one directory or back to repos */
        char *last_slash = strrchr(gh.current_path, '/');
        if (last_slash != NULL) {
            *last_slash = '\0';
            gh.scroll_offset = 0;
            pthread_t tid;
            pthread_create(&tid, NULL, gh_fetch_files_thread, NULL);
            pthread_detach(tid);
        } else if (gh.current_path[0] != '\0') {
            gh.current_path[0] = '\0';
            gh.scroll_offset = 0;
            pthread_t tid;
            pthread_create(&tid, NULL, gh_fetch_files_thread, NULL);
            pthread_detach(tid);
        } else {
            gh.view = 1;
            gh.scroll_offset = 0;
        }
        return;
    }

    if (gh.view == 2 && btn >= 100) {
        /* file/dir tapped */
        int idx = btn - 100;
        if (idx >= 0 && idx < gh.file_count) {
            struct gh_file *f = &gh.files[idx];
            if (strcmp(f->type, "dir") == 0) {
                snprintf(gh.current_path, sizeof(gh.current_path),
                    "%s", f->path);
                gh.scroll_offset = 0;
                pthread_t tid;
                pthread_create(&tid, NULL, gh_fetch_files_thread, NULL);
                pthread_detach(tid);
            } else {
                /* open file content */
                snprintf(gh.file_name, sizeof(gh.file_name),
                    "%s", f->path);
                pthread_t tid;
                pthread_create(&tid, NULL, gh_fetch_content_thread, NULL);
                pthread_detach(tid);
            }
        }
        return;
    }

    if (gh.view == 3 && btn == 1) {
        /* back from content viewer */
        if (gh.file_content) { free(gh.file_content); gh.file_content = NULL; }
        gh.file_content_len = 0;
        gh.view = 2;
        gh.content_scroll = 0;
        return;
    }
}

void lumo_app_github_scroll(int direction) {
    if (gh.view == 3) {
        gh.content_scroll += direction * 3;
        if (gh.content_scroll < 0) gh.content_scroll = 0;
    } else {
        gh.scroll_offset += direction;
        if (gh.scroll_offset < 0) gh.scroll_offset = 0;
    }
}

int lumo_app_github_button_at(
    uint32_t width, uint32_t height, double x, double y
) {
    (void)width;
    int header_h = 84;
    int row_h = 56;

    /* back button area (top-right or top-left) */
    if (y < (double)header_h && x < 80.0 && (gh.view == 2 || gh.view == 3))
        return 1;

    /* repo/file rows */
    if ((gh.view == 1 || gh.view == 2) && y > (double)header_h) {
        int row = (int)(y - header_h) / row_h;
        return 100 + row + gh.scroll_offset;
    }

    /* content view — tap for back */
    if (gh.view == 3)
        return 1;

    /* login — retry */
    if (gh.view == 0)
        return 0;

    (void)height;
    return -1;
}
