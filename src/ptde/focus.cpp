#include "focus.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t kForegroundImport = 0x010CC2BC;  // USER32!GetForegroundWindow in the game's import table
constexpr int kMaxLogged = 40;

using GetForegroundFn = HWND(WINAPI*)();
GetForegroundFn g_real = nullptr;
HWND g_last = reinterpret_cast<HWND>(-1);
bool g_game_in_front = true;
ULONGLONG g_left_at = 0;
int g_logged = 0;
HWND g_game = nullptr;

HWND game_window() {
    if (!g_game || !IsWindow(g_game)) g_game = FindWindowW(L"DARK SOULS", nullptr);
    return g_game;
}

void describe(HWND h, char* out, size_t n) {
    if (!h) {
        std::snprintf(out, n, "no window");
        return;
    }
    wchar_t cls[64] = L"";
    wchar_t title[64] = L"";
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, title, 64);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::snprintf(out, n, "window %p class \"%ls\" title \"%ls\" %s", static_cast<void*>(h), cls, title,
                  pid == GetCurrentProcessId() ? "(this process)" : "(another process)");
}

HWND WINAPI watched_foreground() {
    const HWND h = g_real();
    if (h == g_last) return h;
    g_last = h;
    const HWND game = game_window();
    if (!game) return h;
    const bool in_front = h == game;
    if (in_front == g_game_in_front) return h;
    g_game_in_front = in_front;
    if (g_logged >= kMaxLogged) return h;
    if (++g_logged == kMaxLogged) {
        LOG_INFO("Focus: more changes are not logged");
        return h;
    }
    const ULONGLONG now = GetTickCount64();
    if (!in_front) {
        g_left_at = now;
        char what[256];
        describe(h, what, sizeof(what));
        LOG_INFO("Focus: the game window is no longer in front (now %s); the game ignores the keyboard, mouse and "
                 "gamepads until it is",
                 what);
    } else {
        LOG_INFO("Focus: the game window is in front again after %.1f s", (now - g_left_at) / 1000.0);
    }
    return h;
}

}  // namespace

void focus_install() {
    auto* slot = reinterpret_cast<void**>(kForegroundImport);
    // Only the game's own import, as the loader filled it in (not another build, or a slot something else rewrote).
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* const system = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetForegroundWindow")) : nullptr;
    if (!system || *slot != system) {
        LOG_INFO("Focus: the import at 0x%08X is not GetForegroundWindow; focus changes are not logged",
                 static_cast<unsigned>(kForegroundImport));
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return;
    g_real = reinterpret_cast<GetForegroundFn>(*slot);
    if (g_real && g_real != &watched_foreground) *slot = reinterpret_cast<void*>(&watched_foreground);
    VirtualProtect(slot, 4, old, &old);
}
