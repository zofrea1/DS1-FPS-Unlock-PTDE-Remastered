#include "window.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

#pragma comment(lib, "user32.lib")

namespace {

struct Search {
    DWORD pid;
    HWND best;
    long best_area;
};

BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<Search*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(window, &rect)) {
        return TRUE;
    }
    const long area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > search->best_area) {
        search->best_area = area;
        search->best = window;
    }
    return TRUE;
}

HWND find_game_window() {
    Search search{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

bool same_rect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

DWORD WINAPI watcher(void*) {
    HWND applied_to = nullptr;
    for (;;) {
        Sleep(500);
        HWND window = find_game_window();
        if (!window || IsIconic(window)) {
            continue;
        }
        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            continue;
        }
        RECT rect{};
        GetWindowRect(window, &rect);
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        const bool framed = (style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME)) != 0;
        const bool covers = same_rect(rect, info.rcMonitor);
        if (!framed && covers) {
            continue;  // already borderless, or exclusive fullscreen: leave it alone
        }
        const LONG_PTR frame = WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU |
                               WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        const LONG_PTR ex_frame = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
        SetWindowLongPtrW(window, GWL_STYLE, (style & ~frame) | WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style & ~ex_frame);
        // Async: the game's window thread may be busy, and this must never block it.
        SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (applied_to != window) {
            char cls[128] = {};
            GetClassNameA(window, cls, sizeof(cls));
            LOG_INFO("Borderless: window %p (class \"%s\") %ldx%ld at %ld,%ld style=%08lX -> monitor %ldx%ld at %ld,%ld",
                     window, cls, rect.right - rect.left, rect.bottom - rect.top, rect.left, rect.top,
                     static_cast<unsigned long>(style), info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top, info.rcMonitor.left, info.rcMonitor.top);
            applied_to = window;
        }
    }
    return 0;
}

}  // namespace

bool borderless_start() {
    HANDLE thread = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);
    if (!thread) {
        return false;
    }
    CloseHandle(thread);
    LOG_INFO("Borderless fullscreen enabled (needs the game in windowed mode)");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// DPI awareness

namespace {
const char* g_dpi_result = nullptr;  // what dpi_aware_early did, for the log
}  // namespace

void dpi_aware_early() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using ContextFn = BOOL(WINAPI*)(HANDLE);
    auto set_context = user32 ? reinterpret_cast<ContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
                              : nullptr;
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4, PER_MONITOR_AWARE = -3 (Windows 10 1703 / 1607).
    if (set_context && set_context(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) {
        g_dpi_result = "per-monitor DPI aware (v2)";
    } else if (set_context && set_context(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-3)))) {
        g_dpi_result = "per-monitor DPI aware";
    } else if (GetLastError() == ERROR_ACCESS_DENIED) {
        g_dpi_result = "already set elsewhere (a compatibility setting on DARKSOULS.exe or the game's manifest)";
    } else if (SetProcessDPIAware()) {
        g_dpi_result = "system DPI aware (this Windows has no per-monitor awareness)";
    } else {
        g_dpi_result = "could not be set";
    }
}

void dpi_aware_log(bool wanted) {
    if (!wanted) {
        LOG_INFO("DpiAware is false: on a display scaled above 100%%, Windows stretches the game's picture");
        return;
    }
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode);
    LOG_INFO("DpiAware: %s; primary monitor %ldx%ld as the game sees it, %lux%lu pixels", g_dpi_result ? g_dpi_result :
             "not set", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
             mode.dmPelsWidth, mode.dmPelsHeight);
}
