#include "stepup.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

// FixStepUp: lips a little higher than the step height are climbed as at 30 FPS.
//
// Each frame the movement controller (fn 0xEC08B0, stdcall(body, step info, out), once per character body) lifts the
// physics proxy by the body's step height [body+0xD4] (0.3; [body+0xD8] with [body+0x54] set), moves it with the
// Havok character proxy for the frame, then casts down and puts it back onto the ground (or down by the lift). A lip
// up to the step height is simply stepped over. A higher one is climbed in the frame where the lifted proxy, after
// creeping up its edge, gets to move forward: how far it gets in that one frame decides whether the next snap lands
// it on top of the lip or back below it. At 30 FPS a run covers 0.12 m in that frame and climbs lips about 0.11 m
// above the lift; at 120 FPS it covers 0.03 m and climbs nothing above it (measured in the Great Hollow's Cloranthy
// Ring hollow, user report 2026-11-22: a 0.37 m lip climbed at 30 FPS after 0.4 s, never at 120; the smallest step
// height that climbs it is 0.25-0.27 at 30 FPS and 0.40-0.42 at 120). The difference is the extra forward distance of
// a 1/30 s frame, times about tan(60 degrees), the proxy's steepest walkable slope ([proxy+0xB0] = cos 60 = 0.5).
// So for the call, the step height is raised by tan(steepest slope) * the frame's horizontal speed * (1/30 s - frame
// time), on a body that was on the ground last frame ([body+0x194], its time in the air, is 0), and put back after: the lift, the
// stair probe (0xEBF3C0) and the snap's put-back all see the same height. At 30 FPS and below nothing changes. The
// speed is the one the controller set for this frame ([proxy+0x40], written by 0xEC0BB0 just before the call, the
// solver's own result only after it), so a body pressed against a lip, which hardly moves, still counts its run.
namespace {

constexpr uint32_t kCallSite = 0x00EC296D;     // call 0xEC08B0 in 0xEC2860
constexpr uint32_t kLiftMoveSnap = 0x00EC08B0;
constexpr float kMaxBoost = 0.35f;

using LiftMoveSnapFn = void(__stdcall*)(uint8_t* body, void* info, void* out);

volatile long g_on = 1;
volatile long g_boosted = 0;
volatile long g_installed = 0;
float g_last_boost = 0.0f;

float boost_for(const uint8_t* body, const uint8_t* info) {
    float dt = 0.0f;  // the frame's step ([info+8], the proxy's own time step)
    std::memcpy(&dt, info + 8, sizeof(dt));
    if (!(dt > 0.0f)) return 0.0f;
    const double short_by = 1.0 / 30.0 - dt;
    if (!(short_by > 0.0)) return 0.0f;
    // On a ladder, or not on the ground at the end of the last frame: [body+0x194] is the time in the air, which the
    // caller (0xEC2860) adds to after a frame without ground ([body+0x32], cleared just before this call) and zeroes
    // after one with it.
    float air = 0.0f;
    std::memcpy(&air, body + 0x194, sizeof(air));
    if (body[0x1D4] != 0 || !(air <= 0.0f)) return 0.0f;
    const uint8_t* proxy = *reinterpret_cast<const uint8_t* const*>(body + 0x38);
    if (!proxy) return 0.0f;
    float v[3];
    std::memcpy(v, proxy + 0x40, sizeof(v));
    const float speed = std::sqrt(v[0] * v[0] + v[2] * v[2]);
    if (!(speed > 0.05f) || speed > 50.0f) return 0.0f;
    float c = 0.5f;
    std::memcpy(&c, proxy + 0xB0, sizeof(c));
    if (!(c > 0.05f && c < 0.999f)) c = 0.5f;
    const float tan_slope = std::sqrt(1.0f - c * c) / c;
    float boost = static_cast<float>(tan_slope * speed * short_by);
    if (boost > kMaxBoost) boost = kMaxBoost;
    return boost;
}

void __stdcall hk_lift_move_snap(uint8_t* body, void* info, void* out) {
    float* step = nullptr;
    float saved = 0.0f;
    if (g_on && body && info) {
        const float boost = boost_for(body, static_cast<const uint8_t*>(info));
        if (boost > 0.001f) {
            step = reinterpret_cast<float*>(body + (body[0x54] ? 0xD8 : 0xD4));
            saved = *step;
            if (saved > 0.0f && saved < 2.0f) {
                *step = saved + boost;
                g_last_boost = boost;
                InterlockedIncrement(&g_boosted);
            } else {
                step = nullptr;
            }
        }
    }
    reinterpret_cast<LiftMoveSnapFn>(kLiftMoveSnap)(body, info, out);
    if (step) *step = saved;
}

}  // namespace

bool stepup_install() {
    auto* call = reinterpret_cast<uint8_t*>(kCallSite);
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    if (call[0] != 0xE8 || static_cast<uint32_t>(kCallSite + 5 + rel) != kLiftMoveSnap) {
        LOG_ERROR("FixStepUp: the call at %08X is not a call to %08X in this build. Not patching.", kCallSite,
                  kLiftMoveSnap);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("FixStepUp: VirtualProtect failed (Win32=%lu)", GetLastError());
        return false;
    }
    const int32_t to = static_cast<int32_t>(reinterpret_cast<uint32_t>(&hk_lift_move_snap) - (kCallSite + 5));
    std::memcpy(call + 1, &to, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    g_installed = 1;
    LOG_INFO("FixStepUp: lips a little above the step height are climbed as at 30 FPS (the lift raised by the "
             "distance a 1/30 s frame would cover beyond this one, times the steepest walkable slope)");
    return true;
}

void stepup_set_enabled(bool on) { InterlockedExchange(&g_on, on ? 1 : 0); }
bool stepup_enabled() { return g_on != 0; }
long stepup_boosted_frames() { return g_boosted; }
float stepup_last_boost() { return g_last_boost; }
