#include "log.h"
#include "patches.h"
#include "settings.h"
#include "state.h"
#include "trace.h"

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
    log_init(dll_path);
    LOG_INFO("DS1 Remastered FPS Unlock v0.0.1");

    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    char exe_narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, exe_narrow, MAX_PATH, nullptr, nullptr);
    LOG_INFO("EXE: %s", exe_narrow);

    const Settings settings = settings_load(dll_path);
    LOG_INFO("FPSUnlock: %s", settings.fps_unlock ? "true" : "false");
    LOG_INFO("TargetFPS: %d", settings.target_fps);
    if (settings.trace && !trace_start(dll_path)) {
        LOG_ERROR("Trace was requested but the CSV could not be opened");
    }
    if (!settings.fps_unlock) {
        LOG_INFO("FPSUnlock is false. The game is unchanged.");
        return 0;
    }
    if (settings.target_fps < 61 || settings.target_fps > 1000) {
        LOG_ERROR("TargetFPS must be from 61 to 1000. The game is unchanged.");
        return 0;
    }
    g_target_fps.store(static_cast<uint32_t>(settings.target_fps), std::memory_order_relaxed);
    g_fix_move_dt.store(settings.fix_move_dt ? 1 : 0, std::memory_order_relaxed);
    if (!patches_apply()) {
        LOG_ERROR("FPS unlock was not installed.");
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
        trace_stop();
    }
    return TRUE;
}
