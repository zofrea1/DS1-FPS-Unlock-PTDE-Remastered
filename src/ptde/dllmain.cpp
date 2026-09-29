#include "d3d.h"
#include "dsfix.h"
#include "frame.h"
#include "log.h"
#include "patches.h"
#include "settings.h"
#include "window.h"
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
    LOG_INFO("DS1 PTDE FPS Unlock (development build)");

    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    char exe_narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, exe_narrow, MAX_PATH, nullptr, nullptr);
    LOG_INFO("EXE: %s", exe_narrow);

    const Settings settings = settings_load(dll_path);
    LOG_INFO("FPSUnlock: %s", settings.fps_unlock ? "true" : "false");
    LOG_INFO("TargetFPS: %d", settings.target_fps);

    wchar_t game_dir[MAX_PATH];
    lstrcpynW(game_dir, exe_path, MAX_PATH);
    wchar_t* slash = wcsrchr(game_dir, L'\\');
    if (slash) {
        slash[1] = 0;
    }
    const DsfixInfo dsfix = dsfix_probe(game_dir);
    if (dsfix.present) {
        LOG_INFO("DSfix detected (DSfix.ini %s). unlockFPS=%d FPSlimit=%d", dsfix.ini_found ? "found" : "not found",
                 dsfix.unlock_fps ? 1 : 0, dsfix.fps_limit);
        if (dsfix.unlock_fps && settings.fps_unlock) {
            LOG_ERROR("DSfix has its own FPS unlock enabled. Set unlockFPS 0 in DSfix.ini while this mod drives "
                      "the frame rate, otherwise the two fight over the same timestep.");
        }
    } else {
        LOG_INFO("DSfix not detected");
    }

    if (!settings.fps_unlock) {
        LOG_INFO("FPSUnlock is false. The game is unchanged.");
        return 0;
    }
    if (settings.target_fps < 31 || settings.target_fps > 1000) {
        LOG_ERROR("TargetFPS must be from 31 to 1000. The game is unchanged.");
        return 0;
    }
    if (patches_probe(settings)) {
        frame_install(settings);
    }
    if (settings.borderless) {
        if (dsfix.borderless) {
            LOG_INFO("DSfix's own borderlessFullscreen is on; leaving the window to DSfix.");
        } else {
            borderless_start();
        }
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
        // Before the game calls Direct3DCreate9: wraps it to override the fullscreen refresh rate.
        d3d_install_early(instance);
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        log_close();
    }
    return TRUE;
}
