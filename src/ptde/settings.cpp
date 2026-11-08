#include "settings.h"

#include "extension.h"
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

// The INI beside the DLL. Prepare to Co-op Edition's build reads PTDE-Coop-FPS-Unlock.ini first and falls back to
// PTDE-FPS-Unlock.ini; the FPS Unlock on its own reads only PTDE-FPS-Unlock.ini. Names match in any case (the
// directory is searched, so a case-sensitive file system works too). When neither exists the first name is
// returned, which is the one a fresh install is expected to have.
void ini_path(const wchar_t* dll_path, wchar_t* full, size_t size) {
    wchar_t dir[MAX_PATH];
    lstrcpynW(dir, dll_path, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) {
        slash[1] = 0;
    }
    static const wchar_t* const kCoop[] = {L"PTDE-Coop-FPS-Unlock.ini", L"PTDE-FPS-Unlock.ini"};
    static const wchar_t* const kPlain[] = {L"PTDE-FPS-Unlock.ini"};
    const bool coop = ptde_extension() != nullptr;
    const wchar_t* const* names = coop ? kCoop : kPlain;
    const size_t count = coop ? 2 : 1;

    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, _TRUNCATE, L"%s*.ini", dir);
    wchar_t found[2][MAX_PATH] = {};
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                if (_wcsicmp(data.cFileName, names[i]) == 0) {
                    lstrcpynW(found[i], data.cFileName, MAX_PATH);
                }
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    for (size_t i = 0; i < count; ++i) {
        if (found[i][0] != 0) {
            _snwprintf_s(full, size, _TRUNCATE, L"%s%s", dir, found[i]);
            if (i == 0 && count > 1 && found[1][0] != 0) {
                LOG_INFO("Settings::read - both %ls and %ls exist; only the first is read", found[0], found[1]);
            }
            return;
        }
    }
    _snwprintf_s(full, size, _TRUNCATE, L"%s%s", dir, names[0]);
}

}  // namespace

bool settings_parse_bool(const std::string& value) { return parse_bool(value); }

bool settings_read_keys(const wchar_t* dll_path,
                        const std::function<void(const std::string& key, const std::string& value)>& fn) {
    wchar_t full[MAX_PATH];
    ini_path(dll_path, full, MAX_PATH);
    std::ifstream in(full);
    if (!in) {
        return false;
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
        fn(lower(trim(line.substr(0, eq))), trim(line.substr(eq + 1)));
    }
    return true;
}

Settings settings_load(const wchar_t* dll_path) {
    Settings s;
    wchar_t full[MAX_PATH];
    ini_path(dll_path, full, MAX_PATH);
    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, full, -1, narrow, MAX_PATH, nullptr, nullptr);
    LOG_INFO("Settings::read - reading INI from %s", narrow);

    const bool read = settings_read_keys(dll_path, [&s](const std::string& key, const std::string& val) {
        if (key == "fpsunlock") {
            s.fps_unlock = parse_bool(val);
        } else if (key == "maxfps" || key == "targetfps") {
            s.target_fps = parse_max_fps(val, s.target_fps);
        } else if (key == "fpslimitpoint") {
            const std::string point = lower(val);
            if (point == "present") {
                s.fps_limit_early = false;
            } else if (point == "early") {
                s.fps_limit_early = true;
            } else {
                LOG_INFO("FPSLimitPoint \"%s\" is not early or present; using early", val.c_str());
                s.fps_limit_early = true;
            }
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
        } else if (key == "fixvelocity") {
            s.fix_velocity = parse_bool(val);
        } else if (key == "fixscriptmemory") {
            s.fix_script_memory = parse_bool(val);
        } else if (key == "fixcamera") {
            s.fix_camera = parse_bool(val);
        } else if (key == "fixstepdown" || key == "fixladder") {
            s.fix_step_down = parse_bool(val);
        } else if (key == "fixlockonturn") {
            s.fix_lock_on_turn = parse_bool(val);
        } else if (key == "fixfootik") {
            s.fix_foot_ik = parse_bool(val);
        } else if (key == "fixghosts") {
            s.fix_ghosts = parse_bool(val);
        } else if (key == "fixsfxspawnrate") {
            s.fix_sfx_spawn_rate = parse_bool(val);
        } else if (key == "fixstaminatick") {
            s.fix_stamina_tick = parse_bool(val);
        } else if (key == "fixslidecontact") {
            s.fix_slide_contact = parse_bool(val);
        } else if (key == "fixslidesteer") {
            s.fix_slide_steer = parse_bool(val);
        } else if (key == "fixcutscenecamera") {
            s.fix_cutscene_camera = parse_bool(val);
        } else if (key == "fixsoulcounter") {
            s.fix_soul_counter = parse_bool(val);
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
        } else if (key == "bullettrace") {
            s.bullet_trace = parse_bool(val);
        } else if (key == "profile") {
            s.profile = parse_bool(val);
        } else if (key == "skippresentrepeat") {
            s.skip_present_repeat = parse_bool(val);
        } else if (key == "dxvk") {
            s.dxvk = parse_bool(val);
        } else if (key == "borderlessfullscreen") {
            s.borderless = parse_bool(val);
        } else if (key == "fixcontroller") {
            s.fix_controller = parse_bool(val);
        } else if (key == "fixaspect") {
            s.fix_aspect = parse_bool(val);
        } else if (key == "skipintro") {
            s.skip_intro = parse_bool(val);
        } else if (key == "survey") {
            s.survey_seconds = std::atoi(val.c_str());
        }
    });
    if (!read) LOG_ERROR("Settings::read - INI read failed, using defaults");
    return s;
}
