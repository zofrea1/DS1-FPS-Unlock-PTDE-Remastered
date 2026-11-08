#pragma once

#include <cstdint>

// Diagnostic (SfxWatch = <effect id>): find the code that reads an effect's emitter timing, to check
// FixSfxSpawnRate against what the game does with it.
//
// The effect library (FXSerializer) reads an effect file once, from a temporary buffer, into its own objects; the
// buffer is then freed (zlib reused it at once, 2026-11-07 20:38). So:
//  1. Load: when the effect registers (sfx.cpp's hook), the first emitter timing block of type 2 (pond3 type 2:
//     lifetime +4, an offset +8, a count +0xC, the spawn interval +0x10) gets four hardware watchpoints (read or
//     write) on the loading thread for the call that reads the file; each instruction that touches them is logged
//     with its registers.
//  2. Runtime: the library's copy of those values is searched for from the resource the call returns, and watched
//     in every thread from the next frame until 90 s after its first hit: the emitters that use them.
// The undead dragon's acid effect, 16012, holds two one-frame bursts (lifetime 1/60, offset -1/60, interval 1/60),
// which FixSfxSpawnRate gives an interval of 1/30.
void sfxwatch_set_target(int effect_id);
bool sfxwatch_wanted();
// From the registration hook: before the game reads the effect (and before FixSfxSpawnRate changes it); true when
// the load watch is set for this one.
bool sfxwatch_before_load(const uint8_t* data, uint32_t size);
// After the game's registration returned (its result: the effect's resource holder).
void sfxwatch_after_load(const void* resource);
// Once a frame, on the game thread.
void sfxwatch_tick();
