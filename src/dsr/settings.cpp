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

// MaxFPS: a whole number, clamped to kMinMaxFps..kMaxMaxFps. Anything that is not a number keeps
// the default.
int parse_max_fps(const std::string& v, int fallback) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) {
        LOG_ERROR("Settings::read - MaxFPS \"%s\" is not a number, using %d", v.c_str(), fallback);
        return fallback;
    }
    const long clamped = n < kMinMaxFps ? kMinMaxFps : (n > kMaxMaxFps ? kMaxMaxFps : n);
    if (clamped != n) {
        LOG_INFO("Settings::read - MaxFPS %ld is outside %d to %d, using %ld", n, kMinMaxFps, kMaxMaxFps, clamped);
    }
    return static_cast<int>(clamped);
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
    _snwprintf_s(full, _TRUNCATE, L"%sDSR-FPS-Unlock.ini", path);

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
            s.target_fps = parse_max_fps(val, s.target_fps);
        } else if (key == "variableframetime") {
            s.variable_frame_time = parse_bool(val);
        } else if (key == "menuinputfilter") {
            s.menu_input_filter = parse_bool(val);
        } else if (key == "bonfirefix" || key == "bonfireunstick") {
            s.bonfire_unstick = parse_bool(val);
        } else if (key == "trace") {
            s.trace = parse_bool(val);
        } else if (key == "fixstepdown") {
            s.fix_step_down = parse_bool(val);
        } else if (key == "lockonptdespeed" || key == "cameraptdespeed") {
            s.camera_ptde_speed = parse_bool(val);
        } else if (key == "fixcamera") {
            s.fix_camera = parse_bool(val);
        } else if (key == "watch") {
            s.watch = parse_bool(val);
        } else if (key == "inputlog") {
            s.input_log = parse_bool(val);
        } else if (key == "fixdpadhold") {
            s.fix_dpad_hold = parse_bool(val);
        } else if (key == "fixstaminatick") {
            s.fix_stamina_tick = parse_bool(val);
        } else if (key == "fixui") {
            s.fix_ui = parse_bool(val);
        } else if (key == "fixgraze") {
            s.fix_graze = parse_bool(val);
        } else if (key == "fixdamping") {
            s.fix_damping = parse_bool(val);
        } else if (key == "fixslide") {
            s.fix_slide = parse_bool(val);
        } else if (key == "fixjump") {
            s.fix_jump = parse_bool(val);
        } else if (key == "fixlockonturn") {
            s.fix_lock_on_turn = parse_bool(val);
        } else if (key == "fixghosts") {
            s.fix_ghosts = parse_bool(val);
        } else if (key == "fixtimers") {
            s.fix_timers = parse_bool(val);
        } else if (key == "fixsmoothing") {
            s.fix_smoothing = parse_bool(val);
        } else if (key == "fixvelocity") {
            s.fix_velocity = parse_bool(val);
        } else if (key == "fixmovedt") {
            s.fix_move_dt = parse_bool(val);
        }
    }
    return s;
}
