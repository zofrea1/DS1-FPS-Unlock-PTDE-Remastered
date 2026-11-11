#include "cursor.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#pragma comment(lib, "user32.lib")

namespace {

constexpr UINT_PTR kTimer = 0x46505355;  // "FPSU"
constexpr ULONGLONG kIdleMs = 2000;

WNDPROC g_original = nullptr;
HWND g_window = nullptr;
bool g_hidden = false;
bool g_timer = false;
POINT g_last{-1, -1};
ULONGLONG g_moved = 0;

bool pointer_in_client(HWND window) {
    POINT p{};
    RECT r{};
    if (!GetCursorPos(&p) || !ScreenToClient(window, &p) || !GetClientRect(window, &r)) return false;
    return PtInRect(&r, p) != FALSE;
}

LRESULT CALLBACK cursor_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (!g_timer) {
        // SetTimer on a window belongs on the window's thread: the first message brings us there.
        g_timer = SetTimer(window, kTimer, 250, nullptr) != 0;
        g_moved = GetTickCount64();
    }
    switch (message) {
        case WM_MOUSEMOVE: {
            const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (p.x != g_last.x || p.y != g_last.y) {
                g_last = p;
                g_moved = GetTickCount64();
                if (g_hidden) {
                    g_hidden = false;
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));  // IDC_ARROW
                }
            }
            break;
        }
        case WM_SETCURSOR:
            if (g_hidden && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wp == kTimer) {
                if (!g_hidden && GetTickCount64() - g_moved >= kIdleMs && GetForegroundWindow() == window &&
                    pointer_in_client(window)) {
                    g_hidden = true;
                    SetCursor(nullptr);
                }
                return 0;
            }
            break;
        default:
            break;
    }
    return CallWindowProcW(g_original, window, message, wp, lp);
}

struct Search {
    DWORD pid;
    HWND best;
    long best_area;
};

BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<Search*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return TRUE;
    const long area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > search->best_area) {
        search->best_area = area;
        search->best = window;
    }
    return TRUE;
}

DWORD WINAPI watch_thread(void*) {
    // The game makes its window a few seconds after start; a small window first (the launcher) is skipped.
    for (int i = 0; i < 600 && !g_window; ++i) {
        Sleep(500);
        Search search{GetCurrentProcessId(), nullptr, 0};
        EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
        if (search.best && search.best_area >= 640 * 360) g_window = search.best;
    }
    if (!g_window) {
        LOG_INFO("HideMouseCursor: the game window did not show up; the pointer is left alone");
        return 0;
    }
    g_original = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&cursor_proc)));
    if (!g_original) {
        LOG_ERROR("HideMouseCursor: could not subclass the game window (error %lu)", GetLastError());
        return 0;
    }
    // Wake the window so the timer starts even before the first mouse event.
    PostMessageW(g_window, WM_NULL, 0, 0);
    LOG_INFO("HideMouseCursor: the pointer hides over the game after %llu s without moving", kIdleMs / 1000);
    return 0;
}

}  // namespace

bool cursor_start() {
    HANDLE h = CreateThread(nullptr, 0, &watch_thread, nullptr, 0, nullptr);
    if (!h) return false;
    CloseHandle(h);
    return true;
}
