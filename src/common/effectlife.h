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

// SpEffect ticks on the native frame grid (INI FixSpEffectTiming).
//
// An effect with a tick interval (SpEffectParam motionInterval > 0: lava's 125 points every 0.1 s (row 4000), the
// heals over time 5044 / 5050-5052 / 5083, the percentage heals and drains 5102, 5440, 5540-5542 every 0.1 s, rows
// 64, 5444, 5447 every 0.2 s, and the many 1 s and 3 s ones) applies its change on the update that finds its tick
// timer (node +8) at or below zero, then sets the timer back to the interval, not adding it, and counts it down by
// the frame time in the same update (PTDE fn 0xDBE7B0). So each tick waits whole frames: float steps of 1/30 in the
// original game, where 0.1 s needs 3 frames (10 ticks a second), float(0.2) needs 7 (4.3 a second) and 0.5 s 16.
// At other rates the rounding differs (simulated: tools/ticksim.py): 0.1 s ticks
// 14% less often at 60 FPS, 8% at 120, 4% at 144; 0.2 s 8% (60) to 17% (75, 120, 360) more often; 0.5 s 4-7% more;
// 1 s up to 1.6% less, 3 s 1% more.
//
// The fix sets the timer at a tick to n / rate minus how late this tick came (the timer's value at or below zero,
// at most one frame): with n the frames the game counts the interval down at the native rate, the ticks keep the
// native period n / rate on average, each on the first frame at or after its native time. Simulated against the
// original at 30 FPS: within 0.2% at 60 to 1000 FPS, also with frame times varying +-25%. It applies at the native
// rate too: the original stepped exactly 1/30 s a frame, which the fix reproduces (within 0.06%), while this mod at
// 30 FPS steps the real frame time, a little more or less than 1/30 s, and the game's own reset then waits a fourth
// frame for about half of the 0.1 s ticks (15% fewer, measured in game: 49 instead of 60 HP/s for row 4017).
inline float effecttick_next(float timer, float interval, double dt, double rate);

// Bullet hit records on the native frame grid (INI FixBulletHitTiming).
//
// A bullet that can hit the same target more than once (BulletParam dmgHitRecordLifeTime > 0: the clouds of
// Poison Mist and Toxic Mist 0.2 s (rows 2510, 2610), Acid Surge 0.4 s, the Vow of Silence and Tranquil Walk of
// Peace areas 0.5 s, Chaos Storm 1.5 s, Great Chaos Fireball's lava 2 s, and enemy multi-hit attacks) keeps a
// shared record of the targets it hit, each entry with the remaining time before that target can be hit again. The bullet manager's update (PTDE fn 0xDEA960) first
// counts every entry down by the frame time (fn 0xDF2540) and drops the ones at or below zero, then updates the
// bullets, whose hit test (fn 0xDF12E0) skips targets still in the record. So a target standing in the area is
// hit again after the whole frames the countdown takes: float steps of 1/30 in the original game, where
// float(0.2) needs 7 (0.233 s) and 0.15 needs 5 (0.167 s). At 240 FPS the same records last 0.2 and 0.15 s,
// so those areas hit 17% and 11% more often, 0.5 s ones (16 frames) 7% and 2 s ones (61 frames) 2%.
//
// The fix gives a new entry (fn 0xFD7CF0, the one place entries get their time) n / rate - dt / 2, with n the
// frames the game counts it down at the native rate, so the update that drops it comes n native frames after
// the hit, to the nearest frame. At the native rate and below, and on frames at least 0.8 of a native frame
// long, the game's time is kept.
inline float hitrecord_adjust(float life, double dt, double rate) {
    if (!(life > 0.0f) || !(dt > 0.0) || !(rate > 0.0) || dt >= 0.8 / rate) return life;
    const int n = effectlife_native_frames(life, rate);
    const double t = n / rate - 0.5 * dt;
    if (!(t > 0.5 * dt)) return life;
    return static_cast<float>(t);
}

inline float effecttick_next(float timer, float interval, double dt, double rate) {
    if (!(interval > 0.0f) || !(dt > 0.0) || !(rate > 0.0)) return interval;
    const double period = effectlife_native_frames(interval, rate) / rate;
    double late = -static_cast<double>(timer);
    if (!(late > 0.0)) late = 0.0;
    if (late > dt) late = dt;
    return static_cast<float>(period - late);
}
