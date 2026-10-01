#include "ui.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <TlHelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>

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

// ---- HUD gauge follower ------------------------------------------------------------------------
//
// fn 0xC88BA0 (called with eax = the gauge element) moves a displayed value ([+0x3C]) toward its
// target by a fixed amount per call: [+0x4C] when it has to rise, [+0x44] ([+0x48] for the second
// value) when it has to fall; a gap smaller than the step snaps. The player's health bar and its
// trailing ghost fill through this, a step of about 0.076 of the bar per frame, so an Estus heal
// finished in 50 ms at 120 FPS. The three steps are multiplied by frame time * 30 for the duration
// of each call (and put back afterwards), which is the same animation in real time.
constexpr uint32_t kGaugeFollow = 0x00C88BA0;
constexpr size_t kGaugeStolen = 5;  // movss xmm3, [eax+44h]
void* g_gauge_follow_tramp = nullptr;
float g_gauge_saved[3] = {};

void __stdcall gauge_scale_enter(uint8_t* gauge) {
    float s = static_cast<float>(fixes_last_dt() * 30.0);
    if (s < 0.02f) s = 0.02f;
    if (s > 4.0f) s = 4.0f;
    float* steps[3] = {reinterpret_cast<float*>(gauge + 0x44), reinterpret_cast<float*>(gauge + 0x48),
                       reinterpret_cast<float*>(gauge + 0x4C)};
    for (int i = 0; i < 3; ++i) {
        g_gauge_saved[i] = *steps[i];
        *steps[i] = g_gauge_saved[i] * s;
    }
}

void __stdcall gauge_scale_leave(uint8_t* gauge) {
    *reinterpret_cast<float*>(gauge + 0x44) = g_gauge_saved[0];
    *reinterpret_cast<float*>(gauge + 0x48) = g_gauge_saved[1];
    *reinterpret_cast<float*>(gauge + 0x4C) = g_gauge_saved[2];
}

__declspec(naked) void gauge_follow_hook() {
    __asm {
        push eax
        push eax
        call gauge_scale_enter
        mov eax, dword ptr [esp]
        call dword ptr [g_gauge_follow_tramp]
        push eax
        call gauge_scale_leave
        pop eax
        ret
    }
}

bool install_gauge_follow() {
    auto* site = reinterpret_cast<uint8_t*>(kGaugeFollow);
    static const uint8_t kExpect[kGaugeStolen] = {0xF3, 0x0F, 0x10, 0x58, 0x44};
    if (std::memcmp(site, kExpect, kGaugeStolen) != 0) {
        LOG_ERROR("Gauge follower at %08X does not match this build. Not patching.", kGaugeFollow);
        return false;
    }
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, site, kGaugeStolen);
    tramp[kGaugeStolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((kGaugeFollow + kGaugeStolen) - reinterpret_cast<uintptr_t>(tramp + kGaugeStolen + 5));
    std::memcpy(tramp + kGaugeStolen + 1, &back, 4);
    g_gauge_follow_tramp = tramp;
    DWORD old = 0;
    if (!VirtualProtect(site, kGaugeStolen, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&gauge_follow_hook) - (kGaugeFollow + 5));
    std::memcpy(site + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), site, kGaugeStolen);
    DWORD ignored = 0;
    VirtualProtect(site, kGaugeStolen, old, &ignored);
    LOG_INFO("HUD gauge fill follows the frame time (health bar animation, site %08X)", kGaugeFollow);
    return true;
}

// ---- Sprint stamina tick ------------------------------------------------------------------------
//
// The character update (fn 0xE80A00) keeps a timer at [chr+0x168]: while the character spends
// stamina it adds the frame time, and when the sum reaches the interval (0.1 s, the float at
// 0x12DF014) it takes one point off the stamina ([chr+0x2E4], through 0xE67960) and then STORES ZERO
// in the timer (movss [ebx+168h], xmm2 at 0xE80B00, xmm2 being 0). The leftover of the last frame
// is thrown away, so each tick lasts 0.1 s rounded up to a whole number of frames: at awkward frame
// rates the drain runs up to a frame per tick slow. That store becomes a call that subtracts the
// interval from the timer instead, which makes the drain 10 points a second at any frame rate.
constexpr uint32_t kTickStoreSite = 0x00E80B00;
constexpr uint32_t kTickInterval = 0x012DF014;

bool install_stamina_tick() {
    static const uint8_t kExpect[8] = {0xF3, 0x0F, 0x11, 0x93, 0x68, 0x01, 0x00, 0x00};
    auto* site = reinterpret_cast<uint8_t*>(kTickStoreSite);
    if (std::memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        LOG_ERROR("Sprint stamina tick at %08X does not match this build. Not patching.", kTickStoreSite);
        return false;
    }
    const float interval = *reinterpret_cast<const float*>(kTickInterval);
    if (!(interval >= 0.05f && interval <= 0.5f)) {
        LOG_ERROR("Sprint stamina tick interval is %.4f, not what was expected. Not patching.", interval);
        return false;
    }
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    size_t n = 0;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) stub[n++] = b;
    };
    emit({0x9C});                                      // pushfd (the jg after the call needs the flags)
    emit({0xD9, 0x83, 0x68, 0x01, 0x00, 0x00});        // fld  dword ptr [ebx+168h]
    emit({0xD8, 0x25});                                // fsub dword ptr [interval]
    const uint32_t address = kTickInterval;
    std::memcpy(stub + n, &address, 4);
    n += 4;
    emit({0xD9, 0x9B, 0x68, 0x01, 0x00, 0x00});        // fstp dword ptr [ebx+168h]
    emit({0x9D});                                      // popfd
    emit({0xC3});                                      // ret
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(kExpect), PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (kTickStoreSite + 5));
    site[0] = 0xE8;
    std::memcpy(site + 1, &rel, 4);
    site[5] = site[6] = site[7] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kExpect));
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    DWORD ignored = 0;
    VirtualProtect(site, sizeof(kExpect), old, &ignored);
    LOG_INFO("Sprint stamina drain keeps the leftover of each tick (interval %.3f s)", interval);
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
    uint32_t last_other[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    while (GetModuleHandleW(nullptr)) {
        Sleep(1);
        uint32_t manager = 0, data = 0, hp = 0, max_hp = 0;
        if (!read_u32(kGameDataPtr, &manager) || manager < 0x10000) continue;
        if (!read_u32(manager + 8, &data) || data < 0x10000) continue;
        if (!read_u32(data + 0xC, &hp) || !read_u32(data + 0x10, &max_hp)) continue;
        {
            // The words next to the hit points: stamina and its maximum are among them.
            uint32_t other[4] = {};
            bool ok = true;
            for (int i = 0; i < 4; ++i) ok = ok && read_u32(data + 0x24 + 4 * i, &other[i]);
            if (ok && std::memcmp(other, last_other, sizeof(other)) != 0) {
                std::memcpy(last_other, other, sizeof(other));
                LOG_INFO("[stam] +24=%u +28=%u +2C=%u +30=%u", other[0], other[1], other[2], other[3]);
            }
        }
        if (static_cast<int>(hp) != last) {
            last = static_cast<int>(hp);
            LOG_INFO("[hp] hp=%u max=%u", hp, max_hp);
        }
    }
    return 0;
}

// ---- Dialog time step log ---------------------------------------------------------------------
//
// Diagnostic: fn 0xC303A0 is the base update every HUD and menu dialog runs once per frame with
// a time step. Once a second this logs, for the dialogs that ran, how many updates they got and
// how much time they were given in total: a dialog given the real frame time sums to about 1.0
// per second at any frame rate.
constexpr uint32_t kDialogUpdate = 0x00C303A0;
constexpr size_t kDialogStolen = 5;  // fld dword ptr [esp+4]; push esi

using DialogFn = void(__fastcall*)(void* self, void* edx, float dt);
DialogFn g_dialog_orig = nullptr;

struct DialogStat {
    void* self = nullptr;
    uint32_t calls = 0;
    double sum = 0.0;
    float last = 0.0f;
};
DialogStat g_dialogs[64];
LARGE_INTEGER g_dialog_freq{}, g_dialog_mark{};

void __fastcall hk_dialog(void* self, void* edx, float dt) {
    const size_t start = (reinterpret_cast<uintptr_t>(self) >> 4) & 63;
    for (size_t i = 0; i < 64; ++i) {
        DialogStat& d = g_dialogs[(start + i) & 63];
        if (!d.self) d.self = self;
        if (d.self == self) {
            ++d.calls;
            d.sum += dt;
            d.last = dt;
            break;
        }
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_dialog_mark.QuadPart == 0) g_dialog_mark = now;
    const double wall = static_cast<double>(now.QuadPart - g_dialog_mark.QuadPart) / static_cast<double>(g_dialog_freq.QuadPart);
    if (wall >= 1.0) {
        static int lines = 0;
        if (lines < 400) {
            for (DialogStat& d : g_dialogs) {
                if (d.self && d.calls > 3) {
                    ++lines;
                    LOG_INFO("[dlgdt] obj=%p calls=%u given=%.3f s over %.3f s wall (last dt %.5f)", d.self, d.calls, d.sum, wall,
                             d.last);
                }
                d = DialogStat{};
            }
        } else {
            for (DialogStat& d : g_dialogs) d = DialogStat{};
        }
        g_dialog_mark = now;
    }
    g_dialog_orig(self, edx, dt);
}

bool install_dialog_log() {
    auto* site = reinterpret_cast<uint8_t*>(kDialogUpdate);
    static const uint8_t kExpect[kDialogStolen] = {0xD9, 0x44, 0x24, 0x04, 0x56};
    if (std::memcmp(site, kExpect, kDialogStolen) != 0) {
        LOG_ERROR("Dialog update at %08X does not match this build. Dialog log off.", kDialogUpdate);
        return false;
    }
    QueryPerformanceFrequency(&g_dialog_freq);
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, site, kDialogStolen);
    tramp[kDialogStolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((kDialogUpdate + kDialogStolen) - reinterpret_cast<uintptr_t>(tramp + kDialogStolen + 5));
    std::memcpy(tramp + kDialogStolen + 1, &back, 4);
    g_dialog_orig = reinterpret_cast<DialogFn>(tramp);
    DWORD old = 0;
    if (!VirtualProtect(site, kDialogStolen, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_dialog) - (kDialogUpdate + 5));
    std::memcpy(site + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), site, kDialogStolen);
    DWORD ignored = 0;
    VirtualProtect(site, kDialogStolen, old, &ignored);
    LOG_INFO("Dialog log: [dlgdt] lines once a second");
    return true;
}

// ---- Stamina writer probe -----------------------------------------------------------------------
//
// Diagnostic (INI InputLog): Scroll Lock puts a hardware write watchpoint on the player's stamina
// word ([[0x1378700]+8]+0x28). The routine that writes it copies the value from the character's
// own data, so at every hit the registers are searched for a pointer to memory holding the same
// number; the (register, offset) that matches at every hit, including hits with different values,
// is the character's own stamina word, which then gets a second watchpoint. The writers of both are
// summarised in the log as [stamW] lines, with the return addresses found on the stack.
struct ProbeHit {
    uint32_t eip;
    uint32_t ret[3];
    uint32_t reg[7];  // eax ebx ecx edx esi edi ebp
    int value;
    int slot;
};
constexpr int kProbeMaxHits = 8192;
constexpr int kMatchWords = 512;  // 0x800 bytes behind each register
ProbeHit g_probe_hits[kProbeMaxHits];
std::atomic<int> g_probe_count{0};
uint32_t g_probe_address[2] = {0, 0};
uint16_t g_probe_match[7][kMatchWords];
int g_probe_slot0_hits = 0;
int g_probe_min = 1 << 30, g_probe_max = -(1 << 30);
uint32_t g_probe_first_regs[7] = {};
volatile LONG g_probe_second_armed = 0;

LONG CALLBACK on_probe(EXCEPTION_POINTERS* info) {
    CONTEXT* ctx = info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ctx->Dr6 & 3)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const int which = (ctx->Dr6 & 1) ? 0 : 1;
    const int slot = g_probe_count.fetch_add(1, std::memory_order_relaxed);
    if (slot < kProbeMaxHits) {
        ProbeHit& h = g_probe_hits[slot];
        h.eip = ctx->Eip;
        h.slot = which;
        h.reg[0] = ctx->Eax;
        h.reg[1] = ctx->Ebx;
        h.reg[2] = ctx->Ecx;
        h.reg[3] = ctx->Edx;
        h.reg[4] = ctx->Esi;
        h.reg[5] = ctx->Edi;
        h.reg[6] = ctx->Ebp;
        h.value = *reinterpret_cast<const int*>(g_probe_address[which]);
        const uint32_t* stack = reinterpret_cast<const uint32_t*>(ctx->Esp);
        int found = 0;
        for (int i = 0; i < 3; ++i) h.ret[i] = 0;
        for (int i = 0; i < 48 && found < 3; ++i) {
            uint32_t v = 0;
            __try {
                v = stack[i];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (v > 0x401000 && v < 0x1200000) h.ret[found++] = v;
        }
        if (which == 0 && g_probe_slot0_hits < 1500) {
            if (g_probe_slot0_hits == 0) std::memcpy(g_probe_first_regs, h.reg, sizeof(h.reg));
            ++g_probe_slot0_hits;
            if (h.value < g_probe_min) g_probe_min = h.value;
            if (h.value > g_probe_max) g_probe_max = h.value;
            for (int r = 0; r < 7; ++r) {
                const uint32_t p = h.reg[r];
                if (p < 0x10000 || p > 0x7FFF0000) continue;
                __try {
                    const int* words = reinterpret_cast<const int*>(p);
                    for (int i = 0; i < kMatchWords; ++i) {
                        if (words[i] == h.value) ++g_probe_match[r][i];
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    }
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void probe_arm(int slot, uint32_t address) {
    g_probe_address[slot] = address;
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 e{};
    e.dwSize = sizeof(e);
    int applied = 0;
    if (Thread32First(snap, &e)) {
        do {
            if (e.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, e.th32ThreadID);
            if (!th) continue;
            const bool me = e.th32ThreadID == self;
            if (me || SuspendThread(th) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &ctx)) {
                    if (slot == 0) {
                        ctx.Dr0 = address;
                        ctx.Dr7 |= 1u | (1u << 16) | (3u << 18);
                    } else {
                        ctx.Dr1 = address;
                        ctx.Dr7 |= (1u << 2) | (1u << 20) | (3u << 22);
                    }
                    ctx.Dr6 = 0;
                    if (SetThreadContext(th, &ctx)) ++applied;
                }
                if (!me) ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &e));
    }
    CloseHandle(snap);
    LOG_INFO("[stamW] write watchpoint %d on %08X set on %d threads", slot, address, applied);
}

DWORD WINAPI probe_thread(void*) {
    bool key_was = false;
    for (;;) {
        Sleep(20);
        const bool key = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
        const bool pressed = key && !key_was;
        key_was = key;
        if (!pressed) continue;
        uint32_t manager = 0, data = 0;
        if (!read_u32(kGameDataPtr, &manager) || !read_u32(manager + 8, &data) || data < 0x10000) {
            LOG_ERROR("[stamW] player data not found");
            continue;
        }
        AddVectoredExceptionHandler(1, on_probe);
        probe_arm(0, data + 0x28);
        ULONGLONG last_report = GetTickCount64();
        int reported = 0;
        const ULONGLONG start = last_report;
        while (GetTickCount64() - start < 240000) {
            Sleep(50);
            if (!g_probe_second_armed && g_probe_slot0_hits >= 60 && g_probe_max - g_probe_min >= 3) {
                // Candidates: every (register, word) that held the written value at every single hit.
                const int needed = g_probe_slot0_hits - 2;
                int first_reg = -1, first_word = -1, listed = 0;
                for (int r = 0; r < 7; ++r) {
                    for (int i = 0; i < kMatchWords; ++i) {
                        if (g_probe_match[r][i] >= needed) {
                            const uint32_t address = g_probe_first_regs[r] + 4u * i;
                            if (address == data + 0x28) continue;
                            if (listed < 12) {
                                LOG_INFO("[stamW] candidate: register %d + 0x%X (%08X)", r, 4 * i, address);
                                ++listed;
                            }
                            if (first_reg < 0) {
                                first_reg = r;
                                first_word = i;
                            }
                        }
                    }
                }
                g_probe_second_armed = 1;
                if (first_reg >= 0) {
                    probe_arm(1, g_probe_first_regs[first_reg] + 4u * first_word);
                } else {
                    LOG_ERROR("[stamW] no character word matched; only the stats block is watched");
                }
            }
            if (GetTickCount64() - last_report > 15000 && g_probe_count.load() > reported) {
                last_report = GetTickCount64();
                reported = g_probe_count.load();
                const int total = reported < kProbeMaxHits ? reported : kProbeMaxHits;
                struct Key {
                    int slot;
                    uint32_t eip, r0, r1;
                    int count, vmin, vmax;
                } keys[32] = {};
                int nkeys = 0;
                for (int h = 0; h < total; ++h) {
                    const ProbeHit& hit = g_probe_hits[h];
                    int k = 0;
                    for (; k < nkeys; ++k) {
                        if (keys[k].slot == hit.slot && keys[k].eip == hit.eip && keys[k].r0 == hit.ret[0]) break;
                    }
                    if (k == nkeys) {
                        if (nkeys == 32) continue;
                        keys[nkeys++] = {hit.slot, hit.eip, hit.ret[0], hit.ret[1], 0, 1 << 30, -(1 << 30)};
                    }
                    ++keys[k].count;
                    if (hit.value < keys[k].vmin) keys[k].vmin = hit.value;
                    if (hit.value > keys[k].vmax) keys[k].vmax = hit.value;
                }
                LOG_INFO("[stamW] %d writes so far", total);
                for (int k = 0; k < nkeys; ++k) {
                    LOG_INFO("[stamW]   slot %d writer %08X  ret %08X %08X  x%d  values %d..%d", keys[k].slot, keys[k].eip,
                             keys[k].r0, keys[k].r1, keys[k].count, keys[k].vmin, keys[k].vmax);
                }
            }
        }
        LOG_INFO("[stamW] done");
    }
    return 0;
}

}  // namespace

bool ui_install(const Settings& settings) {
    bool ok = true;
    if (settings.fix_ui) {
        ok &= install_swirl();
        ok &= install_gauge_follow();
    }
    if (settings.fix_stamina_tick) {
        ok &= install_stamina_tick();
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
        if (HANDLE probe = CreateThread(nullptr, 0, probe_thread, nullptr, 0, nullptr)) {
            CloseHandle(probe);
            LOG_INFO("Stamina probe: Scroll Lock starts it");
        }
        HANDLE hp = CreateThread(nullptr, 0, hp_thread, nullptr, 0, nullptr);
        if (hp) {
            CloseHandle(hp);
        }
        ok &= install_input_log();
        ok &= install_dialog_log();
    }
    return ok;
}
