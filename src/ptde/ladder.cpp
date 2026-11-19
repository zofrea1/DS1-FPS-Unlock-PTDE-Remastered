#include "ladder.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

// FixLadderSlide. A slide down a ladder is a chain of short animations: 7000 (the start), then 7001 and 7005 in turn,
// each 0.1 s that moves the body 0.5 m (5 m/s), and 7002 at the bottom. The character's script starts the next one
// only once the last has ended, and the animation it starts moves nothing in the frame it starts in, so every 0.5 m
// costs one frame more than its 0.1 s: at 30 FPS 3 frames that move and 1 that does not (0.133 s, 3.75 m/s, the
// slide of the original game), at 60 FPS 6 + 1 (4.3 m/s), at 240 FPS 24 + 1 (4.8 m/s). Measured in the lower Undead
// Burg (ladder o1215, 9.4 m, 2026-11-19): 2.43 s to the bottom at 30 FPS, 2.23 at 60, 1.93 at 120, 2.01 at 240; the
// higher the frame rate, the faster and smoother the slide.
//
// While the player plays one of the two segments, its animation set's speed ([ChrCtrl + 0x14] + 0x64, the one the
// co-op mod sets to 0 to hold a pose; 1 otherwise) is set to 0.1 / (0.1 + 1/30 - dt), so a segment and the frame that
// starts the next take 0.133 s as at 30 FPS: the same speed and time to the bottom, played smoothly (not the 30 FPS
// pause every fourth frame). At 30 FPS and below the factor is 1 and nothing changes. The value is only written while
// it is the game's 1 (a pose held by the co-op mod or anything else is left alone) and put back to 1 afterwards.
// Measured with this rule at 240 FPS: 2.48 s, as at 30 FPS for the same height (2.55 s; the 30 FPS run started 0.45 m
// lower).
namespace {

constexpr uint32_t kWorldChrMan = 0x0137DC70;  // [[[+0] + 4]] = the local player's ChrIns (as the remote control)
constexpr uint32_t kChrCtrl = 0x28;            // ChrIns -> ChrCtrl
constexpr uint32_t kCtrlAnim = 0x370;          // ChrCtrl: the animation it plays
constexpr uint32_t kCtrlAnimSet = 0x14;        // ChrCtrl -> animation set
constexpr uint32_t kSetSpeed = 0x64;           // animation set: its speed (float, 1 = normal)
constexpr double kSegment = 0.1;               // each slide segment's own length
constexpr double kRetailFrame = 1.0 / 30.0;

bool g_on = false;
uint32_t g_set = 0;        // the animation set whose speed we changed
float g_written = 0.0f;    // what we wrote there

bool read32(uint32_t a, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_float(uint32_t a, float v) {
    __try {
        *reinterpret_cast<float*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float read_float(uint32_t a) {
    uint32_t bits = 0;
    float f = -1.0f;
    if (read32(a, &bits)) std::memcpy(&f, &bits, 4);
    return f;
}

// Give the speed back if it is still ours.
void release() {
    if (g_set && read_float(g_set + kSetSpeed) == g_written) write_float(g_set + kSetSpeed, 1.0f);
    g_set = 0;
}

}  // namespace

void ladder_configure(bool on) {
    g_on = on;
    if (on) {
        LOG_INFO("FixLadderSlide: a slide down a ladder takes as long as at 30 FPS (3.75 m/s), played smoothly");
    }
}

void ladder_update(double dt) {
    if (!g_on) return;
    uint32_t wcm = 0, first = 0, chr = 0, ctrl = 0, anim = 0, set = 0;
    const bool ok = read32(kWorldChrMan, &wcm) && wcm && read32(wcm + 4, &first) && first && read32(first, &chr) &&
                    chr && read32(chr + kChrCtrl, &ctrl) && ctrl >= 0x10000 && read32(ctrl + kCtrlAnim, &anim) &&
                    read32(ctrl + kCtrlAnimSet, &set) && set >= 0x10000;
    const bool segment = ok && (anim == 7001 || anim == 7005);
    if (!segment) {
        if (g_set) release();
        return;
    }
    if (g_set && g_set != set) release();
    float k = 1.0f;
    if (dt > 0.0 && dt < kRetailFrame) k = static_cast<float>(kSegment / (kSegment + kRetailFrame - dt));
    const float now = read_float(set + kSetSpeed);
    // Ours from the last frame, or the game's own 1: anything else is someone else's (a held pose).
    if (now != 1.0f && !(g_set == set && now == g_written)) return;
    if (now != k && write_float(set + kSetSpeed, k)) {
        g_set = set;
        g_written = k;
    } else if (now == k) {
        g_set = set;
        g_written = k;
    }
}
