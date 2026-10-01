#pragma once

#include "settings.h"

// Fixed-rate player physics (INI FixedRatePhysics, on by default): the player's movement physics run
// at 30 steps a second, exactly as in the original game, whatever the frame rate, and the visible
// position is interpolated between steps.
//
// The original game decided several things once per 1/30 s step: whether a step-up succeeds, whether
// the character rides a ledge, the ground snap and the graze check. Scaling the per-frame constants
// (fixes.cpp) gets the rates right but not those discrete decisions; running the same step at the
// same rate does.
//
// Per character, every frame (ChrIns update loop, fn 0xE41640):
//   vfunc 0x4C   animation; writes this frame's root-motion displacement to [phys+0x60]
//   0xE3A8A0     the physics step (fn 0xEC3A90) and then the graze check
//   vfunc 0x50   (fn 0xE83BC0) syncs [phys+0x10] from the Havok proxy (fn 0xEBDA70, the previous value
//                goes to [phys+0x20]) and builds the character's transform from it
// For the player only:
//   * 0xE3A8A0 runs on step frames only (once 1/30 s has accumulated), with the accumulated time as
//     its dt (the dt-scaled fixes see the same time) and the root motion of the skipped frames added
//     up, so the step moves exactly as far as the original game's would.
//   * The graze check measures [phys+0x10] - [phys+0x20], the movement of the last sync. On a step
//     frame that is the last step's movement again, not the zero of the skipped frame before it.
//   * After each sync, [phys+0x10] is set to the interpolation between the last two step results by
//     the time since the last step (one step of delay, like any fixed-step interpolation). The true
//     position goes back in before the next step. A move the physics did not make (warp, grab,
//     ladder) is taken over directly instead of being interpolated.
//
// With Trace = true, Ctrl+8 toggles it while playing.
bool physhz_install(const Settings& settings);

// Called from the render thread once per frame with the frame time in seconds, before the
// dt-scaled fixes are updated.
void physhz_frame(double dt);

// Ctrl+8 (diagnostic; INI Trace = true).
void physhz_poll_hotkeys();
