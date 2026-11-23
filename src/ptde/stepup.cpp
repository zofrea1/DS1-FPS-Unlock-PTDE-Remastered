#include "stepup.h"

#include "fixes.h"
#include "log.h"
#include "snap.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

// FixStepUp: lips a little higher than the step height are climbed as at 30 FPS.
//
// Each frame the movement controller (fn 0xEC08B0, stdcall(body, step info, out), once per character body) lifts the
// physics proxy by the body's step height [body+0xD4] (0.3; [body+0xD8] with [body+0x54] set), moves it with the
// Havok character proxy for the frame, then casts a small sphere down from just above it and puts it back onto the
// ground (or down by the lift). A lip up to the step height is simply stepped over. A higher one with a rounded or
// sloped edge is climbed by creeping: each frame the lifted proxy rides a little way up the edge, and the snap leaves
// it there, until it gets far enough over to pop up onto the lip. How far one frame's move gets decides it, and at
// 120 FPS a frame's move is a quarter of a 30 FPS one: in the Great Hollow's Chloranthy Ring hollow (user report
// 2026-11-22) a 0.37 m lip was climbed at 30 FPS after 0.4 s and never at 120 (the creep per second was 13 times
// smaller; the smallest step height that climbs it is 0.25-0.27 at 30 FPS and 0.40-0.42 at 120).
//
// Two ways to give it back, chosen at run time (RemoteControl `stepup 0|1|2`, Ctrl+8 toggles off / the default):
//
// Window (2, the default): a body that is pushing but hardly moving (over the last 1/30 s it covered under 30% of the
// distance its speed asked for) is moved as at 30 FPS. Its frames still run one by one, but every 1/30 s the window's
// frames are moved again as one: the body goes back to where the window started and the controller runs once with the
// whole window's time step. That is exactly the 30 FPS move, so the body creeps and climbs as at 30 FPS, and what
// 30 FPS cannot climb (a sharp-edged block above the step height) stays unclimbable. When a window's move covers half
// of its distance or more, the body is free again and every frame is its own move. The step-down reach (snap.cpp)
// counts the redone move as one 30 FPS frame.
//
// Boost (1, the first version): for the call the step height is raised by tan(steepest slope) * the frame's
// horizontal speed * (1/30 s - frame time), on a body that was on the ground last frame, and put back after. It climbs
// that lip at 60-240 FPS, but faster than 30 FPS at 120 and above and slower at 60, and a sprint can take the body over
// sharp-edged blocks a little above the step height that 30 FPS stops at.
namespace {

constexpr uint32_t kCallSite = 0x00EC296D;     // call 0xEC08B0 in 0xEC2860
constexpr uint32_t kLiftMoveSnap = 0x00EC08B0;
constexpr uint32_t kSetPosition = 0x008F2380;  // hkpCharacterProxy setPosition (thiscall, &vec4)
constexpr float kMaxBoost = 0.35f;
constexpr float kFrame30 = 1.0f / 30.0f;

constexpr int kModeOff = 0;
constexpr int kModeBoost = 1;
constexpr int kModeWindow = 2;
constexpr int kModeDefault = kModeWindow;

using LiftMoveSnapFn = void(__stdcall*)(uint8_t* body, void* info, void* out);

volatile long g_mode = kModeDefault;
volatile long g_boosted = 0;
volatile long g_redone = 0;   // windows moved again as one 30 FPS frame
volatile long g_entered = 0;  // times a body was found pushing without moving
volatile long g_installed = 0;
float g_last_boost = 0.0f;

// Per body: the frames of the current window and a short history for the "pushing but not moving" test.
constexpr unsigned kHist = 64;
struct Win {
    const uint8_t* body = nullptr;
    ULONGLONG last_ms = 0;
    bool blocked = false;
    float anchor[4] = {};  // body position at the start of the window
    float acc = 0.0f;      // time since the window started
    unsigned frames = 0;   // frames done in the window (not counting the one that closes it)
    float end[3] = {};     // where the last frame left the body
    float want[kHist] = {}, got[kHist] = {}, step[kHist] = {};
    unsigned head = 0;
};
constexpr size_t kSlots = 128;
Win g_win[kSlots];

Win* win_slot(const uint8_t* body, ULONGLONG now) {
    const size_t start = ((reinterpret_cast<uintptr_t>(body) >> 4) * 2654435761u) & (kSlots - 1);
    Win* oldest = nullptr;
    for (size_t n = 0; n < 16; ++n) {
        Win& w = g_win[(start + n) & (kSlots - 1)];
        if (w.body == body) return &w;
        if (!oldest || w.last_ms < oldest->last_ms) oldest = &w;
    }
    *oldest = Win();
    oldest->body = body;
    return oldest;
}

const float* proxy_position(const uint8_t* proxy) {
    // 0x8F2370: [[proxy+0x60]+0x18]+0x30
    const uint8_t* a = *reinterpret_cast<const uint8_t* const*>(proxy + 0x60);
    if (!a) return nullptr;
    const uint8_t* b = *reinterpret_cast<const uint8_t* const*>(a + 0x18);
    if (!b) return nullptr;
    return reinterpret_cast<const float*>(b + 0x30);
}

void proxy_set_position(uint8_t* proxy, const float* p4) {
    alignas(16) float v[4] = {p4[0], p4[1], p4[2], p4[3]};
    float* arg = v;
    const uint32_t fn = kSetPosition;
    const uint32_t self = reinterpret_cast<uint32_t>(proxy);
    __asm {
        push arg
        mov ecx, self
        call fn
    }
}

float dist_xz(const float* a, const float* b) {
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// The frame's step ([info+8], the proxy's own time step) when this body is one the fix applies to: on the ground at
// the end of the last frame ([body+0x194], its time in the air, is 0; [body+0x32] is cleared before this call), not
// on a ladder, with a proxy, at a frame shorter than 1/30 s. 0 otherwise.
float frame_step(const uint8_t* body, const uint8_t* info, const uint8_t** proxy_out, float* speed_out) {
    float dt = 0.0f;
    std::memcpy(&dt, info + 8, sizeof(dt));
    if (!(dt > 0.0f) || !(dt < kFrame30 * 0.999f)) return 0.0f;
    float air = 0.0f;
    std::memcpy(&air, body + 0x194, sizeof(air));
    if (body[0x1D4] != 0 || !(air <= 0.0f)) return 0.0f;
    const uint8_t* proxy = *reinterpret_cast<const uint8_t* const*>(body + 0x38);
    if (!proxy) return 0.0f;
    // The speed the controller set for this frame ([proxy+0x40], written by 0xEC0BB0 just before the call; the
    // solver's own result only after it), so a body pressed against a lip, which hardly moves, still counts its run.
    float v[3];
    std::memcpy(v, proxy + 0x40, sizeof(v));
    const float speed = std::sqrt(v[0] * v[0] + v[2] * v[2]);
    if (!(speed < 50.0f)) return 0.0f;
    *proxy_out = proxy;
    *speed_out = speed;
    return dt;
}

float boost_for(const uint8_t* proxy, float speed, float dt) {
    if (!(speed > 0.05f)) return 0.0f;
    float c = 0.5f;
    std::memcpy(&c, proxy + 0xB0, sizeof(c));
    if (!(c > 0.05f && c < 0.999f)) c = 0.5f;
    const float tan_slope = std::sqrt(1.0f - c * c) / c;
    float boost = tan_slope * speed * (kFrame30 - dt);
    if (boost > kMaxBoost) boost = kMaxBoost;
    return boost;
}

void call_boosted(uint8_t* body, void* info, void* out, const uint8_t* proxy, float speed, float dt) {
    float* step = nullptr;
    float saved = 0.0f;
    const float boost = boost_for(proxy, speed, dt);
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
    reinterpret_cast<LiftMoveSnapFn>(kLiftMoveSnap)(body, info, out);
    if (step) *step = saved;
}

void call_window(uint8_t* body, void* info, void* out, uint8_t* proxy, float speed, float dt) {
    const ULONGLONG now = GetTickCount64();
    Win* w = win_slot(body, now);
    float start[4];
    std::memcpy(start, body + 0x10, sizeof(start));
    // A new body, one not seen for a while, or one moved since its last frame (a teleport, a warp): start over.
    if (now - w->last_ms > 200 || dist_xz(start, w->end) > 0.5f || std::fabs(start[1] - w->end[1]) > 1.0f) {
        const uint8_t* keep = w->body;
        *w = Win();
        w->body = keep;
    }
    w->last_ms = now;
    auto* lms = reinterpret_cast<LiftMoveSnapFn>(kLiftMoveSnap);

    if (w->blocked && w->acc + dt >= kFrame30 - 0.25f * dt) {
        // Close the window: back to its start, then one move with the whole window's time.
        const float window = w->acc + dt;
        std::memcpy(body + 0x10, w->anchor, sizeof(float) * 3);
        proxy_set_position(proxy, w->anchor);
        auto* f = static_cast<float*>(info);
        const float saved_dt = f[2], saved_inv = f[3];
        f[2] = window;
        f[3] = 1.0f / window;
        snap_redo_begin(body, window, w->frames);
        lms(body, info, out);
        snap_redo_end();
        f[2] = saved_dt;
        f[3] = saved_inv;
        InterlockedIncrement(&g_redone);
        const float* p = proxy_position(proxy);
        if (p) std::memcpy(w->end, p, sizeof(w->end));
        const float covered = dist_xz(w->anchor, w->end);
        if (!(speed > 0.05f) || covered >= 0.5f * speed * window) w->blocked = false;
        std::memcpy(w->anchor, w->end, sizeof(w->end));
        w->anchor[3] = start[3];
        w->acc = 0.0f;
        w->frames = 0;
        w->head = 0;
        std::memset(w->step, 0, sizeof(w->step));
        return;
    }

    lms(body, info, out);
    const float* p = proxy_position(proxy);
    if (p) std::memcpy(w->end, p, sizeof(w->end));
    if (w->blocked) {
        w->acc += dt;
        ++w->frames;
        return;
    }
    // Free: is it pushing without moving? Over the last 1/30 s: covered under 30% of the distance asked for, which is
    // at least 2 cm (0.6 m/s).
    const unsigned at = w->head++ % kHist;
    w->want[at] = speed * dt;
    w->got[at] = dist_xz(start, w->end);
    w->step[at] = dt;
    float want = 0.0f, got = 0.0f, span = 0.0f;
    for (unsigned i = 0; i < kHist && span < kFrame30 * 0.95f; ++i) {
        const unsigned j = (w->head - 1 - i) % kHist;
        if (!(w->step[j] > 0.0f)) break;
        want += w->want[j];
        got += w->got[j];
        span += w->step[j];
    }
    if (span >= kFrame30 * 0.95f && want > 0.02f && got < 0.3f * want) {
        w->blocked = true;
        InterlockedIncrement(&g_entered);
        std::memcpy(w->anchor, w->end, sizeof(w->end));
        w->anchor[3] = start[3];
        w->acc = 0.0f;
        w->frames = 0;
    }
}

// A body the fix does not apply to this frame (in the air, on a ladder, at 30 FPS or below) ends its window.
void win_forget(const uint8_t* body) {
    const size_t start = ((reinterpret_cast<uintptr_t>(body) >> 4) * 2654435761u) & (kSlots - 1);
    for (size_t n = 0; n < 16; ++n) {
        Win& w = g_win[(start + n) & (kSlots - 1)];
        if (w.body != body) continue;
        w.blocked = false;
        w.head = 0;
        std::memset(w.step, 0, sizeof(w.step));
        return;
    }
}

void __stdcall hk_lift_move_snap(uint8_t* body, void* info, void* out) {
    const long mode = g_mode;
    const uint8_t* proxy = nullptr;
    float speed = 0.0f;
    const float dt = (mode != kModeOff && body && info)
                         ? frame_step(body, static_cast<const uint8_t*>(info), &proxy, &speed)
                         : 0.0f;
    if (dt > 0.0f && mode == kModeBoost) {
        call_boosted(body, info, out, proxy, speed, dt);
    } else if (dt > 0.0f && mode == kModeWindow) {
        call_window(body, info, out, const_cast<uint8_t*>(proxy), speed, dt);
    } else {
        if (mode == kModeWindow && body) win_forget(body);
        reinterpret_cast<LiftMoveSnapFn>(kLiftMoveSnap)(body, info, out);
    }
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
    LOG_INFO("FixStepUp: a character pushing against something without moving is moved as at 30 FPS (every 1/30 s "
             "its frames are moved again as one), so lips a little above the step height are climbed as at 30 FPS");
    return true;
}

void stepup_set_enabled(bool on) { InterlockedExchange(&g_mode, on ? kModeDefault : kModeOff); }
bool stepup_enabled() { return g_mode != kModeOff; }
void stepup_set_mode(int mode) {
    InterlockedExchange(&g_mode, (mode == kModeBoost || mode == kModeWindow) ? mode : kModeOff);
}
int stepup_mode() { return static_cast<int>(g_mode); }
long stepup_boosted_frames() { return g_boosted; }
float stepup_last_boost() { return g_last_boost; }
long stepup_redone_windows() { return g_redone; }
long stepup_blocked_entries() { return g_entered; }
