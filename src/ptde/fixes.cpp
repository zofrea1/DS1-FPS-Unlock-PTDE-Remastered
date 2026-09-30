#include "fixes.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

// Values the patched loads read. Doubles are 8-byte aligned by the compiler; floats 4.
alignas(8) volatile double g_timer_dt = 1.0 / 30.0;
alignas(8) volatile double g_smooth = 1.0 / 30.0;
alignas(8) volatile double g_damp = 0.95;
alignas(4) volatile float g_gravity = 1.0f;
alignas(4) volatile float g_friction = 0.65f;

struct Site {
    uint32_t va;         // instruction address
    uint32_t operand;    // offset of the abs32 operand inside the instruction
    uint32_t original;   // the address the game's own instruction loads from
    const volatile void* cave;
    int group;           // 0 slide, 1 damping, 2 timers, 3 smoothing
    bool applied;
};

Site g_sites[] = {
    {0x00EC0F8D, 4, 0x012DF970, &g_gravity, 0, false},    // movss xmm0,[1.0]   slope gravity per frame
    {0x00EC13A6, 4, 0x012DF974, &g_friction, 0, false},   // movss xmm0,[0.65]  ground friction per frame
    {0x00EC184F, 4, 0x011E8388, &g_damp, 1, false},       // movsd xmm2,[0.95]  airborne damping per frame
    {0x00DF2C55, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // addsd  fade timer += 1/30
    {0x00DF2C7A, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // subsd  fade timer -= 1/30
    {0x00CFB2B6, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // subsd  countdown -= 1/30
    {0x00EC7769, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // addsd  timer + 1/30
    {0x00F9179D, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // movsd  countdown step
    {0x00F0B556, 4, 0x011E7CF0, &g_smooth, 3, false},     // movsd  lerp factor (camera-like smoothing)
};

bool g_enabled[4] = {true, true, true, true};
bool g_keys_down[4] = {};
bool g_installed = false;

bool write_operand(Site& s, uint32_t address) {
    auto* p = reinterpret_cast<uint8_t*>(s.va + s.operand);
    DWORD old = 0;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, &address, 4);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    DWORD ignored = 0;
    VirtualProtect(p, 4, old, &ignored);
    return true;
}

bool current_operand(const Site& s, uint32_t* out) {
    std::memcpy(out, reinterpret_cast<const void*>(s.va + s.operand), 4);
    return true;
}

void set_group(int group, bool on) {
    for (auto& s : g_sites) {
        if (s.group != group) continue;
        uint32_t cur = 0;
        current_operand(s, &cur);
        const uint32_t want = on ? reinterpret_cast<uint32_t>(s.cave) : s.original;
        if (cur == want) continue;
        if (cur != s.original && cur != reinterpret_cast<uint32_t>(s.cave)) continue;  // not ours
        if (write_operand(s, want)) s.applied = on;
    }
}

}  // namespace

bool fixes_install(const FixFlags& flags) {
    g_enabled[0] = flags.slide;
    g_enabled[1] = flags.damping;
    g_enabled[2] = flags.timers;
    g_enabled[3] = flags.smoothing;
    int ok = 0, bad = 0;
    for (auto& s : g_sites) {
        uint32_t cur = 0;
        current_operand(s, &cur);
        if (cur != s.original) {
            LOG_ERROR("Fix site %08X: operand is %08X, expected %08X. Skipped.", s.va, cur, s.original);
            s.group = -1;
            ++bad;
            continue;
        }
        ++ok;
    }
    for (int g = 0; g < 4; ++g) set_group(g, g_enabled[g]);
    g_installed = true;
    LOG_INFO("Per-frame fixes: %d sites verified, %d skipped. slide=%d damping=%d timers=%d smoothing=%d", ok, bad,
             g_enabled[0], g_enabled[1], g_enabled[2], g_enabled[3]);
    return bad == 0;
}

void fixes_update(double dt) {
    if (!g_installed) return;
    const double s = dt * 30.0;
    g_gravity = static_cast<float>(1.0 * s);
    g_friction = static_cast<float>(std::pow(0.65, s));
    g_damp = std::pow(0.95, s);
    g_timer_dt = dt;
    g_smooth = 1.0 - std::pow(1.0 - 1.0 / 30.0, s);
}

void fixes_poll_hotkeys() {
    if (!g_installed) return;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    for (int i = 0; i < 4; ++i) {
        const bool down = ctrl && (GetAsyncKeyState('1' + i) & 0x8000) != 0;
        if (down && !g_keys_down[i]) {
            g_enabled[i] = !g_enabled[i];
            set_group(i, g_enabled[i]);
            static const char* const names[4] = {"slide", "damping", "timers", "smoothing"};
            LOG_INFO("Fix group '%s' is now %s", names[i], g_enabled[i] ? "ON" : "OFF");
        }
        g_keys_down[i] = down;
    }
}
