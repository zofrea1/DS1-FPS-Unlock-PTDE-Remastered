#pragma once

#include <cstdint>

// Switchback (reversal) animation at any frame rate (INI FixSwitchback, both games).
//
// NS_FRPG::TurnAnim keeps a ring of the stick's world direction ("angle", PTDE +0x80, Remastered +0x90) and of
// the turn it asks for ("dir", PTDE +0xF8, Remastered +0x108), one entry per update (vtable slot 3: PTDE 0xE13160,
// Remastered 0x14039F5C0), index PTDE +0x170 / Remastered +0x180, length +0x174 / +0x184 (the debug menu's
// "reference frames for the turn animation", 2). While running or sprinting, the movement update asks
// (PTDE 0xE10DB0, Remastered 0x14039F980) whether the direction swung more than 105 degrees since the oldest
// entry; if so the character plays the switchback animation (the turn from the entry before the largest swing).
// With 2 entries that is one frame: 1/30 s in the original game, 1/240 s at 240 FPS, so a quick reversal while
// running almost never played the switchback at a high frame rate.
//
// The fix compares on the native frame grid in real time: the reference is the stick direction (length - 1) native
// frames before the native frame the game would next look at, every sample since then counts, and it fires at most
// once per native frame, as the game can only look once per frame. At the native rate and below, and on any frame
// at least 0.8 of a native frame long, the game's own check runs.

struct TurnHistoryLayout {
    uint32_t angle = 0;  // float ring: world direction of the stick
    uint32_t dir = 0;    // float ring: the turn asked for
    uint32_t index = 0;  // int: entry written by the last update
    uint32_t size = 0;   // int: ring length
};

void turnhistory_configure(const TurnHistoryLayout& layout, double rate);

// After the game's ring writer ran for `turn` on a frame of `dt` seconds.
void turnhistory_record(const uint8_t* turn, float dt);

// The game's switchback check, on the native grid. Returns -1 when the game's own check should run (native rate or
// below, or no history yet), else 0 / 1 with *out set like the game's on 1.
int turnhistory_check(const uint8_t* turn, float threshold_deg, uint32_t* out);
