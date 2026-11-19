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
// The frame "that moves nothing" is the segment's last: after a whole number of frames float rounding leaves its time
// just short of 0.1 s, so one more frame plays the sliver left, and the script starts the next segment after it. A
// segment at speed k thus lasts floor(0.1 / (k dt)) + 1 frames (4 at 30 FPS, 7 at 60, 25 at 240: as measured).
//
// While the player plays one of the two segments, its animation set's speed ([ChrCtrl + 0x14] + 0x64, the one the
// co-op mod sets to 0 to hold a pose; 1 otherwise) is set so that a segment lasts n frames, n the whole number nearest
// to the 30 FPS segment's 4/30 s: k = 0.1 / ((n - 0.5) dt), half a frame clear of the rounding (a first try with
// 0.1 / (0.1 + 1/30 - dt) landed on whole frames at 60 FPS, and every other segment dropped its last frame). The slide
// then takes as long as at 30 FPS, played smoothly: each segment's last frame moves half a frame's worth instead of the
// 30 FPS pause every fourth frame. At 30 FPS and below k is 1 and nothing changes. The value is only written while it
// is the game's 1 (a pose held by the co-op mod or anything else is left alone) and put back to 1 afterwards.
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
    // A segment lasts floor(0.1 / (k dt)) + 1 frames: its last frame plays what is left of it (at a whole number of
    // frames that is a sliver, as float rounding leaves it just short: the frame "that moves nothing"). n frames, the
    // nearest to the 30 FPS segment (4/30 s), come from k with 0.1 / (k dt) = n - 0.5, clear of the rounding.
    float k = 1.0f;
    if (dt > 0.0 && dt < kRetailFrame * 0.98) {
        const double want = (kSegment + kRetailFrame) / dt;            // 4/30 s in frames
        double n = static_cast<double>(static_cast<long>(want + 0.5));  // nearest whole frame
        const double fewest = static_cast<double>(static_cast<long>(kSegment / dt)) + 1.0;
        if (n < fewest) n = fewest;
        k = static_cast<float>(kSegment / ((n - 0.5) * dt));
        if (k > 1.0f) k = 1.0f;
    }
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
