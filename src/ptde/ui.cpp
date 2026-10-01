#include "ui.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

// ---- Loading screen swirl ---------------------------------------------------------------------
//
// FrpgMenuDlgNowLoading::update (0xC383C0) runs `mov eax, 1; add [esi+0xF4], eax; ...;
// add [esi+0xF8], eax` every time it is drawn; both counters wrap at 100 and drive the bonfire
// swirl. At 30 FPS that is 30 steps a second. The `mov eax, 1` at 0xC383FD becomes a call that
// returns how many 1/30 s steps have really elapsed since the last call.
constexpr uint32_t kSwirlSite = 0x00C383FD;

uint32_t __stdcall swirl_steps() {
    static LARGE_INTEGER freq{};
    static LARGE_INTEGER last{};
    static double carry = 0.0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    if (last.QuadPart == 0) {
        last = now;
        return 1;
    }
    double elapsed = static_cast<double>(now.QuadPart - last.QuadPart) / static_cast<double>(freq.QuadPart);
    last = now;
    // The swirl is not drawn while the screen is being built; a long gap must not skip ahead.
    if (elapsed > 0.25) {
        elapsed = 1.0 / 30.0;
    }
    carry += elapsed * 30.0;
    const uint32_t whole = static_cast<uint32_t>(carry);
    carry -= whole;
    return whole;
}

__declspec(naked) void swirl_stub() {
    __asm {
        push ecx
        push edx
        call swirl_steps
        pop edx
        pop ecx
        ret
    }
}

bool install_swirl() {
    auto* site = reinterpret_cast<uint8_t*>(kSwirlSite);
    static const uint8_t kExpect[5] = {0xB8, 0x01, 0x00, 0x00, 0x00};
    if (std::memcmp(site, kExpect, 5) != 0) {
        LOG_ERROR("Loading swirl site %08X does not match this build. Not patching.", kSwirlSite);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("Could not unprotect the loading swirl site (Win32=%lu)", GetLastError());
        return false;
    }
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&swirl_stub) - (kSwirlSite + 5));
    site[0] = 0xE8;
    std::memcpy(site + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    DWORD ignored = 0;
    VirtualProtect(site, 5, old, &ignored);
    LOG_INFO("Loading screen swirl follows real time (site %08X)", kSwirlSite);
    return true;
}

// ---- Bonfire softlock watchdog ----------------------------------------------------------------
//
// Reported for Prepare to Die Edition at an unlocked frame rate: after "Reverse hollowing" the
// bonfire menu disappears but the character stays seated and nothing responds. The same problem
// was first solved for this game by NullBy7e's FPSFix and Sean Pesce's FPSFix+ (GPL-3.0). The
// addresses and animation ids below come from their published notes; the code is written fresh.
constexpr uint32_t kMenuState = 0x013786D0;     // pointer to the menu state block
constexpr uint32_t kPlayerStatusPtr = 0x0137E204;  // pointer; +0xA28 is the character status
constexpr uint32_t kAnimPtr = 0x012E29E8;       // pointer -> pointer -> +0xFC is the animation id
constexpr uint32_t kMenuFlags[] = {0x40, 0x4C, 0x78, 0x84, 0x80, 0x50, 0xAC, 0x60};

bool read_u32(uint32_t address, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_u8(uint32_t address, uint8_t* out) {
    __try {
        *out = *reinterpret_cast<const uint8_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Address of the animation id, or 0 when the chain is not resolvable yet.
uint32_t animation_address() {
    uint32_t a = 0, b = 0;
    if (!read_u32(kAnimPtr, &a) || a < 0x10000) return 0;
    if (!read_u32(a, &b) || b < 0x10000) return 0;
    return b + 0xFC;
}

// True when the character is seated at a bonfire with no bonfire menu or dialog open.
bool stuck_at_bonfire(uint32_t* anim_address) {
    uint32_t status_block = 0;
    if (!read_u32(kPlayerStatusPtr, &status_block) || status_block < 0x10000) return false;
    uint32_t status = 0;
    if (!read_u32(status_block + 0xA28, &status)) return false;
    if (status != 0 && status != 8) return false;  // human or hollow only

    const uint32_t address = animation_address();
    if (!address) return false;
    uint32_t anim = 0;
    if (!read_u32(address, &anim)) return false;
    if (anim != 7701 && anim != 7711 && anim != 7721) return false;  // the three bonfire sit animations

    uint32_t menus = 0;
    if (!read_u32(kMenuState, &menus) || menus < 0x10000) return false;
    for (uint32_t offset : kMenuFlags) {
        uint8_t open = 0;
        if (!read_u8(menus + offset, &open)) return false;
        if (open) return false;
    }
    *anim_address = address;
    return true;
}

DWORD WINAPI bonfire_thread(void*) {
    Sleep(2000);
    ULONGLONG since = 0;
    int fixed = 0;
    while (GetModuleHandleW(nullptr)) {
        Sleep(200);
        uint32_t address = 0;
        if (!stuck_at_bonfire(&address)) {
            since = 0;
            continue;
        }
        const ULONGLONG now = GetTickCount64();
        if (since == 0) {
            since = now;
            continue;
        }
        if (now - since < 1000) continue;
        __try {
            *reinterpret_cast<uint32_t*>(address) = 0;
            ++fixed;
            LOG_INFO("Bonfire softlock: no menu open for a second while seated; told the character to stand (%d)",
                     fixed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("Bonfire softlock: could not write the animation id");
        }
        since = 0;
    }
    return 0;
}

// ---- HUD shortcut input log -------------------------------------------------------------------
//
// fn 0x783A70 asks a HUD dialog whether an action fired this frame (the shortcut dialog asks
// for 0x51-0x54 and 0xD5: the D-pad item and weapon cycling). This logs every "yes".
constexpr uint32_t kActionQuery = 0x00783A70;
constexpr size_t kActionStolen = 10;  // sub esp,8; test byte [ecx+0A4h],1 (no relative operands)

using ActionFn = bool(__fastcall*)(void* self, void* edx, int action);
ActionFn g_action_orig = nullptr;
LONG g_action_lines = 0;

bool __fastcall hk_action(void* self, void* edx, int action) {
    const bool result = g_action_orig(self, edx, action);
    if (result && ((action >= 0x51 && action <= 0x54) || action == 0xD5) && g_action_lines < 4000) {
        InterlockedIncrement(&g_action_lines);
        LOG_INFO("[input] t=%llu action=0x%02X", GetTickCount64(), action);
    }
    return result;
}

bool install_input_log() {
    auto* site = reinterpret_cast<uint8_t*>(kActionQuery);
    static const uint8_t kExpect[kActionStolen] = {0x83, 0xEC, 0x08, 0xF6, 0x81, 0xA4, 0x00, 0x00, 0x00, 0x01};
    if (std::memcmp(site, kExpect, kActionStolen) != 0) {
        LOG_ERROR("HUD action query at %08X does not match this build. Input log off.", kActionQuery);
        return false;
    }
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, site, kActionStolen);
    tramp[kActionStolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((kActionQuery + kActionStolen) - reinterpret_cast<uintptr_t>(tramp + kActionStolen + 5));
    std::memcpy(tramp + kActionStolen + 1, &back, 4);
    g_action_orig = reinterpret_cast<ActionFn>(tramp);

    DWORD old = 0;
    if (!VirtualProtect(site, kActionStolen, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_action) - (kActionQuery + 5));
    std::memcpy(site + 1, &rel, 4);
    std::memset(site + 5, 0x90, kActionStolen - 5);
    FlushInstructionCache(GetCurrentProcess(), site, kActionStolen);
    DWORD ignored = 0;
    VirtualProtect(site, kActionStolen, old, &ignored);
    LOG_INFO("Input log: HUD shortcut actions will be logged as [input] lines");
    return true;
}

// ---- Hit point log ----------------------------------------------------------------------------
//
// Diagnostic: logs every change of the player's hit points (PlayerGameData at [0x1378700]+8,
// current at +0xC, maximum at +0x10) so the speed of an Estus heal can be measured.
constexpr uint32_t kGameDataPtr = 0x01378700;

DWORD WINAPI hp_thread(void*) {
    int last = -1;
    while (GetModuleHandleW(nullptr)) {
        Sleep(1);
        uint32_t manager = 0, data = 0, hp = 0, max_hp = 0;
        if (!read_u32(kGameDataPtr, &manager) || manager < 0x10000) continue;
        if (!read_u32(manager + 8, &data) || data < 0x10000) continue;
        if (!read_u32(data + 0xC, &hp) || !read_u32(data + 0x10, &max_hp)) continue;
        if (static_cast<int>(hp) != last) {
            last = static_cast<int>(hp);
            LOG_INFO("[hp] hp=%u max=%u", hp, max_hp);
        }
    }
    return 0;
}

}  // namespace

bool ui_install(const Settings& settings) {
    bool ok = true;
    if (settings.fix_ui) {
        ok &= install_swirl();
    }
    if (settings.bonfire_unstick) {
        HANDLE thread = CreateThread(nullptr, 0, bonfire_thread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
            LOG_INFO("Bonfire softlock watchdog running");
        } else {
            ok = false;
        }
    }
    if (settings.input_log) {
        HANDLE hp = CreateThread(nullptr, 0, hp_thread, nullptr, 0, nullptr);
        if (hp) {
            CloseHandle(hp);
        }
        ok &= install_input_log();
    }
    return ok;
}
