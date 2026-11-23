#include "fpsu_buildcfg.h"
#include "bullettrace.h"
#include "controller.h"
#include "d3d.h"
#include "deep.h"
#include "diag.h"
#include "dsfix.h"
#include "extension.h"
#include "ffx.h"
#include "rendermath.h"
#include "rwlock.h"
#include "mutex.h"
#include "menuscale.h"
#include "texpreload.h"
#include "frame.h"
#include "gauge.h"
#include "intro.h"
#include "laa.h"
#include "framekick.h"
#include "focus.h"
#include "drivetype.h"
#include "inputcache.h"
#include "log.h"
#include "patches.h"
#include "settingsmenu.h"
#include "settings.h"
#include "steamguard.h"
#include "trace.h"
#include "fpsu_version.h"
#include "window.h"
#include "cursor.h"
#include "xinput_proxy.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwchar>

namespace {

HMODULE g_self = nullptr;

bool filename_is_game(const wchar_t* path) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name = slash ? slash + 1 : path;
    return _wcsicmp(name, L"DARKSOULS.exe") == 0;
}

// Runs on its own thread so nothing here happens under the loader lock.
DWORD WINAPI worker(void*) {
    wchar_t dll_path[MAX_PATH];
    GetModuleFileNameW(g_self, dll_path, MAX_PATH);
    log_init(dll_path, L"PTDE-FPS-Unlock.log");
    xinput_log_target();
    diag_install(dll_path, L"PTDE-FPS-Unlock-crash.dmp");
    LOG_INFO("DS1 PTDE FPS Unlock v%s%s", ptde_fps_unlock_version(), FPSU_DIAGNOSTICS ? " (diagnostics build)" : "");
    const PtdeExtension* ext = ptde_extension();
    if (ext) LOG_INFO("Built together with %s", ext->name);

    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    char exe_narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, exe_narrow, MAX_PATH, nullptr, nullptr);
    LOG_INFO("EXE: %s", exe_narrow);

    Settings settings = settings_load(dll_path);
#if !FPSU_DIAGNOSTICS
    if (settings.trace || settings.bullet_trace || settings.sfx_watch || settings.watch[0] || settings.profile ||
        settings.input_log) {
        LOG_INFO("Trace, BulletTrace, SfxWatch, Watch, Profile and InputLog are diagnostics, which this release build "
                 "does not include. Ignored.");
        settings.trace = settings.bullet_trace = settings.profile = settings.input_log = false;
        settings.sfx_watch = 0;
        settings.watch[0] = 0;
    }
#endif
    trace_set_dir(dll_path);
    deep_set_dir(dll_path);
    gauge_set_dir(dll_path);
    LOG_INFO("FPSUnlock: %s", settings.fps_unlock ? "true" : "false");
    LOG_INFO("MaxFPS: %d", settings.target_fps);
    LOG_INFO("FPSLimitPoint: %s", settings.fps_limit_early ? "early" : "present");
    LOG_INFO("DXVK: %s", settings.dxvk ? "true" : "false");
    LOG_INFO("FixAspect: %s", settings.fix_aspect ? "true" : "false");
    dpi_aware_log(settings.fps_unlock && settings.dpi_aware);

    // FPS Unlock Settings first, so that a bundled mod can add its page when it starts.
    if (settings.fps_unlock && settings.settings_menu) {
        wchar_t ini[MAX_PATH];
        settings_ini_path(dll_path, ini, MAX_PATH);
        settingsmenu_install(ini, settings.settings_menu_key);
    }
    // A mod built into this DLL (see extension.h) and the start-up logos do not depend on the FPS unlock.
    if (ext && ext->start) ext->start(g_self);
    intro_install(settings.skip_intro);
    laa_ensure(settings.large_address_aware);
    framekick_install(settings.fix_framerate_kick);
    drivetype_install(settings.cache_drive_type, settings.profile);
    inputcache_configure(settings.cache_input_reads);

    wchar_t game_dir[MAX_PATH];
    lstrcpynW(game_dir, exe_path, MAX_PATH);
    wchar_t* slash = wcsrchr(game_dir, L'\\');
    if (slash) {
        slash[1] = 0;
    }
    const DsfixInfo dsfix = dsfix_probe(game_dir);
    if (dsfix.present) {
        LOG_INFO("DSfix detected (DSfix.ini %s%s). unlockFPS=%d FPSlimit=%d", dsfix.ini_found ? "found" : "not found",
                 dsfix.v3 ? ", DSfix 3.0's" : "", dsfix.unlock_fps ? 1 : 0, dsfix.fps_limit);
        if (dsfix.skip_intro && settings.skip_intro) {
            LOG_INFO("DSfix's skipIntro is on too: it only acts while the logos show, and with SkipIntro they never do");
        }
        if (dsfix.unlock_fps && settings.fps_unlock) {
            LOG_INFO("DSfix's unlockFPS is on as well. This mod drives the game's time step and frame rate; DSfix's "
                     "FPSlimit (%d) can still hold the frame rate below MaxFPS while it is on (unlockFPS 0 avoids that)",
                     dsfix.fps_limit);
        }
    } else {
        LOG_INFO("DSfix not detected");
    }
    dsfix_set_yield(settings.yield_to_dsfix);
    // The PC settings screen's Frequency: every rate the display offers (FullscreenRefreshRate decides which is used;
    // DSfix's fullscreenHz does under YieldToDSFix, and -1 keeps the game's own).
    if (settings.fps_unlock && settings.fullscreen_refresh >= 0 && !dsfix_yields()) d3d_refresh_list_patch();
    if (dsfix_yields()) {
        LOG_INFO("YieldToDSFix: DSfix decides the picture where it has settings of its own: RenderResolution, "
                 "Supersampling, DepthOfFieldStrength, DepthOfFieldResolution, EffectsResolution, WaterResolution, "
                 "HighPrecisionColor and AnisotropicFiltering are off (DSfix's renderWidth/renderHeight, "
                 "presentWidth/presentHeight, dof settings and filteringOverride apply), FullscreenRefreshRate is off "
                 "(DSfix's fullscreenHz), and the HUD and menus are the game's for DSfix's HUD mod: UIScale, "
                 "UIScaleWorld, UIScaleText, UIScaleMenus, FixBossBarOverlap, FixAspect and FixAspectUI are off (a "
                 "screen other than 16:9 keeps the game's 16:9 picture)");
    } else if (settings.yield_to_dsfix) {
        LOG_INFO("YieldToDSFix: DSfix is not loaded (no DINPUT8.dll with DSfix.ini beside the game); nothing to yield to");
    }
    // DSfix's resolution needs the game's own anti-aliasing off: with it on the game draws the scene through 1024x720
    // multisampled surfaces DSfix does not enlarge, and the 3D picture fills only a corner of the screen (seen
    // 2026-11-14 with DSfix 3840x2160, Direct3D 9 and DXVK alike; the multisample type alone is not the cause).
    if (dsfix_changes_resolution() && (dsfix_yields() || settings.render_width == 0 || settings.render_height == 0)) {
        wchar_t local[MAX_PATH] = {}, game_ini[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
            _snwprintf_s(game_ini, _TRUNCATE, L"%s\\NBGI\\DarkSouls\\DarkSouls.ini", local);
            if (GetPrivateProfileIntW(L"DisplaySettingFilter", L"Antialiasing", 0, game_ini) != 0) {
                LOG_ERROR("DSfix makes the resolution, but the game's own anti-aliasing is on (Antialiasing = 1 in "
                          "DarkSouls.ini): DSfix needs it off, or the 3D picture fills only a corner of the screen. "
                          "Turn it off in the game's options (or set Antialiasing = 0 under [DisplaySettingFilter]).");
            }
        }
    }

    // FPSUnlock is the master switch (the refresh-rate override in d3d.cpp checks it too).
    if (!settings.fps_unlock) {
        LOG_INFO("FPSUnlock is false. The game is unchanged%s%s.",
                 settings.skip_intro ? ", except the start-up notice and logos (SkipIntro)" : "",
                 !settings.fix_framerate_kick ? ""
                 : settings.skip_intro        ? " and the frame rate kick (FixFramerateKick)"
                                              : ", except the frame rate kick (FixFramerateKick)");
        // A diagnostic, so the original 30 FPS game can be compared with the unlocked one.
        if (settings.bullet_trace) {
            bullettrace_install();
        }
        return 0;
    }
    focus_install();
    if (patches_probe(settings)) {
        if (settings.fix_controller) {
            controller_install(settings);
        }
        // Prepare to Co-op hooks SteamAPI_RunCallbacks itself and asks the guard (steamguard.h), but only while the
        // co-op is on: with it off the guard hooks the game's own Steam imports, as on its own.
        const PtdeExtension* steam_ext = ptde_extension();
        const bool ext_hooks_steam =
            steam_ext && (!steam_ext->hooks_steam_imports || steam_ext->hooks_steam_imports());
        steamguard_start(settings.steam_pause_guard, !ext_hooks_steam);
        frame_install(settings);
        // Last: it looks for the library's exact debug prologues (see ffx.h).
        if (settings.fast_effects) {
            ffx_install();
        } else {
            LOG_INFO("FastEffects is false. The effects library keeps its debug checks.");
        }
        if (settings.fast_render_math || settings.fast_effects) {
            rendermath_install(settings.fast_render_math, settings.fast_effects);
            if (settings.fast_render_math && settings.render_math_relaxed) {
                rendermath_set_relaxed(true);
                LOG_INFO("RenderMathPrecision: relaxed (the render thread's matrix multiplies and multiply-adds round "
                         "in float)");
            }
        }
    }
    rwlock_log(settings.fps_unlock && settings.fast_read_locks);
    mutex_log(settings.fps_unlock && settings.fast_mutexes);
    if (settings.preload_textures) {
        texpreload_install();
    } else {
        LOG_INFO("PreloadTextures is false. Textures reach the graphics card at their first draw.");
    }
    if (settings.borderless != 0) {
        if (dsfix.borderless) {
            LOG_INFO("DSfix's own borderlessFullscreen is on; leaving the window to DSfix.");
        } else {
            borderless_start(settings.borderless == 2);
        }
    }
    if (settings.hide_mouse_cursor) {
        cursor_start();
    }
    return 0;
}

}  // namespace

// Kept as a literal: Prepare to Co-op's tools\make_ini.ps1 reads the version from this line.
const char* ptde_fps_unlock_version() { return "3.0.0"; }

namespace {
constexpr bool same_text(const char* a, const char* b) { return *a == *b && (*a == 0 || same_text(a + 1, b + 1)); }
}  // namespace
static_assert(same_text("3.0.0", FPSU_VERSION_STR), "ptde_fps_unlock_version() and fpsu_version.h disagree");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        if (!filename_is_game(exe_path)) {
            return TRUE;
        }
        // Before the game calls Direct3DCreate9: wraps it to override the fullscreen refresh rate.
        d3d_install_early(instance);
        // Before the game's entry point, while no thread can be in the game's locks (rwlock.h). Two keys of the INI,
        // read without the log (not open yet).
        {
            wchar_t dll_path[MAX_PATH];
            GetModuleFileNameW(instance, dll_path, MAX_PATH);
            bool fps_unlock = true, fast_read_locks = true, fast_read_locks_always = false, dpi_aware = true;
            bool fast_mutexes = true, fast_mutexes_always = false;
            settings_read_keys(dll_path, [&](const std::string& key, const std::string& value) {
                if (key == "fpsunlock") fps_unlock = settings_parse_bool(value);
                if (key == "fastreadlocks") {
                    fast_read_locks_always = _stricmp(value.c_str(), "always") == 0;
                    fast_read_locks = fast_read_locks_always || settings_parse_bool(value);
                }
                if (key == "fastmutexes") {
                    fast_mutexes_always = _stricmp(value.c_str(), "always") == 0;
                    fast_mutexes = fast_mutexes_always || settings_parse_bool(value);
                }
                if (key == "dpiaware") dpi_aware = settings_parse_bool(value);
            });
            if (fps_unlock && fast_read_locks) rwlock_install_early(reserved != nullptr, fast_read_locks_always);
            if (fps_unlock && fast_mutexes) mutex_install_early(reserved != nullptr, fast_mutexes_always);
            // Before the game makes its window (window.h).
            if (fps_unlock && dpi_aware) dpi_aware_early();
        }
        if (const PtdeExtension* ext = ptde_extension()) {
            if (ext->attach) ext->attach(instance);
        }
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        menuscale_detach();
        log_close();
    }
    return TRUE;
}
