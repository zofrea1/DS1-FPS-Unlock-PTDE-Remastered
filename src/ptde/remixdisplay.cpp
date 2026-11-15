#include "remixdisplay.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

using ChangeDisplayFn = LONG(WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
ChangeDisplayFn g_original = nullptr;
volatile LONG g_refresh = -1;
int g_logged = 0;

// The rate for a mode of `w` x `h` (and `bpp` when nonzero) on `device`: FullscreenRefreshRate 0 = the highest listed,
// N = N when listed, else the highest below N. 0 when nothing is listed at that size.
DWORD choose_rate(LPCWSTR device, DWORD w, DWORD h, DWORD bpp) {
    const LONG want = g_refresh;
    DWORD best = 0, exact = 0;
    DEVMODEW m{};
    m.dmSize = sizeof(m);
    for (DWORD i = 0; EnumDisplaySettingsExW(device, i, &m, 0); ++i) {
        if (m.dmPelsWidth != w || m.dmPelsHeight != h || (bpp && m.dmBitsPerPel != bpp)) continue;
        const DWORD r = m.dmDisplayFrequency;
        if (want > 0) {
            if (r == static_cast<DWORD>(want)) exact = r;
            if (r <= static_cast<DWORD>(want) && r > best) best = r;
        } else if (r > best) {
            best = r;
        }
    }
    return exact ? exact : best;
}

LONG WINAPI hook_change_display(LPCWSTR device, DEVMODEW* mode, HWND window, DWORD flags, LPVOID param) {
    if (mode && (mode->dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) == (DM_PELSWIDTH | DM_PELSHEIGHT) &&
        mode->dmPelsWidth && mode->dmPelsHeight && g_refresh >= 0) {
        const DWORD bpp = (mode->dmFields & DM_BITSPERPEL) ? mode->dmBitsPerPel : 0;
        const DWORD asked = (mode->dmFields & DM_DISPLAYFREQUENCY) ? mode->dmDisplayFrequency : 0;
        const DWORD rate = choose_rate(device, mode->dmPelsWidth, mode->dmPelsHeight, bpp);
        if (rate && rate != asked) {
            DEVMODEW copy = *mode;
            copy.dmFields |= DM_DISPLAYFREQUENCY;
            copy.dmDisplayFrequency = rate;
            if (g_logged++ < 4) {
                LOG_INFO("FullscreenRefreshRate: RTX Remix sets %lux%lu at %lu Hz; asking for %lu Hz instead (%s)",
                         mode->dmPelsWidth, mode->dmPelsHeight, asked, rate,
                         g_refresh > 0 ? "FullscreenRefreshRate = N" : "the display's highest at that size");
            }
            const LONG r = g_original(device, &copy, window, flags, param);
            if (r == DISP_CHANGE_SUCCESSFUL) return r;
            LOG_ERROR("FullscreenRefreshRate: %lu Hz was refused (%ld); RTX Remix's own mode instead", rate, r);
        }
    }
    return g_original(device, mode, window, flags, param);
}

// The import of USER32!ChangeDisplaySettingsExW in `module`, wrapped. True when it is (now or already).
bool wrap_import(HMODULE module) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "user32.dll") != 0 || !imp->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "ChangeDisplaySettingsExW") != 0) continue;
            void* const current = reinterpret_cast<void*>(slots->u1.Function);
            if (current == reinterpret_cast<void*>(&hook_change_display)) return true;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return false;
            if (!g_original) g_original = reinterpret_cast<ChangeDisplayFn>(current);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(&hook_change_display);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            return true;
        }
    }
    return false;
}

}  // namespace

void remix_display_install(int refresh) {
    InterlockedExchange(&g_refresh, refresh);
    if (refresh < 0) return;
    static bool logged = false;
    bool any = false;
    for (const wchar_t* name : {L"d3d9_remix.dll", L"d3d9.dll"}) {
        if (HMODULE m = GetModuleHandleW(name)) any = wrap_import(m) || any;
    }
    if (!logged) {
        logged = true;
        if (any) {
            LOG_INFO("FullscreenRefreshRate: RTX Remix's display mode follows it (%s)",
                     refresh > 0 ? "the rate set here" : "the display's highest at the game's size");
        } else {
            LOG_INFO("FullscreenRefreshRate: RTX Remix's bridge sets no display mode through USER32 here; its own rate "
                     "stays");
        }
    }
}
