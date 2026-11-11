#include "fixes.h"

#include "log.h"
#include "camera.h"
#include "footik.h"
#include "snap.h"
#include "turn.h"

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
// A landed bullet's ground factor (see the sites below): 0.4 per 1/30 s, and what a landing stores so that the
// next update's decay leaves exactly 0.4 (1.0 at 30 FPS).
alignas(8) volatile double g_ground_decay = 0.4;
alignas(4) volatile float g_ground_land = 1.0f;
// The speed a ground-crawling bullet keeps at each landing: the bullet manager's 0.97, per 1/30 s.
alignas(4) volatile float g_land_keep = 0.97f;
alignas(8) volatile double g_rate = 30.0;
alignas(8) volatile double g_slow = 0.8;
alignas(8) volatile double g_recover = 1.2;
// HUD gauges (HP, stamina, boss and enemy bars): the displayed value chases the real one by
// half the gap per frame when it is more than 2 away, and by 1.0 per frame when closer.
alignas(8) volatile double g_gauge_k = 0.5;
alignas(4) volatile float g_gauge_min = 1.0f;
alignas(4) volatile float g_gravity = 1.0f;
alignas(4) volatile float g_friction = 0.65f;
// 1 / frame time, for velocities the game derives from one frame's movement (30 at 30 FPS).
alignas(8) volatile double g_inv_dt = 30.0;
// The ragdoll driver's pull on the root body per unit of distance from its pose: 30 per 30 FPS frame, scaled to the
// frame (900 * frame time), or 1 / frame time below 30 FPS so a long frame does not overshoot the pose (see below).
alignas(8) volatile double g_ragdoll_gain = 30.0;
// The item glow fade divisor: 60 frames at 30 FPS, so 2 / frame time.
alignas(8) volatile double g_shine_div = 60.0;
// A fixed 1/30 for code that copies the engine's 1/30 step into a setting instead of stepping with it.
alignas(4) volatile float g_fixed_step = 1.0f / 30.0f;

struct Site {
    uint32_t va;         // instruction address
    uint32_t operand;    // offset of the abs32 operand inside the instruction
    uint32_t original;   // the address the game's own instruction loads from
    const volatile void* cave;
    int group;           // 0 slide, 1 damping, 2 timers, 3 smoothing, 4 graze, 5 ui, 6 velocity, 7 fixed step
    bool applied;
};

Site g_sites[] = {
    {0x00EC0F8D, 4, 0x012DF970, &g_gravity, 0, false},    // movss xmm0,[1.0]   slope gravity per frame
    {0x00EC13A6, 4, 0x012DF974, &g_friction, 0, false},   // movss xmm0,[0.65]  ground friction per frame
    {0x00EC184F, 4, 0x011E8388, &g_damp, 1, false},       // movsd xmm2,[0.95]  airborne damping per frame
    // A ground-crawling bullet (BulletParam FollowType 4, the undead dragon's acid) that lands (0xDAA1D0) gets a
    // ground factor of 1 at [bullet+0x50], which holds its gravity off (gravity * (1 - factor)); the velocity
    // update (0xDA6CD0) multiplies the factor by 0.4 every frame and drops it to 0 at 0.1 or below. A sliding
    // glob lands again every frame, so at 30 FPS it always slides with 0.6 of its gravity. The decay follows the
    // frame time, and the landing stores 0.4 ^ (1 - dt * 30) instead of 1, so the update after a landing still
    // leaves 0.4 (0.6 of the gravity) at any frame rate, and a bounce fades by time.
    {0x00DA6DA8, 4, 0x011E8300, &g_ground_decay, 1, false},  // mulsd xmm1,[0.4] -> 0.4 ^ (dt * 30)
    {0x00DAC020, 4, 0x0115F59C, &g_ground_land, 1, false},   // movss xmm0,[1.0] -> 0.4 ^ (1 - dt * 30)
    {0x00DF2C55, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // addsd  fade timer += 1/30
    {0x00DF2C7A, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // subsd  fade timer -= 1/30
    {0x00CFB2B6, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // subsd  countdown -= 1/30
    {0x00EC7769, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // addsd  timer + 1/30
    {0x00F9179D, 4, 0x011E7CF0, &g_timer_dt, 2, false},   // movsd  countdown step
    {0x00F0B556, 4, 0x011E7CF0, &g_smooth, 3, false},     // movsd  lerp factor (camera-like smoothing)
    // Movement controller graze check (fn 0xE3AA..): speed = |position delta| * 30 per frame is
    // compared with 1; below it the walk-speed multiplier [ctrl+0x170] is multiplied by 0.8 each
    // frame (floor 0.5), otherwise by 1.2 (cap 1.0). At 240 FPS a normal run fails the test.
    {0x00E3ABA2, 4, 0x011E7CD8, &g_rate, 4, false},       // mulsd xmm0,[30.0]  -> 1 / dt
    {0x00E3ABCB, 4, 0x011E7DE8, &g_slow, 4, false},       // mulsd xmm0,[0.8]   -> 0.8 ^ (dt * 30)
    {0x00E3AC01, 4, 0x011E84F8, &g_recover, 4, false},    // mulsd xmm0,[1.2]   -> 1.2 ^ (dt * 30)
    // FrpgMenuDlgObjGauge update (fn 0xC89A40): displayed value moves toward the real one by
    // |gap| * 0.5 per frame while the gap is over 2, else by 1.0 per frame. Drinking an Estus
    // flask fills the bar through this, four times too fast at 120 FPS.
    {0x00C89A90, 4, 0x0115F59C, &g_gauge_min, 5, false},  // movss xmm2,[1.0]   -> 1.0 * dt * 30
    {0x00C89AC3, 4, 0x011E7C60, &g_gauge_k, 5, false},    // mulsd xmm2,[0.5]   -> 1 - 0.5 ^ (dt * 30)
    // ObjShineTreasureSlot (vtable slot 3, fn 0xE85380): the item glow fades in and out by [+0x18] / 60
    // per frame (two seconds at 30 FPS) and ignores its time argument.
    {0x00E853BE, 4, 0x011E7FF8, &g_shine_div, 2, false},  // divsd xmm1,[60.0]  -> 2 / dt
    {0x00E85525, 4, 0x011E7FF8, &g_shine_div, 2, false},  // divsd xmm1,[60.0]  -> 2 / dt
    // Velocities from one frame's movement times 30. Remastered changed both factors to 60.
    //  * 3D sound sources (fn 0xDEB3B0, 0xDF12E0): (position - last frame's position) * 30, the velocity
    //    FMOD uses for the Doppler shift.
    //  * Ragdoll driver (fn 0xEC63B0, from 0xEC6870 in ragdoll mode 2): the root body gets an impulse of
    //    mass * 30 * (pose - position) every update, through hkpMotion::applyLinearImpulse (vtable +0x4C,
    //    0x87E7C0: velocity += impulse / mass). It ADDS 30 * the gap to the velocity once per update: at 30 FPS
    //    that is 900 * the gap per second, a spring pulling the body to its pose. 1.3.0 to 2.0.1 used 1 / dt
    //    (240 * the gap per update at 240 FPS) and 2.1.0 to 2.2.0 kept 30 per update (still 8 times the original
    //    pull per second at 240 FPS): bodies loading in were pulled to their pose instead of dropping, and
    //    characters standing on or against them took that speed as a moving floor or wall and were flung,
    //    upward into ceilings (user report: Firelink's bodies, the Undead Burg's bandit). The per-update push is
    //    scaled to the frame, 900 * dt (30 at 30 FPS); below 30 FPS it stays 1 / dt, so a long frame reaches the
    //    pose without passing it.
    {0x00DEB9BF, 4, 0x011E7CD8, &g_inv_dt, 6, false},     // movsd xmm0,[30.0]  -> 1 / dt
    {0x00DF1809, 4, 0x011E7CD8, &g_inv_dt, 6, false},     // movsd xmm0,[30.0]  -> 1 / dt
    {0x00EC6562, 2, 0x011E7CD8, &g_ragdoll_gain, 6, false},  // fmul [30.0]     -> min(900 dt, 1 / dt)
    // The engine's 1/30 step (0x11E7E90) is rewritten with the frame time every frame, so code that
    // copies it into a setting would store whatever the frame time was at that moment. These keep 1/30:
    // hkpWorldCinfo's expected minimum step, an ImageFilter setting, and the repeat interval of eight
    // input actions that PadMan registers again whenever the key configuration changes.
    {0x0087B526, 2, 0x011E7E90, &g_fixed_step, 7, false},  // fld [1/30]   hkpWorldCinfo
    {0x00D8621C, 4, 0x011E7E90, &g_fixed_step, 7, false},  // movss [1/30] ImageFilter
    {0x00F7C78E, 4, 0x011E7E90, &g_fixed_step, 7, false},  // movss [1/30] input repeat interval
    {0x00F7C7C6, 4, 0x011E7E90, &g_fixed_step, 7, false},
    {0x00F7C7FE, 4, 0x011E7E90, &g_fixed_step, 7, false},
    {0x00F7C836, 4, 0x011E7E90, &g_fixed_step, 7, false},
    {0x00F7C86E, 4, 0x011E7E90, &g_fixed_step, 7, false},
    {0x00F7C8A6, 4, 0x011E7E90, &g_fixed_step, 7, false},
    {0x00F7C8DE, 4, 0x011E7E90, &g_fixed_step, 7, false},
    {0x00F7C916, 4, 0x011E7E90, &g_fixed_step, 7, false},
};

constexpr int kGroups = 8;
bool g_enabled[kGroups] = {true, true, true, true, true, true, true, true};
bool g_keys_down[5] = {};
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

// The landing (0xDAA1D0) turns a ground-crawling bullet's velocity along the surface and multiplies its speed by
// (0.5 + 0.5 * cos(turn)) * 0.97, the 0.97 read from the bullet manager ([0x137D6DC] + 0x58). A sliding glob
// lands about three times a frame at any frame rate, so at 120 FPS it lost that speed four times as fast and
// stopped short of where it reaches at 30 (down to its BulletParam minimum speed, 1.7 for the undead dragon's
// acid): it no longer reached a player it hits at 30. The stub loads 0.97 ^ (dt * 30) instead; with the damping
// group off it loads the manager's own value.
constexpr uint32_t kLandKeep = 0x00DABF2B;  // movss xmm0, dword ptr [eax+0x58]
constexpr uint8_t kLandKeepBytes[5] = {0xF3, 0x0F, 0x10, 0x40, 0x58};
constexpr uint32_t kBulletMan = 0x0137D6DC;
bool g_land_keep_hooked = false;

__declspec(naked) void land_keep_stub() {
    __asm {
        movss xmm0, dword ptr [g_land_keep]
        push 0DABF30h
        ret
    }
}

bool install_land_keep() {
    if (std::memcmp(reinterpret_cast<const void*>(kLandKeep), kLandKeepBytes, sizeof(kLandKeepBytes)) != 0) {
        LOG_ERROR("Bullet landing speed at %08X does not match this build. Not patched.", kLandKeep);
        return false;
    }
    DWORD old = 0;
    auto* p = reinterpret_cast<uint8_t*>(kLandKeep);
    if (!VirtualProtect(p, sizeof(kLandKeepBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&land_keep_stub) - (kLandKeep + 5));
    p[0] = 0xE9;
    std::memcpy(p + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(kLandKeepBytes));
    DWORD ignored = 0;
    VirtualProtect(p, sizeof(kLandKeepBytes), old, &ignored);
    return true;
}

// The bullet manager's landing keep (0.97), or 0.97 before the manager exists.
float bullet_land_keep() {
    float k = 0.97f;
    __try {
        const uint8_t* man = *reinterpret_cast<const uint8_t* const*>(kBulletMan);
        if (man) k = *reinterpret_cast<const float*>(man + 0x58);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        k = 0.97f;
    }
    return (k > 0.0f && k <= 1.0f) ? k : 0.97f;
}

// A bullet emitter that fires repeatedly while its animation event lasts (BulletParam shootInterval > 0, the
// undead dragon's acid among them) runs fn 0xDAF900 every frame: while its countdown [+0x2F0] is above 0 it
// subtracts the frame time, otherwise it fires a volley and sets the countdown to shootInterval. The decrement
// and the shot are a frame apart, so volleys come every (frames to count down + 1) frames: 4 frames (0.133 s) for
// 0.1 at 30 FPS, but 0.117-0.133 s at 60 and 0.108-0.117 s at 120, which fired 54 acid globs a burst instead of
// 45. The countdown stored after a volley is now the period the game has at 30 FPS less one and a half frames,
// so the next volley comes on the frame nearest that period at any frame rate (4 frames at 30 FPS, as before).
constexpr uint32_t kEmitReset = 0x00DAF966;  // xorps xmm1,xmm1 ; comiss xmm1,xmm0 ; movss [edi+0x2F0],xmm0
constexpr uint8_t kEmitResetBytes[14] = {0x0F, 0x57, 0xC9, 0x0F, 0x2F, 0xC8, 0xF3, 0x0F,
                                         0x11, 0x87, 0xF0, 0x02, 0x00, 0x00};
bool g_emit_hooked = false;

float __stdcall emit_countdown(float interval, float dt) {
    if (!g_enabled[2] || !(interval > 0.0f) || !(dt > 0.0f) || interval > 600.0f) return interval;
    // The game's own count at 30 FPS: subtract 1/30 (in double, stored as float) until the countdown is not
    // above 0; the volley is the frame after.
    const float step = 1.0f / 30.0f;
    float t = interval;
    int frames = 0;
    while (t > 0.0f && frames < 20000) {
        t = static_cast<float>(static_cast<double>(t) - static_cast<double>(step));
        ++frames;
    }
    const double period = (frames + 1) / 30.0;
    const double count = period - 1.5 * static_cast<double>(dt);
    return static_cast<float>(count > 0.0 ? count : 0.0);
}

__declspec(naked) void emit_reset_stub() {
    __asm {
        // xmm0 = shootInterval; the caller's frame time is at [esp+0Ch].
        sub esp, 4
        movss dword ptr [esp], xmm0
        push dword ptr [esp + 10h]
        push dword ptr [esp + 4]
        call emit_countdown
        fstp dword ptr [edi + 2F0h]
        movss xmm0, dword ptr [esp]
        add esp, 4
        // The jb at 0xDAF974 tests 0 < shootInterval, as the replaced comiss did.
        xorps xmm1, xmm1
        comiss xmm1, xmm0
        push 0DAF974h
        ret
    }
}

bool install_emit_reset() {
    if (std::memcmp(reinterpret_cast<const void*>(kEmitReset), kEmitResetBytes, sizeof(kEmitResetBytes)) != 0) {
        LOG_ERROR("Bullet emitter countdown at %08X does not match this build. Not patched.", kEmitReset);
        return false;
    }
    DWORD old = 0;
    auto* p = reinterpret_cast<uint8_t*>(kEmitReset);
    if (!VirtualProtect(p, sizeof(kEmitResetBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&emit_reset_stub) - (kEmitReset + 5));
    p[0] = 0xE9;
    std::memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < sizeof(kEmitResetBytes); ++i) p[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(kEmitResetBytes));
    DWORD ignored = 0;
    VirtualProtect(p, sizeof(kEmitResetBytes), old, &ignored);
    return true;
}

}  // namespace

bool fixes_install(const FixFlags& flags) {
    g_enabled[0] = flags.slide;
    g_enabled[1] = flags.damping;
    g_enabled[2] = flags.timers;
    g_enabled[3] = flags.smoothing;
    g_enabled[4] = flags.graze;
    g_enabled[5] = flags.ui;
    g_enabled[6] = flags.velocity;
    g_enabled[7] = true;  // the copied 1/30 step is always restored while the frame time is written
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
    for (int g = 0; g < kGroups; ++g) set_group(g, g_enabled[g]);
    g_land_keep_hooked = install_land_keep();
    if (g_land_keep_hooked) ++ok;
    g_emit_hooked = install_emit_reset();
    if (g_emit_hooked) ++ok;
    g_installed = true;
    LOG_INFO("Per-frame fixes: %d sites verified, %d skipped. slide=%d damping=%d timers=%d smoothing=%d graze=%d ui=%d "
             "velocity=%d",
             ok, bad, g_enabled[0], g_enabled[1], g_enabled[2], g_enabled[3], g_enabled[4], g_enabled[5], g_enabled[6]);
    return bad == 0;
}

double g_last_dt = 1.0 / 30.0;

double fixes_last_dt() {
    return g_last_dt;
}

void fixes_update(double dt) {
    g_last_dt = dt;
    if (!g_installed) return;
    const double s = dt * 30.0;
    g_gravity = static_cast<float>(1.0 * s);
    g_friction = static_cast<float>(std::pow(0.65, s));
    g_damp = std::pow(0.95, s);
    g_ground_decay = std::pow(0.4, s);
    g_ground_land = static_cast<float>(std::pow(0.4, 1.0 - s));
    if (g_land_keep_hooked) {
        const float k = bullet_land_keep();
        g_land_keep = g_enabled[1] ? static_cast<float>(std::pow(static_cast<double>(k), s)) : k;
    }
    g_timer_dt = dt;
    g_smooth = 1.0 - std::pow(1.0 - 1.0 / 30.0, s);
    g_rate = 1.0 / dt;
    g_slow = std::pow(0.8, s);
    g_recover = std::pow(1.2, s);
    g_gauge_k = 1.0 - std::pow(0.5, s);
    g_gauge_min = static_cast<float>(1.0 * s);
    g_inv_dt = 1.0 / dt;
    g_ragdoll_gain = 900.0 * dt < 1.0 / dt ? 900.0 * dt : 1.0 / dt;
    g_shine_div = 2.0 / dt;
}

void fixes_poll_hotkeys() {
    if (!g_installed) return;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    static bool snap_key = false;
    const bool snap_down = ctrl && (GetAsyncKeyState('6') & 0x8000) != 0;
    if (snap_down && !snap_key) {
        snap_set_enabled(!snap_enabled());
        LOG_INFO("Fix group 'step down (ledge/ladder snap)' is now %s", snap_enabled() ? "ON" : "OFF");
    }
    snap_key = snap_down;
    static bool camera_key = false;
    const bool camera_down = ctrl && (GetAsyncKeyState('7') & 0x8000) != 0;
    if (camera_down && !camera_key) {
        camera_set_enabled(!camera_enabled());
        LOG_INFO("Fix group 'camera' is now %s", camera_enabled() ? "ON" : "OFF");
    }
    camera_key = camera_down;
    static bool turn_key = false;
    const bool turn_down = ctrl && (GetAsyncKeyState('9') & 0x8000) != 0;
    if (turn_down && !turn_key) {
        turn_set_enabled(!turn_enabled());
        LOG_INFO("Fix group 'lock-on turn' is now %s", turn_enabled() ? "ON" : "OFF");
    }
    turn_key = turn_down;
    static bool foot_key = false;
    const bool foot_down = ctrl && (GetAsyncKeyState('0') & 0x8000) != 0;
    if (foot_down && !foot_key) {
        footik_set_enabled(!footik_enabled());
        LOG_INFO("Fix group 'foot IK' is now %s", footik_enabled() ? "ON" : "OFF");
    }
    foot_key = foot_down;
    for (int i = 0; i < 5; ++i) {
        const bool down = ctrl && (GetAsyncKeyState('1' + i) & 0x8000) != 0;
        if (down && !g_keys_down[i]) {
            g_enabled[i] = !g_enabled[i];
            set_group(i, g_enabled[i]);
            static const char* const names[5] = {"slide", "damping", "timers", "smoothing", "graze"};
            LOG_INFO("Fix group '%s' is now %s", names[i], g_enabled[i] ? "ON" : "OFF");
        }
        g_keys_down[i] = down;
    }
}
