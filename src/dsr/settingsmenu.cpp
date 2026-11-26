#include "settingsmenu.h"

#include "fpsu_buildcfg.h"
#include "log.h"
#include "overlay.h"
#include "proxy.h"
#include "settings.h"
#include "settingspage.h"
#include "state.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// FPS Unlock Settings in Remastered. The page, the INI and the controls are src/common/settingspage.cpp; here are
// Remastered's rows, the drawing (overlay.cpp, the swap chain's Present) and the game's controls:
//   * the controller: the game reads it with XInputGetState from XINPUT1_3.dll (imported by ordinal 2); that import is
//     pointed at a wrapper, which notes each pad and gives the game none while the page has the controls;
//   * the keyboard and mouse: the game's DirectInput devices (proxy.cpp, which watches their reads);
//   * the System menu's screens (the System list and each screen it opens: NS_FRPG::FrpgMenuDlgSystem, ...Option,
//     found by their run-time type information): each one's method that runs once a frame while it is up is counted
//     (hook_system_screens); while one runs, Y opens the page and a line at the bottom says so.
// The rows: the frame rate cap and the switches that pick the original game's or Remastered's own behaviour. Most of
// those are read by the fixes every time (the atomics in state.h), so they apply at once; the few that are set into
// the game's code at start say they apply after a restart. The fixes themselves stay in the INI.
namespace {

std::wstring g_dir;  // the game folder, with its trailing backslash
bool g_effects_mod = false;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool on(const char* v) {
    const std::string s = lower(v);
    return s == "true" || s == "1" || s == "on" || s == "yes";
}
int look(const char* v) { return lower(v) == "auto" ? (g_effects_mod ? 1 : 0) : (on(v) ? 1 : 0); }

// ---------------------------------------------------------------- rows
void apply_max_fps(const char* v) {
    int n = std::atoi(v);
    n = n < kMinMaxFps ? kMinMaxFps : (n > kMaxMaxFps ? kMaxMaxFps : n);
    g_target_fps.store(static_cast<uint32_t>(n), std::memory_order_relaxed);
}
void apply_variable(const char* v) { g_variable_dt.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_lock_on(const char* v) { g_camera_ptde_speed.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_slide_friction(const char* v) { g_slide_ptde.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_graze(const char* v) { g_graze_ptde.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_step_down(const char* v) { g_step_down_ptde.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_step_height(const char* v) { g_step_height_ptde.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_air(const char* v) { g_air_damping_ptde.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_foot_ik(const char* v) { g_foot_ik_ptde_speed.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_foot_ik_npcs(const char* v) { g_foot_ik_npcs.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_slide(const char* v) { g_slide_ptde_speed.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_hud(const char* v) { g_hud_ptde_speed.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_speffect(const char* v) { g_speffect_ptde_speed.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_event_wait(const char* v) { g_event_wait_ptde_speed.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_bullet(const char* v) { g_bullet_ptde_speed.store(on(v) ? 1 : 0, std::memory_order_relaxed); }
void apply_sway(const char* v) { g_sfx_sway_ptde_speed.store(look(v), std::memory_order_relaxed); }

const SettingsMenuChoice kFps[] = {{"30", L"30"},   {"40", L"40"},   {"45", L"45"},   {"60", L"60"},   {"72", L"72"},
                                   {"90", L"90"},   {"100", L"100"}, {"120", L"120"}, {"144", L"144"}, {"165", L"165"},
                                   {"180", L"180"}, {"200", L"200"}, {"225", L"225"}, {"240", L"240"}, {"288", L"288"},
                                   {"300", L"300"}, {"360", L"360"}, {"480", L"480"}};
const SettingsMenuChoice kOnOff[] = {{"true", L"On"}, {"false", L"Off"}};
const SettingsMenuChoice kFeel[] = {{"true", L"The original game's"}, {"false", L"Remastered's"}};
const SettingsMenuChoice kLook[] = {{"auto", L"Auto"}, {"true", L"The original game's"}, {"false", L"Remastered's"}};

const SettingsMenuRow kFrame[] = {
    {"MaxFPS", L"Frame rate cap", L"The highest frame rate the game runs at. Keep it a little below what your PC holds "
     L"in busy places, and below your display's refresh rate (set Frequency in the game's PC settings).", kFps,
     _countof(kFps), apply_max_fps, false},
    {"VariableFrameTime", L"Real time when the frame rate dips", L"On: the game is handed the measured frame time, so "
     L"it keeps its speed when the frame rate dips. Off: a fixed step of 1/cap, so it runs slow below the cap.",
     kOnOff, _countof(kOnOff), apply_variable, false},
    {"MenuInputFilter", L"Menu presses counted once", L"One button press is not seen several times a frame in the menus "
     L"at a high frame rate. Turn it off if a menu ever stops responding.", kOnOff, _countof(kOnOff), nullptr, false},
    {"BonfireFix", L"Stand up from a stuck bonfire", L"After resting, a character left seated with no menu and no way "
     L"to stand is told to stand.", kOnOff, _countof(kOnOff), nullptr, false},
    {"StartOffline", L"Start offline", L"The game starts in offline mode (the modded game stays off the online "
     L"servers). To play online again, also set the game's Launch setting back to Start Online.", kOnOff,
     _countof(kOnOff), nullptr, false, L"Applies at the next start of the game"},
    {"SkipIntro", L"Skip the start-up logos", L"Straight to the title screen.", kOnOff, _countof(kOnOff), nullptr, false,
     L"Applies at the next start of the game"},
};
const SettingsMenuRow kMovement[] = {
    {"LockOnPtdeSpeed", L"Lock-on turn speed", L"The lock-on camera's pan and the body's turn toward the target. "
     L"Remastered turns twice as fast as the original game.", kFeel, _countof(kFeel), apply_lock_on, false},
    {"SlideFrictionPtde", L"Sliding off a slope", L"Momentum after sliding off a steep slope fades as in the original "
     L"game; Remastered stops it twice as fast.", kFeel, _countof(kFeel), apply_slide_friction, false},
    {"GrazePtde", L"Walls and corners", L"Running into a wall or a corner slows you down, and you pick up speed again, "
     L"at the original game's pace; Remastered does both twice as fast.", kFeel, _countof(kFeel), apply_graze, false},
    {"StepDownPtde", L"Ground that drops away", L"How far the character holds on to ground that drops away (a rounded "
     L"ledge, a steep slope) before falling; Remastered holds on twice as far.", kFeel, _countof(kFeel),
     apply_step_down, false},
    {"StepHeightPtde", L"Step height", L"The highest step climbed without a jump: 0.30 m in the original game, 0.31 m "
     L"in Remastered.", kFeel, _countof(kFeel), apply_step_height, false},
    {"AirDampingPtde", L"Momentum in the air", L"Momentum after walking or running off a ledge fades at the original "
     L"game's rate (about 2% faster over a second than Remastered's).", kFeel, _countof(kFeel), apply_air, false},
    {"FootIkPtdeSpeed", L"Feet on stairs and slopes", L"The hips settle onto stairs and slopes at the original game's "
     L"speed; Remastered settles them twice as fast.", kFeel, _countof(kFeel), apply_foot_ik, false},
    {"FootIkNpcs", L"NPCs' feet on the ground", L"Human NPCs and summoned phantoms place their feet on stairs and "
     L"slopes, as your character does.", kOnOff, _countof(kOnOff), apply_foot_ik_npcs, false},
    {"SlidePtdeSpeed", L"Slides", L"A slide's contact and its turns as in the original game (30 FPS) or as in "
     L"Remastered (60 FPS), where a turn carries a quarter of the weight.", kFeel, _countof(kFeel), apply_slide, false},
    {"StickInputPtdeSpeed", L"Stick timing", L"Kicks, jumping attacks, a roll's direction and switchbacks follow the "
     L"original game's 30 FPS (the most forgiving timing) or Remastered's 60 FPS (twice as quick).", kFeel,
     _countof(kFeel), nullptr, false},
};
const SettingsMenuRow kTiming[] = {
    {"HudPtdeSpeed", L"HUD bars", L"Health, stamina and boss bars fill and drain at the original game's speed; "
     L"Remastered moves them twice as fast.", kFeel, _countof(kFeel), apply_hud, false},
    {"SpEffectPtdeSpeed", L"Poison and other effects", L"Effects over time tick on the original game's 30 FPS (the "
     L"swamp poisons 30 points a second) or Remastered's 60 FPS (36).", kFeel, _countof(kFeel), apply_speffect, false},
    {"EventWaitPtdeSpeed", L"Scripted waits", L"Scripted sequences keep the original game's timing (30 FPS) or "
     L"Remastered's (60 FPS).", kFeel, _countof(kFeel), apply_event_wait, false},
    {"BulletPtdeSpeed", L"Crawling and repeating attacks", L"Projectiles that crawl along the ground (the undead "
     L"dragon's acid) and repeating ones keep the original game's slide and schedule, or Remastered's.", kFeel,
     _countof(kFeel), apply_bullet, false},
    {"SfxDistortionPtdeSpeed", L"Heat haze", L"Lava and fire haze rolls on the original game's 30 FPS or Remastered's "
     L"60. Auto: the original game's with the PTDE effects mod installed.", kLook, _countof(kLook), nullptr, false},
    {"SfxSwayPtdeSpeed", L"Swaying particles", L"Swaying effects turn on the original game's 30 FPS or Remastered's "
     L"60. Auto: the original game's with the PTDE effects mod installed.", kLook, _countof(kLook), apply_sway, false},
    {"SfxTrailsPtdeSpeed", L"Trails", L"Weapon and effect trails as long as in the original game (30 FPS) or "
     L"Remastered (60). Auto: the original game's with the PTDE effects mod installed.", kLook, _countof(kLook),
     nullptr, false},
};

// ---------------------------------------------------------------- the controller
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
XInputGetStateFn g_xinput_get_state = nullptr;
volatile LONG g_pad_buttons[4] = {};
volatile LONG g_pad_stick[4] = {};  // lx in the low 16 bits, ly in the high 16
volatile LONG g_blank = 0;          // the page has the controls

DWORD WINAPI hk_xinput_get_state(DWORD index, void* state) {
    const DWORD result = g_xinput_get_state(index, state);
    if (index < 4) {
        if (result != 0 || !state) {
            InterlockedExchange(&g_pad_buttons[index], 0);
            InterlockedExchange(&g_pad_stick[index], 0);
        } else {
            // XINPUT_STATE: packet (4), buttons (2), triggers (2), lx, ly, rx, ry.
            auto* b = static_cast<uint8_t*>(state);
            WORD buttons = 0;
            int16_t lx = 0, ly = 0;
            std::memcpy(&buttons, b + 4, 2);
            std::memcpy(&lx, b + 8, 2);
            std::memcpy(&ly, b + 10, 2);
            InterlockedExchange(&g_pad_buttons[index], buttons);
            InterlockedExchange(&g_pad_stick[index],
                                static_cast<LONG>((static_cast<uint32_t>(static_cast<uint16_t>(ly)) << 16) |
                                                  static_cast<uint16_t>(lx)));
            if (g_blank) std::memset(b + 4, 0, 12);
        }
    }
    return result;
}

// The game's own import of a function (DLL name prefix, function name or ordinal) pointed at HOOK; the original in
// *ORIGINAL. False when the game does not import it.
bool hook_import(const char* dll_prefix, const char* name, WORD ordinal, void* hook, void** original) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    const size_t prefix = std::strlen(dll_prefix);
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        const char* dll = reinterpret_cast<const char*>(base + d->Name);
        if (_strnicmp(dll, dll_prefix, prefix) != 0 || !d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            bool match = false;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                match = ordinal != 0 && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal;
            } else {
                const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                match = std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) == 0;
            }
            if (!match) continue;
            DWORD old = 0;
            void** slot = reinterpret_cast<void**>(&iat->u1.Function);
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *original = *slot;
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), hook);
            VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// The game's keyboard also goes through user32 (its menus): GetKeyboardState and GetAsyncKeyState, called through the
// same imports, see no key while the page has the controls.
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
GetKeyboardStateFn g_get_keyboard_state = nullptr;
GetAsyncKeyStateFn g_get_async_key_state = nullptr;

BOOL WINAPI hk_get_keyboard_state(PBYTE keys) {
    const BOOL ok = g_get_keyboard_state(keys);
    if (ok && keys && g_blank) std::memset(keys, 0, 256);
    return ok;
}

SHORT WINAPI hk_get_async_key_state(int vk) {
    const SHORT state = g_get_async_key_state(vk);
    return g_blank ? 0 : state;
}

bool hook_xinput() {
    void* original = nullptr;
    if (!hook_import("xinput", "XInputGetState", 2, reinterpret_cast<void*>(&hk_xinput_get_state), &original)) {
        return false;
    }
    g_xinput_get_state = reinterpret_cast<XInputGetStateFn>(original);
    LOG_INFO("FPS Unlock Settings: the controller through the game's import of XInputGetState");
    return true;
}

void hook_user32_keys() {
    void* original = nullptr;
    const bool a = hook_import("user32", "GetKeyboardState", 0, reinterpret_cast<void*>(&hk_get_keyboard_state),
                               &original);
    if (a) g_get_keyboard_state = reinterpret_cast<GetKeyboardStateFn>(original);
    const bool b = hook_import("user32", "GetAsyncKeyState", 0, reinterpret_cast<void*>(&hk_get_async_key_state),
                               &original);
    if (b) g_get_async_key_state = reinterpret_cast<GetAsyncKeyStateFn>(original);
    LOG_INFO("FPS Unlock Settings: the game's user32 keyboard reads watched (GetKeyboardState %s, GetAsyncKeyState %s)",
             a ? "yes" : "no", b ? "yes" : "no");
}

// ---------------------------------------------------------------- the System menu's screens
// The PC settings screen's method table slot 16 (+0x80), the screen's own method, runs once a frame while the screen is
// up and not otherwise (counted with `pcprobe` on 2026-11-23: slots 16, 19 and 31 once a frame, 23 and 25 twice, 15
// forty times, all only while it is up). Every screen of the System menu has the same layout (slots 15 and 19 their
// base class's, 16 each its own), so each one's slot 16 is counted (the user, 2026-11-26: the page from the whole
// System menu, as in Prepare to Die Edition). Each goes through a thunk that only counts (lock inc [count];
// jmp [original]: no register changed, whatever the method takes); the counts are watched every frame.
constexpr int kScreenSlot = 16;
struct SystemScreen {
    const char* rtti;
    const char* what;
};
constexpr SystemScreen kSystemScreens[] = {
    {".?AVFrpgMenuDlgSystem@NS_FRPG@@", "System list"},
    {".?AVFrpgMenuDlgEnvOption@NS_FRPG@@", "game options"},
    {".?AVFrpgMenuDlgDispOption@NS_FRPG@@", "display"},
    {".?AVFrpgMenuDlgSoundOption@NS_FRPG@@", "sound"},
    {".?AVFrpgMenuDlgPcOption@NS_FRPG@@", "PC settings"},
    {".?AVFrpgMenuDlgKeyOption@NS_FRPG@@", "key config"},
    {".?AVFrpgMenuDlgNetworkOption@NS_FRPG@@", "network"},
};
constexpr int kScreenCount = sizeof(kSystemScreens) / sizeof(kSystemScreens[0]);
volatile LONG* g_screen_counts = nullptr;  // one a screen (0 for a screen not found)
LONG g_screen_seen_calls[kScreenCount] = {};
LONG g_screen_report[kScreenCount] = {};
ULONGLONG g_screen_seen = 0;

const uint8_t* find_bytes(const uint8_t* from, const uint8_t* to, const void* what, size_t n) {
    const auto* w = static_cast<const uint8_t*>(what);
    for (const uint8_t* p = from; p + n <= to; ++p) {
        if (*p == w[0] && std::memcmp(p, w, n) == 0) return p;
    }
    return nullptr;
}

// The method table of the class named NAME (".?AV...@@"): its type descriptor holds the name 0x10 bytes in; the
// complete object locator (signature 1, offset 0) points at the descriptor and at itself (image-relative); the method
// table follows the pointer to the locator.
void** find_vtable(const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const IMAGE_SECTION_HEADER* rdata = nullptr;
    const uint8_t* text = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".rdata", 6) == 0) rdata = &sec[i];
    }
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !text; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE) continue;
        const uint8_t* s = base + sec[i].VirtualAddress;
        text = find_bytes(s, s + sec[i].Misc.VirtualSize, name, std::strlen(name) + 1);
    }
    if (!text || !rdata) return nullptr;
    const uint32_t td = static_cast<uint32_t>(text - 0x10 - base);
    const uint8_t* r0 = base + rdata->VirtualAddress;
    const uint8_t* r1 = r0 + rdata->Misc.VirtualSize;
    for (const uint8_t* p = r0; p + 24 <= r1; p += 4) {
        uint32_t col[6];
        std::memcpy(col, p, sizeof(col));
        if (col[0] != 1 || col[1] != 0 || col[3] != td || col[5] != static_cast<uint32_t>(p - base)) continue;
        const uint64_t at = reinterpret_cast<uint64_t>(p);
        for (const uint8_t* q = r0; q + 16 <= r1; q += 8) {
            uint64_t v = 0;
            std::memcpy(&v, q, 8);
            if (v == at) return reinterpret_cast<void**>(const_cast<uint8_t*>(q + 8));
        }
    }
    return nullptr;
}

// Diagnostics: every slot of the screen's method table counted (a thunk per slot: lock inc [count]; jmp [original],
// no register touched), to see which ones run while the screen is up. `pcprobe` installs, `pcreport` logs.
void** g_pc_vtable = nullptr;
constexpr int kProbeSlots = 48;
volatile LONG* g_probe_counts = nullptr;
LONG g_probe_last[kProbeSlots] = {};

void probe_install() {
    if (!g_pc_vtable || g_probe_counts) return;
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!page) return;
    auto* counts = reinterpret_cast<volatile LONG*>(page + 0x1000);
    auto* originals = reinterpret_cast<void**>(page + 0x1400);
    for (int i = 0; i < kProbeSlots; ++i) {
        uint8_t* code = page + i * 16;
        originals[i] = g_pc_vtable[i];
        const int32_t to_count = static_cast<int32_t>(reinterpret_cast<uint8_t*>(const_cast<LONG*>(&counts[i])) - (code + 7));
        code[0] = 0xF0; code[1] = 0xFF; code[2] = 0x05;  // lock inc dword ptr [rip+x]
        std::memcpy(code + 3, &to_count, 4);
        const int32_t to_orig = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&originals[i]) - (code + 13));
        code[7] = 0xFF; code[8] = 0x25;  // jmp qword ptr [rip+x]
        std::memcpy(code + 9, &to_orig, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    DWORD old = 0;
    if (!VirtualProtect(g_pc_vtable, kProbeSlots * sizeof(void*), PAGE_READWRITE, &old)) return;
    for (int i = 0; i < kProbeSlots; ++i) g_pc_vtable[i] = page + i * 16;
    VirtualProtect(g_pc_vtable, kProbeSlots * sizeof(void*), old, &old);
    g_probe_counts = counts;
    LOG_INFO("[pcprobe] %d slots counted", kProbeSlots);
}

void probe_report() {
    if (!g_probe_counts) return;
    std::string line;
    for (int i = 0; i < kProbeSlots; ++i) {
        const LONG n = g_probe_counts[i];
        if (n != g_probe_last[i]) line += " [" + std::to_string(i) + "]+" + std::to_string(n - g_probe_last[i]);
        g_probe_last[i] = n;
    }
    LOG_INFO("[pcprobe] since last:%s", line.empty() ? " none" : line.c_str());
}

void** safe_find_vtable(const char* name) {
    __try {
        return find_vtable(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// One page: a 16-byte thunk a screen, then their counts (+0x1000) and the original methods (+0x1400).
int hook_system_screens() {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) return 0;
    auto* counts = reinterpret_cast<volatile LONG*>(page + 0x1000);
    auto* originals = reinterpret_cast<void**>(page + 0x1400);
    void** slots[kScreenCount] = {};
    std::string found, missing;
    for (int i = 0; i < kScreenCount; ++i) {
        void** vtable = safe_find_vtable(kSystemScreens[i].rtti);
        if (!vtable) {
            missing += std::string(missing.empty() ? "" : ", ") + kSystemScreens[i].what;
            continue;
        }
        if (i == 4) g_pc_vtable = vtable;  // `pcprobe`
        slots[i] = vtable + kScreenSlot;
        originals[i] = *slots[i];
        uint8_t* code = page + i * 16;
        const int32_t to_count =
            static_cast<int32_t>(reinterpret_cast<uint8_t*>(const_cast<LONG*>(&counts[i])) - (code + 7));
        const int32_t to_orig = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&originals[i]) - (code + 13));
        code[0] = 0xF0;  // lock inc dword ptr [rip+to_count]
        code[1] = 0xFF;
        code[2] = 0x05;
        std::memcpy(code + 3, &to_count, 4);
        code[7] = 0xFF;  // jmp qword ptr [rip+to_orig]
        code[8] = 0x25;
        std::memcpy(code + 9, &to_orig, 4);
        found += std::string(found.empty() ? "" : ", ") + kSystemScreens[i].what;
    }
    DWORD old = 0;
    if (found.empty() || !VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old)) return 0;
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    int hooked = 0;
    for (int i = 0; i < kScreenCount; ++i) {
        if (!slots[i] || !VirtualProtect(slots[i], sizeof(void*), PAGE_READWRITE, &old)) continue;
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slots[i]), page + i * 16);
        VirtualProtect(slots[i], sizeof(void*), old, &old);
        ++hooked;
    }
    g_screen_counts = counts;
    LOG_INFO("FPS Unlock Settings: the System menu's screens watched (slot %d of each): %s%s%s", kScreenSlot,
             found.c_str(), missing.empty() ? "" : "; not found: ", missing.c_str());
    return hooked;
}

// Diagnostics (`screens`): which screens' methods ran since the last report.
void screens_report() {
    std::string line;
    for (int i = 0; i < kScreenCount && g_screen_counts; ++i) {
        const LONG n = g_screen_counts[i];
        if (n != g_screen_report[i]) {
            line += std::string(" ") + kSystemScreens[i].what + " +" + std::to_string(n - g_screen_report[i]);
        }
        g_screen_report[i] = n;
    }
    LOG_INFO("[screens] since last:%s", line.empty() ? " none" : line.c_str());
}

// ---------------------------------------------------------------- the keyboard
int dik_of(int vk) {
    static const uint8_t kLetters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                         0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (vk >= 'A' && vk <= 'Z') return kLetters[vk - 'A'];
    if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= VK_F1 && vk <= VK_F10) return 0x3B + (vk - VK_F1);
    switch (vk) {
        case VK_F11: return 0x57;
        case VK_F12: return 0x58;
        case VK_ESCAPE: return 0x01;
        case VK_BACK: return 0x0E;
        case VK_RETURN: return 0x1C;
        case VK_UP: return 0xC8;
        case VK_DOWN: return 0xD0;
        case VK_LEFT: return 0xCB;
        case VK_RIGHT: return 0xCD;
        case VK_PRIOR: return 0xC9;
        case VK_NEXT: return 0xD1;
        case VK_SPACE: return 0x39;
        case VK_OEM_MINUS: return 0x0C;
        default: return 0;
    }
}

bool key(int vk) {
    if (vk == VK_SHIFT) return proxy_key(0x2A) || proxy_key(0x36);
    if (vk == VK_RETURN && proxy_key(0x9C)) return true;  // the keypad's Enter
    return proxy_key(dik_of(vk));
}

bool game_window_focused() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void blank(bool b) {
    InterlockedExchange(&g_blank, b ? 1 : 0);
    proxy_set_blank(b);
}

// ---------------------------------------------------------------- diagnostics: commands in fpsu-dsr-cmd.txt
// One per line, read and deleted every quarter second: shot NAME (NAME.bmp in the game folder), open 0|1, rate.
ULONGLONG g_cmd_at = 0;
void poll_commands() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_cmd_at) return;
    g_cmd_at = now + 250;
    const std::wstring file = g_dir + L"fpsu-dsr-cmd.txt";
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return;
    char line[256];
    std::string all;
    while (std::fgets(line, sizeof(line), f)) all += line;
    std::fclose(f);
    DeleteFileW(file.c_str());
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string cmd = all.substr(at, end - at);
        while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
        at = end + 1;
        if (cmd.rfind("shot ", 0) == 0) {
            const std::string name = cmd.substr(5);
            overlay_request_shot((g_dir + std::wstring(name.begin(), name.end()) + L".bmp").c_str());
            LOG_INFO("[cmd] shot %s", name.c_str());
        } else if (cmd == "open 1" || cmd == "open 0") {
            settingspage_set_open(cmd == "open 1");
        } else if (cmd == "pcprobe") {
            probe_install();
        } else if (cmd == "pcreport") {
            probe_report();
        } else if (cmd == "screens") {
            screens_report();
        } else if (cmd == "footik npcs 1" || cmd == "footik npcs 0") {
            g_foot_ik_npcs.store(cmd.back() == '1' ? 1 : 0, std::memory_order_relaxed);
            LOG_INFO("[cmd] FootIkNpcs %c", cmd.back());
        } else if (cmd == "footik") {
            LOG_INFO("[cmd] FootIkNpcs %d: %lld calls placed another character's feet, %lld left to the game (another "
                     "player's character)", g_foot_ik_npcs.load(), g_foot_ik_npcs_placed.load(),
                     g_foot_ik_npcs_players.load());
        } else if (cmd == "rate") {
            LOG_INFO("[cmd] rate %.1f presents a second (cap %u)", overlay_present_rate(), g_target_fps.load());
        } else if (!cmd.empty()) {
            LOG_INFO("[cmd] unknown: %s", cmd.c_str());
        }
    }
}

// ---------------------------------------------------------------- every frame (the game's render thread)
void frame(int width, int height) {
    if (FPSU_DIAGNOSTICS) poll_commands();
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kScreenCount && g_screen_counts; ++i) {
        const LONG calls = g_screen_counts[i];
        if (calls != g_screen_seen_calls[i]) {
            g_screen_seen_calls[i] = calls;
            g_screen_seen = now;
        }
    }
    const bool system_screen = g_screen_seen && now - g_screen_seen < 300;
    SettingsPageInput in;
    for (int i = 0; i < 4; ++i) {
        in.pad = static_cast<uint16_t>(in.pad | static_cast<uint16_t>(g_pad_buttons[i]));
        const LONG st = g_pad_stick[i];
        const int x = static_cast<int16_t>(st & 0xFFFF), y = static_cast<int16_t>((st >> 16) & 0xFFFF);
        if (std::abs(x) > std::abs(in.lx)) in.lx = x;
        if (std::abs(y) > std::abs(in.ly)) in.ly = y;
    }
    in.keyboard = game_window_focused();
    in.key = key;
    in.pad_opens = system_screen;
    settingspage_frame(in);
    if (settingspage_is_open()) {
        const SettingsPageImage* img = settingspage_panel(width, height);
        if (img) overlay_draw(0, img, (width - img->w) / 2, (height - img->h) / 2);
    } else if (system_screen) {
        std::wstring line = L"Y: FPS Unlock Settings";
        const int k = settingspage_key();
        if (k >= VK_F1 && k <= VK_F12) line += L"   (keyboard: F" + std::to_wstring(k - VK_F1 + 1) + L")";
        const SettingsPageImage* img = settingspage_hint(width, height, line.c_str());
        if (img) overlay_draw(1, img, (width - img->w) / 2, height - img->h - height / 54);
    }
}

}  // namespace

void settingsmenu_install(const wchar_t* dll_path, const Settings& settings) {
    if (!settings.settings_menu) {
        LOG_INFO("FPS Unlock Settings: off (SettingsMenu = false); the game's swap chain is left alone");
        return;
    }
    wchar_t dir[MAX_PATH];
    lstrcpynW(dir, dll_path, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;
    g_dir = dir;
    g_effects_mod = settings_effects_mod_installed(dir);
    settingspage_init((g_dir + L"DSR-FPS-Unlock.ini").c_str(), settings.settings_menu_key, blank);
    settingspage_add_page(L"Frame rate", kFrame, _countof(kFrame));
    settingspage_add_page(L"Movement", kMovement, _countof(kMovement));
    settingspage_add_page(L"Timing", kTiming, _countof(kTiming));
    const bool pad = hook_xinput();
    hook_user32_keys();
    const bool pc = hook_system_screens() > 0;
    if (!overlay_install(frame)) return;
    char k[16] = "none";
    const int key_vk = settings.settings_menu_key;
    if (key_vk >= VK_F1 && key_vk <= VK_F24) std::snprintf(k, sizeof(k), "F%d", key_vk - VK_F1 + 1);
    LOG_INFO("FPS Unlock Settings: open with %s%s%s", k,
             pad && pc ? ", or Y on a controller in the System menu" : "",
             !pad ? " (the controller's import was not found: keyboard only)"
             : !pc ? " (the System menu's screens were not found: the key only)"
                   : "");
}
