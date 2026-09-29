#pragma once

#include <windows.h>

// Optional sampling profiler for the render thread (INI Profile = true).
//
// Press F10 in a busy scene: for 15 seconds a background thread samples the render
// thread's instruction pointer about a thousand times a second, then logs which
// modules (the game, the Direct3D 9 runtime, the graphics driver, DSfix...) the time
// went to and the hottest code addresses. Diagnostic only.
bool profile_start();

// Set by the frame hook on the first frame boundary (the render thread).
extern volatile DWORD g_render_thread_id;
