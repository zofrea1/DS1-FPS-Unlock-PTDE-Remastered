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
    // Not GetWindowTextW: for a window of this process it sends WM_GETTEXT and waits for the window's thread. Under
    // RTX Remix the game's window belongs to a thread that waits on this one, and the game hung before its first
    // frame (2026-11-15). InternalGetWindowText reads the stored title without a message.
    InternalGetWindowText(h, title, 64);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::snprintf(out, n, "window %p class \"%ls\" title \"%ls\" %s", static_cast<void*>(h), cls, title,
                  pid == GetCurrentProcessId() ? "(this process)" : "(another process)");
}

// Under Wine (GameNative, Winlator, Proton) Wine can hand the foreground to its own desktop window (class #32769) or
// to no window at all while the game's window is still shown, with nothing on screen to click it back: on the phone,
// 12 minutes into a session the game window lost the foreground to the desktop with no Android event at the time,
// and every button was ignored from then on while the game ran on (2026-11-18 13:17). Under Wine the desktop or no
// window in front, with the game's window shown and not minimized, counts as the game's: it keeps reading its input.
// A real window of another program in front still counts as leaving. Windows itself is unaffected.
bool under_wine() {
    static int cached = -1;
    if (cached < 0) {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        cached = ntdll && GetProcAddress(ntdll, "wine_get_version") ? 1 : 0;
    }
    return cached == 1;
}

int g_kept = 0;        // times the desktop (or no window) in front was answered with the game's window
bool g_keeping = false;

HWND keep_under_wine(HWND h) {
    if (!under_wine() || (h && h != GetDesktopWindow())) {
        if (g_keeping) {
            g_keeping = false;
            if (g_kept <= kMaxLogged) LOG_INFO("Focus: Wine's desktop is no longer in front");
        }
        return h;
    }
    const HWND game = game_window();
    if (!game || !IsWindowVisible(game) || IsIconic(game)) return h;
    if (!g_keeping) {
        g_keeping = true;
        if (++g_kept <= kMaxLogged) {
            char what[256];
            describe(h, what, sizeof(what));
            LOG_INFO("Focus: Wine put %s in front of the game's window; the game keeps reading the keyboard, mouse and "
                     "gamepads (under Wine its desktop or no window in front counts as the game's)",
                     what);
        }
    }
    return game;
}

HWND WINAPI watched_foreground() {
    const HWND h = keep_under_wine(g_real());
    // The first answer, whatever it is (a game that is in front from the start logs no change at all).
    static bool first = true;
    if (first) {
        if (const HWND game = game_window()) {
            first = false;
            char who[200];
            describe(h, who, sizeof(who));
            LOG_INFO("Focus: at the game's first input check its window is %s (in front: %s)",
                     h == game ? "in front" : "NOT in front; the game ignores the keyboard, mouse and gamepads", who);
        }
    }
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
