#pragma once

// Event script waits on the original game's 30 FPS frames (INI FixEventWaits; see src/common/eventwait.h).
//
// EcTestPassTime (vtable 0x011DE3FC) counts a wait down in its test, slot 1 (fn 0xBB22D0, thiscall(dt, unused),
// ret 8): remaining time at +0x18, done bit 0 of +8. The slot points to our test instead, which counts the wait
// in game time and ends it on the frame nearest to the original's end.
bool eventwaits_install();

// RemoteControl `eventwaits 0|1` (diagnostics): the game's own countdown (0) or ours (1), for A/B tests.
void eventwaits_set(bool on);
