#pragma once

#include "settings.h"

// SANDBOX (branch ptde-fidelity). Experiment: run the player's movement physics at a fixed 30 or
// 60 steps a second, whatever the frame rate, the way the original game did.
//
// Two routines are involved, both once per frame for every character:
//   * the physics controller update (fn 0xEC3A90, stdcall(controller, dt)): turns the commanded
//     velocity into a displacement and does the step-up / ground snap;
//   * the graze check (fn 0xE3A8A0, edi = movement controller): measures the displacement of the
//     last frame.
// For the player only, both are skipped on frames where less than 1/Hz of time has accumulated; on
// the frame that completes a step the controller runs once with the accumulated time as its dt
// (and the dt-scaled fixes use that time too). Everything else (animation, camera, rendering,
// other characters) keeps running every frame. There is no interpolation, so the player's
// position moves in steps of Hz per second: this is a test of whether the small steps in the
// frame time cause the movement differences from the original, not a finished feature.
//
// INI [Experimental] PhysicsHz = 0 (off), 30 or 60. With Trace = true, Ctrl+8 cycles 0 -> 30 -> 60.
bool physhz_install(const Settings& settings);

// Called from the render thread once per frame with the frame time in seconds, before the
// dt-scaled fixes are updated.
void physhz_frame(double dt);

// Ctrl+8 (diagnostic; INI Trace = true).
void physhz_poll_hotkeys();
