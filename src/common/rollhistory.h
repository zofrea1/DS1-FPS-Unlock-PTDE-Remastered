#pragma once

#include <cstdint>

// Roll direction (INI FixRollDirection, both games).
//
// The player's PadManipulator keeps a ring of the left stick's dominant axis (PTDE fn 0xE17600: forward/back at
// +0x250, side at +0x2A0, index +0x2F0; Remastered fn 0x140398920: +0x264, +0x2B4, +0x304), one entry per frame.
// The debug menu calls its length レバー入力期間（フレーム） ("lever input period (frames)"): 6. Every update the
// game walks back over the newest 5 entries with a weight that starts at 1 and is multiplied by 0.9 per entry, and
// keeps the weighted value with the largest magnitude per axis. When the roll button is let go, that decides the
// roll's direction (forward, back, left, right); with nothing in it the character backsteps if the stick is
// neutral and the press armed one, or does nothing. At 30 FPS the 5 entries reach back 0.13 s (Remastered at 60:
// 0.067 s); at 240 FPS only 0.017 s, so a push let go of just before the roll button came out as a backstep (or as
// nothing) instead of a roll that way.
//
// The fix computes the same weighted maximum over real time: every sample of the last 4 native frames, weighted
// 0.9 per native frame of its age. At the native rate and below, and on any frame at least 0.8 of a native frame
// long, it is the game's own walk over its own ring, unchanged.

struct RollHistoryLayout {
    uint32_t ring_y = 0;  // forward/back, one float per entry
    uint32_t ring_x = 0;  // side
    uint32_t index = 0;   // int: the entry written this update
};

// ring_size: the game's own length (its debug value, 6), read on every call; rate: native frames per second.
void rollhistory_configure(const RollHistoryLayout& layout, const int* ring_size, double rate);

// Called where the game walks its ring, after it wrote this update's entry. out: [0] weighted forward/back,
// [1] weighted side, [2] this update's forward/back, [3] this update's side.
void rollhistory_compute(uint8_t* manip, float dt, float* out);
