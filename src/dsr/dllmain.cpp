#include "bonfire.h"
#include "diag.h"
#include "log.h"
#include "patches.h"
#include "settings.h"
#include "state.h"
#include "trace.h"
#include "inputdump.h"
#include "watch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

HMODULE g_self = nullptr;

bool filename_is_game(const wchar_t* path) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name = slash ? slash + 1 : path;
    return _wcsicmp(name, L"DarkSoulsRemastered.exe") == 0;
}

DWORD WINAPI worker(void*) {
    wchar_t dll_path[MAX_PATH];
    GetModuleFileNameW(g_self, dll_path, MAX_PATH);
    log_init(dll_path, L"DSR-FPS-Unlock.log");
    diag_install(dll_path, L"DSR-FPS-Unlock-crash.dmp");
    LOG_INFO("DS1 Remastered FPS Unlock v1.6.0");

    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    char exe_narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, exe_narrow, MAX_PATH, nullptr, nullptr);
    LOG_INFO("EXE: %s", exe_narrow);

    const Settings settings = settings_load(dll_path);
    LOG_INFO("FPSUnlock: %s", settings.fps_unlock ? "true" : "false");
    LOG_INFO("MaxFPS: %d", settings.target_fps);
    LOG_INFO("BonfireFix: %s", settings.bonfire_unstick ? "true" : "false");
    // FPSUnlock is the master switch: nothing below, diagnostics included, runs without it.
    if (!settings.fps_unlock) {
        LOG_INFO("FPSUnlock is false. The game is unchanged.");
        return 0;
    }
    if ((settings.trace || settings.watch) && !trace_start(dll_path)) {
        LOG_ERROR("Trace was requested but the CSV could not be opened");
    }
    if (settings.input_log) {
        inputdump_start(dll_path);
    }
    if (settings.watch && !watch_start(dll_path)) {
        LOG_ERROR("Watch was requested but could not start");
    }
    g_target_fps.store(static_cast<uint32_t>(settings.target_fps), std::memory_order_relaxed);
    g_variable_dt.store(settings.variable_frame_time ? 1 : 0, std::memory_order_relaxed);
    g_frame_dt.store(1.0f / static_cast<float>(settings.target_fps), std::memory_order_relaxed);
    LOG_INFO("VariableFrameTime: %s", settings.variable_frame_time ? "true" : "false");
    g_menu_filter.store(settings.menu_input_filter ? 1 : 0, std::memory_order_relaxed);
    LOG_INFO("MenuInputFilter: %s", settings.menu_input_filter ? "true" : "false");
    g_fix_camera.store(settings.fix_camera ? 1 : 0, std::memory_order_relaxed);
    g_camera_ptde_speed.store(settings.camera_ptde_speed ? 1 : 0, std::memory_order_relaxed);
    g_fix_lock_on_turn.store(settings.fix_lock_on_turn ? 1 : 0, std::memory_order_relaxed);
    g_fix_ghosts.store(settings.fix_ghosts ? 1 : 0, std::memory_order_relaxed);
    LOG_INFO("FixCamera: %s, FixLockOnTurn: %s, LockOnPtdeSpeed: %s, FixGhosts: %s", settings.fix_camera ? "true" : "false",
             settings.fix_lock_on_turn ? "true" : "false", settings.camera_ptde_speed ? "true" : "false",
             settings.fix_ghosts ? "true" : "false");
    g_input_log.store(settings.input_log ? 1 : 0, std::memory_order_relaxed);
    g_fix_hold.store(settings.fix_dpad_hold ? 1 : 0, std::memory_order_relaxed);
    g_fix_stamina_tick.store(settings.fix_stamina_tick ? 1 : 0, std::memory_order_relaxed);
    g_fix_ui.store(settings.fix_ui ? 1 : 0, std::memory_order_relaxed);
    g_fix_graze.store(settings.fix_graze ? 1 : 0, std::memory_order_relaxed);
    g_fix_damping.store(settings.fix_damping ? 1 : 0, std::memory_order_relaxed);
    g_fix_slide.store(settings.fix_slide ? 1 : 0, std::memory_order_relaxed);
    g_fix_jump.store(settings.fix_jump ? 1 : 0, std::memory_order_relaxed);
    g_fix_move_dt.store(settings.fix_move_dt ? 1 : 0, std::memory_order_relaxed);
    g_fix_step_down.store(settings.fix_step_down ? 1 : 0, std::memory_order_relaxed);
    g_fix_timers.store(settings.fix_timers ? 1 : 0, std::memory_order_relaxed);
    g_fix_smoothing.store(settings.fix_smoothing ? 1 : 0, std::memory_order_relaxed);
    g_fix_velocity.store(settings.fix_velocity ? 1 : 0, std::memory_order_relaxed);
    LOG_INFO("FixTimers: %s, FixSmoothing: %s, FixVelocity: %s", settings.fix_timers ? "true" : "false",
             settings.fix_smoothing ? "true" : "false", settings.fix_velocity ? "true" : "false");
    if (!patches_apply()) {
        LOG_ERROR("FPS unlock was not installed.");
    } else if (settings.bonfire_unstick) {
        bonfire_start();
    }
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        if (!filename_is_game(exe_path)) {
            return TRUE;
        }
        g_is_game.store(1, std::memory_order_release);
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        watch_stop();
        trace_stop();
    }
    return TRUE;
}
