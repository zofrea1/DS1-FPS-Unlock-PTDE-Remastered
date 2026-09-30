#pragma once

// Per-frame constants in the game's code that were tuned for a fixed 30 FPS, made
// frame-rate independent. Each fix retargets the 32-bit address operand of one load so it
// reads a value this module recomputes every frame from the measured frame time:
//
//   slide gravity   1.0  per frame      -> scaled by dt * 30
//   slide friction  0.65 per frame      -> 0.65 ^ (dt * 30)
//   air damping     0.95 per frame      -> 0.95 ^ (dt * 30)
//   timers          + or - 1/30 / frame -> dt
//   smoothing       lerp factor 1/30    -> 1 - (1 - 1/30) ^ (dt * 30)
//
// At exactly 30 FPS every value equals the game's original constant.
struct FixFlags {
    bool slide = true;    // slope gravity and ground friction of the movement controller
    bool damping = true;  // airborne horizontal momentum decay
    bool timers = true;   // per-frame 1/30 second timers (fades, countdowns)
    bool smoothing = true;  // per-frame 1/30 interpolation factor
};

bool fixes_install(const FixFlags& flags);

// Called on the render thread at every frame boundary with the frame time in seconds.
void fixes_update(double dt);

// Ctrl+1..4 toggle the groups at run time (diagnostic; INI Trace = true).
void fixes_poll_hotkeys();
