#pragma once

#include "ui_shell.h"

// Stable IDs are retained for existing app/service dispatch; hiding is UI-only.
enum AppID : uintptr_t {
    APP_NONE = 0, APP_SYSTEM = 1, APP_SETTINGS = 2, APP_WIFI = 3,
    APP_ABOUT = 4, APP_MAP = 5, APP_TOOLS = 6, APP_AUDIO = 7,
    APP_MUSIC = 8, APP_AI_VOICE = 9, APP_CAMERA = 10, APP_POWER = 11,
    APP_HEALTH = 12, APP_COLOR_TEST = 13, APP_TOUCH_DEBUG = 14
};

namespace minios_catalog {
inline const minios_shell::App (&home_apps())[5] {
    static const minios_shell::App apps[5] = {
        {LV_SYMBOL_CHARGE, "System", COLOR_ACCENT_BLUE, APP_SYSTEM},
        {LV_SYMBOL_WIFI, "WiFi", COLOR_ACCENT_GREEN, APP_WIFI},
        {LV_SYMBOL_GPS, "Map", COLOR_ACCENT_RED, APP_MAP},
        {LV_SYMBOL_IMAGE, "Camera", COLOR_ACCENT_PURPLE, APP_CAMERA},
        {LV_SYMBOL_POWER, "Power", COLOR_ACCENT_AMBER, APP_POWER}
    };
    return apps;
}
inline const minios_shell::App (&dock_apps())[3] {
    static const minios_shell::App apps[3] = {
        {LV_SYMBOL_AUDIO, "Music", COLOR_ACCENT_PURPLE, APP_MUSIC},
        {LV_SYMBOL_AUDIO, "AI Voice", COLOR_ACCENT_BLUE, APP_AI_VOICE},
        {LV_SYMBOL_SETTINGS, "Settings", COLOR_TEXT_SECONDARY, APP_SETTINGS}
    };
    return apps;
}
} // namespace minios_catalog
