#include "lumo/app.h"

#include <string.h>

struct lumo_app_definition {
    enum lumo_app_id app_id;
    const char *name;
    const char *title;
    const char *subtitle;
    uint32_t accent_argb;
};

static const struct lumo_app_definition lumo_apps[] = {
    {LUMO_APP_PHONE, "phone", "Phone", "Favorites, recents, and keypad",
        0xFF4F7DFFu},
    {LUMO_APP_MESSAGES, "messages", "Messages", "Conversations that stay close",
        0xFF30C48Du},
    {LUMO_APP_BROWSER, "browser", "Browser", "Fast tabs and saved starts",
        0xFF4DD0FFu},
    {LUMO_APP_CAMERA, "camera", "Camera", "Capture in one tap",
        0xFFFFB84Du},
    {LUMO_APP_MAPS, "maps", "Maps", "Places, routes, and nearby pins",
        0xFF62D26Fu},
    {LUMO_APP_MUSIC, "music", "Music", "Play now and keep moving",
        0xFFF56A8Au},
    {LUMO_APP_PHOTOS, "photos", "Photos", "Moments, albums, and stories",
        0xFFB27DFFu},
    {LUMO_APP_VIDEOS, "videos", "Videos", "Resume, queue, and continue",
        0xFFFF7E54u},
    {LUMO_APP_CLOCK, "clock", "Clock", "Time, alarms, and timers",
        0xFFFFD166u},
    {LUMO_APP_NOTES, "notes", "Notes", "Quick capture and checklists",
        0xFF7BDFF2u},
    {LUMO_APP_FILES, "files", "Files", "Recent items and storage places",
        0xFF7BA3FFu},
    {LUMO_APP_SETTINGS, "settings", "Settings", "Display, sound, and privacy",
        0xFF8FA5BAu},
    {LUMO_APP_SYSMON, "sysmon", "System Monitor", "CPU, GPU, RAM, storage stats",
        0xFF44CC88u},
    {LUMO_APP_GITHUB, "github", "GitHub", "Repositories, issues, and profile",
        0xFF6E5494u},
    {LUMO_APP_CALCULATOR, "calculator", "Calculator", "Quick math at your fingertips",
        0xFFFF8844u},
    {LUMO_APP_CALENDAR, "calendar", "Calendar", "Dates, events, and reminders",
        0xFF44AAFFu},
    {LUMO_APP_WEATHER, "weather", "Weather", "Forecast and conditions",
        0xFF66CCFFu},
    {LUMO_APP_CONTACTS, "contacts", "Contacts", "People and numbers",
        0xFF44CC88u},
    {LUMO_APP_RECORDER, "recorder", "Recorder", "Voice memos and audio",
        0xFFFF5566u},
    {LUMO_APP_TASKS, "tasks", "Tasks", "To-do lists and reminders",
        0xFFAADD44u},
    {LUMO_APP_DOWNLOADS, "downloads", "Downloads", "Saved files and transfers",
        0xFF7799FFu},
    {LUMO_APP_PACKAGE, "package", "Packages", "Installed software and updates",
        0xFFBB77FFu},
    {LUMO_APP_SYSLOG, "syslog", "System Log", "Journal entries and diagnostics",
        0xFF88AABBu},
    {LUMO_APP_PDF, "pdf", "PDF Reader", "View documents and ebooks",
        0xFFDD4444u},
    {LUMO_APP_SETUP, "setup", "Setup Wizard", "First-boot configuration",
        0xFFFF8844u},
};

size_t lumo_app_count(void) {
    return sizeof(lumo_apps) / sizeof(lumo_apps[0]);
}

static const struct lumo_app_definition *lumo_app_definition_for(
    enum lumo_app_id app_id
) {
    for (size_t i = 0; i < lumo_app_count(); i++) {
        if (lumo_apps[i].app_id == app_id) {
            return &lumo_apps[i];
        }
    }

    return NULL;
}

const char *lumo_app_id_name(enum lumo_app_id app_id) {
    const struct lumo_app_definition *app = lumo_app_definition_for(app_id);

    return app != NULL ? app->name : NULL;
}

bool lumo_app_id_parse(const char *value, enum lumo_app_id *app_id) {
    if (value == NULL || app_id == NULL) {
        return false;
    }

    for (size_t i = 0; i < lumo_app_count(); i++) {
        if (strcmp(lumo_apps[i].name, value) == 0) {
            *app_id = lumo_apps[i].app_id;
            return true;
        }
    }

    return false;
}

bool lumo_app_id_for_launcher_tile(uint32_t tile_index, enum lumo_app_id *app_id) {
    if (app_id == NULL || tile_index >= lumo_app_count()) {
        return false;
    }

    *app_id = lumo_apps[tile_index].app_id;
    return true;
}

const char *lumo_app_title(enum lumo_app_id app_id) {
    const struct lumo_app_definition *app = lumo_app_definition_for(app_id);

    return app != NULL ? app->title : "Lumo App";
}

const char *lumo_app_subtitle(enum lumo_app_id app_id) {
    const struct lumo_app_definition *app = lumo_app_definition_for(app_id);

    return app != NULL ? app->subtitle : "Touch-first native client";
}

uint32_t lumo_app_accent_argb(enum lumo_app_id app_id) {
    const struct lumo_app_definition *app = lumo_app_definition_for(app_id);

    return app != NULL ? app->accent_argb : 0xFF69D1FFu;
}

bool lumo_app_wants_osk(enum lumo_app_id app_id, int note_editing) {
    switch (app_id) {
    case LUMO_APP_MESSAGES:
    case LUMO_APP_SETUP:
        return true;
    case LUMO_APP_NOTES:
    case LUMO_APP_MAPS:
    case LUMO_APP_BROWSER:
    case LUMO_APP_CONTACTS:
    case LUMO_APP_TASKS:
        return note_editing >= 0;
    case LUMO_APP_PHONE:
    case LUMO_APP_CAMERA:
    case LUMO_APP_MUSIC:
    case LUMO_APP_PHOTOS:
    case LUMO_APP_VIDEOS:
    case LUMO_APP_CLOCK:
    case LUMO_APP_FILES:
    case LUMO_APP_SETTINGS:
    default:
        return false;
    }
}

bool lumo_app_close_rect(
    uint32_t width,
    uint32_t height,
    struct lumo_rect *rect
) {
    uint32_t size;

    if (rect == NULL || width == 0 || height == 0) {
        return false;
    }

    size = width / 18;
    if (size < 44) {
        size = 44;
    } else if (size > 60) {
        size = 60;
    }

    rect->width = (int)size;
    rect->height = (int)size;
    rect->x = (int)width - (int)size - 28;
    rect->y = 22;
    return true;
}
