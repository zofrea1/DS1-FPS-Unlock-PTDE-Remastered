#include "settings.h"

#include "extension.h"
#include "log.h"
#include "rwlock.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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

    // Phone* keys (any setting with "Phone" in front) are kept and applied after the rest when the phone profile is on.
    std::vector<std::pair<std::string, std::string>> phone_keys;
    std::function<void(const std::string&, const std::string&)> apply;
    apply = [&s, &phone_keys](const std::string& key, const std::string& val) {
        if (key == "phoneprofile") {
            const std::string v = lower(trim(val));
            s.phone_profile = (v == "true" || v == "on" || v == "1") ? 1 : (v == "false" || v == "off" || v == "0") ? 2 : 0;
            return;
        }
        if (key.size() > 5 && key.compare(0, 5, "phone") == 0) {
            phone_keys.emplace_back(key.substr(5), val);
            return;
        }
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
        } else if (key == "fixsfxactions") {
            s.fix_sfx_actions = parse_bool(val);
        } else if (key == "fixsfxdistortion") {
            s.fix_sfx_distortion = parse_bool(val);
        } else if (key == "fixsun") {
            s.fix_sun_with_dsfix = parse_bool(val);
        } else if (key == "hidemousecursor") {
            s.hide_mouse_cursor = parse_bool(val);
        } else if (key == "shadowbias") {
            s.shadow_bias = static_cast<float>(std::atof(val.c_str()));
        } else if (key == "anisotropicfiltering") {
            s.anisotropic_filtering = std::atoi(val.c_str());
        } else if (key == "renderscalepost") {
            s.render_scale_post = parse_bool(val);
        } else if (key == "renderresolution") {
            const std::string v = lower(trim(val));
            int w = 0, h = 0;
            if (v.empty() || v == "auto") {
                s.render_width = s.render_height = -1;
            } else if (v == "off" || v == "false" || v == "0") {
                s.render_width = s.render_height = 0;
            } else if (std::sscanf(v.c_str(), "%dx%d", &w, &h) == 2 || std::sscanf(v.c_str(), "%d %d", &w, &h) == 2) {
                s.render_width = w;
                s.render_height = h;
            }
        } else if (key == "supersampling") {
            const float f = static_cast<float>(std::atof(val.c_str()));
            s.supersampling = f < 1.0f ? 1.0f : (f > 2.0f ? 2.0f : f);
        } else if (key == "depthoffieldstrength") {
            // Percent, 0 to 200.
            const double pct = std::atof(val.c_str());
            s.dof_strength = static_cast<float>(pct <= 0 ? 0.0 : (pct >= 200 ? 2.0 : pct / 100.0));
        } else if (key == "depthoffieldresolution") {
            const std::string v = lower(trim(val));
            s.dof_half_resolution = !(v == "game" || v == "off" || v == "false" || v == "0");
        } else if (key == "effectsresolution") {
            const std::string v = lower(trim(val));
            s.effects_resolution = (v == "full") ? 2 : (v == "game" || v == "off" || v == "false" || v == "0") ? 0 : 3;
        } else if (key == "highprecisioncolor") {
            s.high_precision_color = parse_bool(val);
        } else if (key == "antialiasingsamples") {
            s.antialiasing_samples = std::atoi(val.c_str());
        } else if (key == "alphatocoverage") {
            s.alpha_to_coverage = parse_bool(val);
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
        } else if (key == "fixstickflick") {
            s.fix_stick_flick = parse_bool(val);
        } else if (key == "fixswitchback") {
            s.fix_switchback = parse_bool(val);
        } else if (key == "fixrolldirection") {
            s.fix_roll_direction = parse_bool(val);
        } else if (key == "fixspeffecttiming") {
            s.fix_speffect_timing = parse_bool(val);
        } else if (key == "fixbullethittiming") {
            s.fix_bullet_hit_timing = parse_bool(val);
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
        } else if (key == "sfxwatch") {
            s.sfx_watch = std::atoi(val.c_str());
        } else if (key == "profile") {
            s.profile = parse_bool(val);
        } else if (key == "profilespikes") {
            s.profile_spikes = parse_bool(val);
        } else if (key == "profiled3d") {
            s.profile_d3d = parse_bool(val);
        } else if (key == "profilestreams") {
            s.profile_streams = parse_bool(val);
        } else if (key == "profileimports") {
            s.profile_imports = parse_bool(val);
        } else if (key == "profilecapturems") {
            s.profile_capture_ms = std::atoi(val.c_str());
        } else if (key == "remotecontrol") {
            s.remote_control = parse_bool(val);
        } else if (key == "remoteshaderdumps") {
            s.remote_shader_dumps = parse_bool(val);
        } else if (key == "skippresentrepeat") {
            s.skip_present_repeat = parse_bool(val);
        } else if (key == "dxvk") {
            s.dxvk = parse_bool(val);
        } else if (key == "dxvkexclusivefullscreen") {
            const std::string v = lower(trim(val));
            s.dxvk_exclusive_fullscreen = v == "auto" || v.empty() ? 0 : parse_bool(v) ? 1 : 2;
        } else if (key == "dxvkpresentthroughdxgi") {
            s.dxvk_dxgi_present = parse_bool(val);
        } else if (key == "gammaramp") {
            const std::string v = lower(trim(val));
            s.gamma_ramp = v == "auto" || v.empty() ? 0 : parse_bool(v) ? 1 : 2;
        } else if (key == "asyncreadback") {
            s.async_readback = parse_bool(val);
        } else if (key == "filtersamplerstates") {
            s.filter_sampler_states = parse_bool(val);
        } else if (key == "filtershaderconstants") {
            s.filter_shader_constants = parse_bool(val);
        } else if (key == "reusesurfaces") {
            s.reuse_surfaces = parse_bool(val);
        } else if (key == "fastreadlocks") {
            s.fast_read_locks = parse_bool(val) || lower(trim(val)) == "always";
        } else if (key == "preloadtextures") {
            s.preload_textures = parse_bool(val);
        } else if (key == "cachedrivetype") {
            s.cache_drive_type = parse_bool(val);
        } else if (key == "fasteffects") {
            s.fast_effects = parse_bool(val);
        } else if (key == "fastrendermath") {
            s.fast_render_math = parse_bool(val);
        } else if (key == "dpiaware") {
            s.dpi_aware = parse_bool(val);
        } else if (key == "borderlessfullscreen") {
            s.borderless = parse_bool(val);
        } else if (key == "steampauseguard") {
            s.steam_pause_guard = parse_bool(val);
        } else if (key == "fixcontroller") {
            s.fix_controller = parse_bool(val);
        } else if (key == "fixaspect") {
            s.fix_aspect = parse_bool(val);
        } else if (key == "skipintro") {
            s.skip_intro = parse_bool(val);
        } else if (key == "fixframeratekick") {
            s.fix_framerate_kick = parse_bool(val);
        } else if (key == "survey") {
            s.survey_seconds = std::atoi(val.c_str());
        }
    };
    const bool read = settings_read_keys(dll_path, apply);
    if (!read) LOG_ERROR("Settings::read - INI read failed, using defaults");
    const char* cpu = nullptr;
    const bool translated = cpu_translated(&cpu);
    if (!phone_keys.empty() && (s.phone_profile == 1 || (s.phone_profile == 0 && translated))) {
        std::string list;
        for (const auto& kv : phone_keys) {
            apply(kv.first, kv.second);
            list += (list.empty() ? "" : ", ") + kv.first + " = " + trim(kv.second);
        }
        s.phone_applied = true;
        static bool logged = false;
        if (!logged) LOG_INFO("Phone profile (%s): %s", s.phone_profile == 1 ? "PhoneProfile = true" : cpu, list.c_str());
        logged = true;
    } else if (translated) {
        static int logged_off = 0;
        if (!logged_off++) LOG_INFO("Phone profile: off (PhoneProfile = false); the game runs translated for %s", cpu);
    }
    return s;
}
