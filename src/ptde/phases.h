#pragma once

#include <cstdint>

// Phase times (diagnostics builds; RemoteControl `phases`): how long each subsystem call of the game's frame function
// (0xF91770: input, events, AI, physics, characters, effects, the scene) takes, timed with the time-stamp counter so
// it stays cheap on a phone (where the sampling profiler cannot see translated code). Each timed call site's call is
// sent through a small stub that reads the counter before and after the original callee. Sites are patched on the
// game thread at the start of a frame, so a site must be one only the game thread runs.

// Game thread, at the start of each frame (before the frame function): installs requested sites, reports every 5 s.
void phases_game_frame();
// Game thread, around the frame function: the whole frame's counter ticks (for the part no site covers).
void phases_frame_ticks(uint64_t ticks);
bool phases_active();
// Render thread, at each frame's start (names the thread `phases d3d` times).
void phases_render_frame();
// RemoteControl (runs on the game thread at the start of a frame): `phases on` times the frame function's own call
// sites; `phases add ADDR` also times the call at ADDR (an E8 call that only the game thread runs). Returns a short
// status for the remote output.
const char* phases_command(const char* verb, uint32_t address);
