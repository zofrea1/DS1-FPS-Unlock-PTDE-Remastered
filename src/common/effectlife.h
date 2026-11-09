#pragma once

#include <cmath>

// SpEffect lifetimes on the native frame grid (INI FixSpEffectTiming).
//
// A special effect node keeps its remaining time at +0 and counts it down by the frame time once a frame
// (PTDE fn 0xDBE7B0). The update that finds it at or below zero marks the effect expired (+0x25); the next
// pass of the list update (PTDE fn 0xDC3020) applies its replaceSpEffectId and removes it. While an effect is
// active, applying it again is refused (unless its category refreshes it), so an effect that something applies
// every frame comes back on the update that marks the old one expired.
//
// The floor of Blighttown's swamp (HitMtrlParam 23) applies SpEffect 4020 every frame: 0.06 s, replaced on
// expiry by 4021, which takes 3 points off the poison resistance pool (stateInfo 2). One cycle is
// ceil(0.06 / dt) + 1 frames: 3 frames (0.1 s) at 30 FPS, 16 frames (0.067 s) at 240. The swamp poisoned
// 30 points a second at 30 FPS, 36 at 60, 43 at 144, 45 at 240 and 49 at 1000. Every short effect that is
// kept up by being applied again (floors, areas, auras) and every replace chain is timed the same way.
//
// The fix gives a new or refreshed node the remaining time that makes it expire on the update a native-rate
// frame would: with n = the frames the game counts down at the native rate (float steps of 1/rate until the
// time is at or below zero), the node is marked expired (n + 1) native frames after it was applied. At a
// higher rate the time is set to (n + 1) / rate - 1.5 * dt, so the update that sees it at or below zero comes
// (n + 1) / rate after the application, to the nearest frame. At the native rate and below, and on any frame
// at least 0.8 of a native frame long, the game's own time is kept. The effect's full duration (+4, read for
// the HUD icons) and the instant-effect test still use the game's value.

// Frames the game counts an effect of `life` seconds down at the native rate: float steps of 1/rate.
inline int effectlife_native_frames(float life, double rate) {
    const float step = static_cast<float>(1.0 / rate);
    const double frames = static_cast<double>(life) * rate;
    if (frames > 2048.0) return static_cast<int>(std::ceil(frames - 1e-3));
    float t = life;
    int n = 0;
    while (t > 0.0f && n < 4096) {
        t -= step;
        ++n;
    }
    return n;
}

// The remaining time to store for an effect of `life` seconds applied on a frame of `dt` seconds.
inline float effectlife_adjust(float life, double dt, double rate) {
    if (!(life > 0.0f) || !(dt > 0.0) || !(rate > 0.0) || dt >= 0.8 / rate) return life;
    const int n = effectlife_native_frames(life, rate);
    const double t = (n + 1) / rate - 1.5 * dt;
    if (!(t > 0.5 * dt)) return life;
    return static_cast<float>(t);
}
