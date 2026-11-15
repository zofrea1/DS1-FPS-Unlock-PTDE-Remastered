#pragma once

#include <cstdint>
#include <cstring>

// Event script waits on the native frame grid (INI FixEventWaits; Remastered also EventWaitPtdeSpeed).
//
// Every wait of the event scripts (EMEVD 1001[00] "wait N seconds", 1001[01] "wait N frames", which the game turns
// into N * (float)(1/30) seconds, and the random waits) is an EcTestPassTime condition: it keeps the remaining time
// (PTDE +0x18, Remastered +0x28) and its test counts it down by the frame time once a frame, done when it is at or
// below zero (PTDE fn 0xBB22D0, Remastered 0x1407E8D00). The original game stepped exactly (float)(1/30) s a
// frame, so a wait lasts whole frames: rounding leaves 0.5 s needing 16 of them (0.533 s), 2 s 61 (2.033 s); 121 of
// the 193 timed waits of the scripts end a frame after their nominal time. At a higher frame rate the same waits end
// up to a frame of the original (33 ms) earlier, and a long one drifts (float steps of a few ms off a large
// remaining time: a 600 s wait lasts 600.67 s at 150 FPS). Most of that is invisible; where a script times an attack
// against something moving it is not: the Undead Burg's barrel thrower (m10_01 event 11010120: his push, wait 0.5 s,
// the barrel rolls, wait 0.5 s, the barrel's damaging object) is hit by his own barrel at 150 FPS, whose first hit
// test comes 0.054 s sooner after the push than at 30 FPS, when the barrel is 7 cm nearer to him (measured in game,
// 2026-11-14).
//
// The fix keeps the time a wait has run (in double precision, per wait) and ends it on the frame nearest to the
// moment the original would: n native frames after it began, n = the frames the game's own countdown takes at the
// native rate (simulated with the game's float arithmetic). The game's own remaining time is written back each frame
// (the time left until that frame, less half a frame), so it stays at or below zero exactly when the wait is done.
// At the native rate a wait ends on the original's frame.

namespace eventwait {

// Frames the game's countdown takes for a wait of `wait` seconds at the native rate (float steps of (float)(1/rate)
// from the float wait, each difference rounded to float as the game's test does). Simulated in full, also for long
// waits (the original's own drift included: 600 s takes 18002 frames at 30 FPS).
inline uint32_t native_frames(float wait, double rate) {
    if (!(wait > 0.0f) || !(rate > 0.0)) return 1;
    const float step = static_cast<float>(1.0 / rate);
    float t = wait;
    uint32_t n = 0;
    do {
        t = static_cast<float>(static_cast<double>(t) - static_cast<double>(step));
        ++n;
    } while (t > 0.0f && n < 100000000u);
    return n;
}

// The waits being counted, by the condition's address. A wait is the same one while the game's remaining time is still
// the value written for it last; a new condition at a freed address starts with its own time and is a new wait.
// Entries not used for 5 s (a wait whose event went away with its map) are taken for new ones.
struct Entry {
    const void* key;
    double elapsed;    // game time since the wait began
    double length;     // n / rate
    float written;     // the remaining time written last
    uint64_t used_ms;  // when it was last counted
};

constexpr int kSlots = 512;

struct Table {
    Entry e[kSlots];
};

inline Table& table() {
    static Table t{};
    return t;
}

inline Entry* find_or_add(const void* key, float remaining, uint64_t now_ms, bool* fresh) {
    Table& t = table();
    const uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key) >> 3) * 2654435761u;
    Entry* free_slot = nullptr;
    for (int k = 0; k < kSlots; ++k) {
        Entry& s = t.e[(h + k) % kSlots];
        if (s.key == key) {
            uint32_t a, b;
            std::memcpy(&a, &s.written, 4);
            std::memcpy(&b, &remaining, 4);
            if (a == b) {
                *fresh = false;
                return &s;
            }
            free_slot = &s;  // the same address, a new wait
            break;
        }
        if (!free_slot && (s.key == nullptr || now_ms - s.used_ms > 5000)) free_slot = &s;
        if (s.key == nullptr) break;
    }
    if (!free_slot) return nullptr;
    *fresh = true;
    free_slot->key = key;
    return free_slot;
}

// One frame of a wait's test: `remaining` is the game's countdown field (read, then written), `dt` the frame time.
// Returns whether the wait is done. Without a free entry it does what the game does.
inline bool step(const void* key, float* remaining, float dt, double rate, uint64_t now_ms) {
    bool fresh = false;
    Entry* s = find_or_add(key, *remaining, now_ms, &fresh);
    if (!s) {
        *remaining = static_cast<float>(static_cast<double>(*remaining) - static_cast<double>(dt));
        return *remaining <= 0.0f;
    }
    if (fresh) {
        s->elapsed = 0.0;
        s->length = native_frames(*remaining, rate) / rate;
    }
    s->used_ms = now_ms;
    if (dt > 0.0f) s->elapsed += dt;
    // Done on the frame nearest to the native end: the time left less half of this frame at or below zero.
    const double half = dt > 0.0f ? 0.5 * dt : 0.0;
    const float left = static_cast<float>(s->length - s->elapsed - half);
    *remaining = left;
    s->written = left;
    return !(left > 0.0f);
}

}  // namespace eventwait
