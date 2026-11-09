#pragma once

#include <cstdint>

// Kick (forward + R1) and jumping attack (forward + R2): the stick flick, in both games.
//
// The player's PadManipulator turns the left stick into a "flick" every update (PTDE fn 0xE16F80, Remastered
// 0x140399290; the debug menu calls its values レバーはじき, "lever flick"). For each of the four directions:
// once the stick was neutral (every component at most 0.2) on the previous frame and is now past 0.3 in that
// direction, the flick in that direction is shown while its timer is at most 0.06 s, which at the native rate
// is 2 frames of 1/30 s (Remastered: 4 of 1/60 s). The character script (c0000.esd) does a kick when an R1
// request (action 0) comes with a forward flick (env 212: flick y > 0), and a jumping attack when an R2 request
// (action 5) does. Both tests were per frame:
//
//  * The stick has to get from 0.2 to 0.3 between two frames. A push that does that in 1/30 s at 30 FPS has
//    1/240 s at 240 FPS: a frame lands between 0.2 and 0.3, the neutral record is lost and no flick is made.
//    At 240 FPS an ordinary quick push (0 to full in 50 ms) never made one (at 30 FPS: 84% of the time).
//  * A press is seen on the frame after it happens. At 30 FPS a press up to a frame before the stick reached
//    0.3 still came with the flick; at 240 FPS it came 4 ms before it and was an ordinary attack.
//
// The fix keeps the game's rules and moves them onto the native frame grid in real time. A virtual 1/rate clock
// runs with the frame times; the neutral test looks at the stick one native frame before the native frame that
// first sees it past 0.3 (interpolated between real frames), and the flick is shown until the end of the native
// frame the game would last show it in. A fresh R1 or R2 press while the stick is on its way out of neutral
// toward forward (forward between 0 and 0.3 and rising, and a flick still possible this native frame) is held
// back until the flick starts or that native frame ends, the press a 30 FPS frame would have taken together with
// the stick. Presses with the stick at rest or held are never delayed. At the native rate and below, and on any
// frame at least 0.8 of a native frame long, the game's own per-frame rules run unchanged.
//
// Offline check (scratchpad flicksim.py): kick chance by press-to-stick timing and push speed at 144, 240 and
// 1000 FPS matches the original game's at 30 FPS within the sampling noise (and is identical at 30 and 20 FPS).

// Where the flick keeps its state in the manipulator.
struct StickFlickLayout {
    uint32_t neutral = 0;  // byte: the stick was neutral on the last update (PTDE +0x239, Remastered +0x23D)
    uint32_t timers = 0;   // 4 floats, one per direction (PTDE +0x2F4, Remastered +0x308)
};

// The game's own values (its debug menu can change them), read on every call like the game does.
struct StickFlickConsts {
    const float* window = nullptr;   // flick duration, 0.06 s
    const float* on = nullptr;       // flick threshold, 0.3
    const float* neutral = nullptr;  // neutral threshold, 0.2
};

// rate: the native frame rate to reproduce (30 for the original game, 60 for Remastered's own timing).
void stickflick_configure(const StickFlickLayout& layout, const StickFlickConsts& consts, double rate);

// The game's flick function, replaced. Same contract as the game's: out (16 bytes) gets the flicked stick
// (out[0] side, out[1] forward, 0 where no flick), the neutral byte and the four timers are updated. Returns out.
float* stickflick_compute(uint8_t* manip, float* out, float dt, float side, float forward);

// After PadManipulator::Update has written this frame's action flags (+0x84, hold times +0xF0): holds back a
// fresh R1 (actions 0 and 7) or R2 (action 5) press while it could still become a kick or a jumping attack.
void stickflick_after_update(uint8_t* manip);

// Counters for the log: flicks started on the real-time path, presses held, presses that became a flick.
struct StickFlickStats {
    uint32_t flicks = 0;
    uint32_t holds = 0;
    uint32_t held_into_flick = 0;
};
StickFlickStats stickflick_stats();
