#pragma once

#include <cstdint>

// Diagnostic (SfxWatch = <effect id>): find the code that reads an effect's emitter timing, to check
// FixSfxSpawnRate against what the game does with it.
//
// When the effect loads (sfx.cpp's registration hook), the first emitter timing block of type 2 (pond3 type 2:
// lifetime +4, an offset +8, a count +0xC, the spawn interval +0x10) is found; on the next frame four hardware
// watchpoints (read or write) go on those fields in every thread. Each instruction that touches them is logged
// once with its address, how often it did, the value, its thread and the return addresses on its stack; the
// watch is lifted 20 s after its first hit or at 5,000 hits, and moves to a new copy if the effect loads again. The undead dragon's acid effect, 16012, holds two one-frame bursts
// (lifetime 1/60, offset -1/60, interval 1/60), which FixSfxSpawnRate gives an interval of 1/30.
void sfxwatch_set_target(int effect_id);
bool sfxwatch_wanted();
// From the registration hook, before FixSfxSpawnRate changes the data.
void sfxwatch_on_register(const uint8_t* data, uint32_t size);
// Once a frame, on the game thread.
void sfxwatch_tick();
