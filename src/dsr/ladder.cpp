#include "ladder.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

// FixLadderSlide, Remastered port of src/ptde/ladder.cpp (measured there; see that file for the whole story). A slide
// down a ladder is a chain of short animations: 7000 (the start), then 7001 and 7005 in turn, each 0.1 s that moves
// the body 0.5 m (5 m/s), and 7002 at the bottom. The character's script starts the next one only once the last has
// ended, and float rounding leaves each segment's time just short of 0.1 s after a whole number of frames, so one more
// frame plays the sliver: a segment at animation speed k lasts floor(0.1 / (k dt)) + 1 frames. Every 0.5 m costs one
// frame more than its 0.1 s, so the higher the frame rate, the faster the slide (60 FPS: 6 + 1 frames, 4.3 m/s; 240
// FPS: 24 + 1, 4.8 m/s; Prepare to Die Edition, measured: 30 FPS 2.53 s down a 9.4 m ladder, 60 FPS 2.23, 240 2.01).
//
// Remastered's own frame rate is 60, so this keeps its 60 FPS slide: while the player plays one of the two segments,
// its animation speed is set so that a segment lasts n frames, n the whole number nearest to the 60 FPS segment's
// 7/60 s, with k = 0.1 / ((n - 0.5) dt), half a frame clear of the rounding (Prepare to Die Edition with this rule: 2.50
// s at 60, 2.49 at 144, 2.52 at 240 against 2.53 at its 30). At 60 FPS and below k is 1 and nothing changes. The value
// is only written while it is the game's 1 (a speed set by anything else is left alone) and put back to 1 afterwards.
//
// Fields (the 2022 Steam executable; every read guarded, a pointer outside [16 MB, user space] not followed):
//   * World character manager RVA 0x1C77E50; the local player's ChrIns at +0x68 (as BonfireFix, src/dsr/bonfire.cpp).
//   * ChrIns+0x68 = the character's ChrCtrl (its +0x28 is the physics block the movement trace reads, trace.cpp).
//   * ChrCtrl+0x18 = the animation data, its speed a float at +0xA8 (DSR-Gadget's ChrAnimData.AnimSpeed; Prepare to Die
//     Edition's is ChrCtrl+0x14, +0x64, the one its ladder fix and the co-op pose hold write).
//   * [ChrCtrl+0x48]+0x80 = the animation played, which DSR-Gadget reads (Prepare to Die Edition's [ChrCtrl+0x2C]+0x60
//     shows exactly the slide segments, 7001 and 7005, and nothing once the slide ends; the two layouts differ by the
//     wider pointers before those fields, as the animation data and speed do).
// The Remastered offsets were not read in a running game for this: if the chain never shows a segment, nothing is
// written and the slide is as before.
namespace {

constexpr uint32_t kWorldChrManRva = 0x1C77E50;
constexpr uint32_t kPlayer = 0x68;     // world character manager -> the local player's ChrIns
constexpr uint32_t kChrCtrl = 0x68;    // ChrIns -> ChrCtrl
constexpr uint32_t kAnimData = 0x18;   // ChrCtrl -> animation data
constexpr uint32_t kSpeed = 0xA8;      // animation data: its speed (float, 1 = normal)
constexpr uint32_t kAnimObject = 0x48; // ChrCtrl -> current animation object
constexpr uint32_t kAnimId = 0x80;     // current animation object: the animation played
constexpr double kSegment = 0.1;       // each slide segment's own length
constexpr double kRetailFrame = 1.0 / 60.0;
constexpr uintptr_t kMinPointer = 0x1000000;
constexpr uintptr_t kMaxPointer = 0x00007FFFFFFFFFFFull;

bool g_on = false;
uintptr_t g_image = 0;
uintptr_t g_data = 0;      // the animation data whose speed we changed
float g_written = 0.0f;    // what we wrote there
bool g_seen = false;       // a segment was seen (logged once)

bool read_ptr(uintptr_t a, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read32(uintptr_t a, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_float(uintptr_t a, float v) {
    __try {
        *reinterpret_cast<float*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float read_float(uintptr_t a) {
    uint32_t bits = 0;
    float f = -1.0f;
    if (read32(a, &bits)) std::memcpy(&f, &bits, 4);
    return f;
}

bool follow(uintptr_t base, uint32_t offset, uintptr_t* out) {
    uintptr_t next = 0;
    if (!read_ptr(base + offset, &next) || next < kMinPointer || next > kMaxPointer) return false;
    *out = next;
    return true;
}

// Give the speed back if it is still ours.
void release() {
    if (g_data && read_float(g_data + kSpeed) == g_written) write_float(g_data + kSpeed, 1.0f);
    g_data = 0;
}

}  // namespace

void ladder_configure(bool on) {
    g_on = on;
    g_image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (on) {
        LOG_INFO("FixLadderSlide: a slide down a ladder takes as long as at 60 FPS, played smoothly");
    }
}

void ladder_update(double dt) {
    if (!g_on || !g_image) return;
    uintptr_t world = 0, chr = 0, ctrl = 0, object = 0, data = 0;
    uint32_t anim = 0;
    const bool ok = follow(g_image, kWorldChrManRva, &world) && follow(world, kPlayer, &chr) &&
                    follow(chr, kChrCtrl, &ctrl) && follow(ctrl, kAnimObject, &object) &&
                    read32(object + kAnimId, &anim) && follow(ctrl, kAnimData, &data);
    const bool segment = ok && (anim == 7001 || anim == 7005);
    if (!segment) {
        if (g_data) release();
        return;
    }
    if (g_data && g_data != data) release();
    // A segment lasts floor(0.1 / (k dt)) + 1 frames; n frames, the nearest to the 60 FPS segment (7/60 s), come from
    // k with 0.1 / (k dt) = n - 0.5, clear of the rounding.
    float k = 1.0f;
    if (dt > 0.0 && dt < kRetailFrame * 0.98) {
        const double want = (kSegment + kRetailFrame) / dt;            // 7/60 s in frames
        double n = static_cast<double>(static_cast<long>(want + 0.5));  // nearest whole frame
        const double fewest = static_cast<double>(static_cast<long>(kSegment / dt)) + 1.0;
        if (n < fewest) n = fewest;
        k = static_cast<float>(kSegment / ((n - 0.5) * dt));
        if (k > 1.0f) k = 1.0f;
    }
    const float now = read_float(data + kSpeed);
    if (!g_seen) {
        g_seen = true;
        LOG_INFO("FixLadderSlide: first slide segment (animation %u, speed %.3f, step %.2f ms, segment speed %.3f)",
                 anim, now, dt * 1000.0, k);
    }
    // Ours from the last frame, or the game's own 1: anything else is someone else's.
    if (now != 1.0f && !(g_data == data && now == g_written)) return;
    if (now != k && write_float(data + kSpeed, k)) {
        g_data = data;
        g_written = k;
    } else if (now == k) {
        g_data = data;
        g_written = k;
    }
}
