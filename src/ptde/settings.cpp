#include "settings.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

std::string trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool parse_bool(const std::string& v) {
    const std::string s = lower(trim(v));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

}  // namespace

Settings settings_load(const wchar_t* dll_path) {
    Settings s;
    wchar_t path[MAX_PATH];
    lstrcpynW(path, dll_path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) {
        slash[1] = 0;
    }
    wchar_t full[MAX_PATH];
    _snwprintf_s(full, _TRUNCATE, L"%sPTDE-FPS-Unlock.ini", path);

    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, full, -1, narrow, MAX_PATH, nullptr, nullptr);
    LOG_INFO("Settings::read - reading INI from %s", narrow);

    std::ifstream in(full);
    if (!in) {
        LOG_ERROR("Settings::read - INI read failed, using defaults");
        return s;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "fpsunlock") {
            s.fps_unlock = parse_bool(val);
        } else if (key == "maxfps" || key == "targetfps") {
            s.target_fps = std::atoi(val.c_str());
        } else if (key == "variableframetime" || key == "driver") {
            s.driver = parse_bool(val);
        } else if (key == "vblankpatch") {
            s.vblank_patch = parse_bool(val);
        } else if (key == "fullscreenrefreshrate") {
            s.fullscreen_refresh = std::atoi(val.c_str());
        } else if (key == "fixslide") {
            s.fix_slide = parse_bool(val);
        } else if (key == "fixdamping") {
            s.fix_damping = parse_bool(val);
        } else if (key == "fixtimers") {
            s.fix_timers = parse_bool(val);
        } else if (key == "fixcamera") {
            s.fix_camera = parse_bool(val);
        } else if (key == "fixladder") {
            s.fix_ladder = parse_bool(val);
        } else if (key == "fixui") {
            s.fix_ui = parse_bool(val);
        } else if (key == "bonfireunstick") {
            s.bonfire_unstick = parse_bool(val);
        } else if (key == "inputlog") {
            s.input_log = parse_bool(val);
        } else if (key == "fixgraze") {
            s.fix_graze = parse_bool(val);
        } else if (key == "fixsmoothing") {
            s.fix_smoothing = parse_bool(val);
        } else if (key == "contentprobe") {
            s.content_probe = parse_bool(val);
        } else if (key == "watch") {
            std::strncpy(s.watch, val.c_str(), sizeof(s.watch) - 1);
        } else if (key == "trace") {
            s.trace = parse_bool(val);
        } else if (key == "profile") {
            s.profile = parse_bool(val);
        } else if (key == "skippresentrepeat") {
            s.skip_present_repeat = parse_bool(val);
        } else if (key == "borderlessfullscreen") {
            s.borderless = parse_bool(val);
        } else if (key == "survey") {
            s.survey_seconds = std::atoi(val.c_str());
        }
    }
    return s;
}
