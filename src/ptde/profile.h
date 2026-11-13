#pragma once

#include <windows.h>

struct IDirect3DDevice9;

// Performance profiler (INI Profile = true, diagnostics builds only).
//
//  * Every 5 s a [perf] line: frames per second, how long the game thread's update and the render thread's frame
//    took, how busy both threads were (CPU cycles), Direct3D 9 calls per frame by kind (draws, constant uploads,
//    state changes, locks and query waits, timed where they can stall) and the player's position.
//  * F10: for 10 s every busy thread of the game is sampled about a thousand times a second; the [prof] lines then
//    give, per thread, its CPU use, the modules and the hottest code, the game's call sites that time was spent
//    under (return addresses found on the stack) and, for time in Direct3D, the driver or DSfix, which game code
//    called it.
// Diagnostic only; a release build compiles these as empty stubs.
bool profile_start();
// Before profile_start: the spike watch on or off (ProfileSpikes; each of its samples stops the game thread, which
// costs a lot under Wine on another CPU) and the capture's pause between sampling rounds in ms (ProfileCaptureMs).
void profile_configure(bool spikes, int capture_ms);
bool profile_enabled();
// The F10 capture without the key (RemoteControl `capture`).
void profile_request_capture();
// A frozen game (the watchdog saw no frame for `seconds`): one [stall] line per thread with where it is, its
// registers and the return addresses on its stack. Works without Profile; a release build leaves it out.
void profile_stall_report(int seconds);

// Counts the game's calls on its Direct3D 9 device (the object the game holds, DSfix's wrapper when DSfix is
// there) and times Present, buffer and texture locks, query waits and render target read-backs.
void profile_hook_device(IDirect3DDevice9* device);

// The game thread's update (WinMain's frame call), measured around the call. Its start also arms the spike watch:
// an update that runs past max(4 ms, 3x the average) is sampled every millisecond until it ends, and logged as a
// [spike] line (hottest code, the game and DLL call sites it was under, time since the last spike).
void profile_game_frame_begin();
void profile_game_frame(double ms);
// The render thread's frame: once at each frame boundary, and the time from the start of its frame work (command 2)
// to its end (command 4).
void profile_frame_boundary();
void profile_render_span(double ms);

// Set by the frame hook on the first frame boundary (the render thread) and the first game update (the game
// thread).
extern volatile DWORD g_render_thread_id;
extern volatile DWORD g_game_thread_id;
