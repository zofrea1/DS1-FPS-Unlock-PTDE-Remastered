#pragma once

// Diagnostic (INI Trace = true): F11 starts and stops a "deep" recording of the player object
// graph. At the start it collects every word of the player's character object (0x400 bytes), of
// each object it points to (0x100 bytes) and of each object those point to (0x40 bytes), then
// records all of them once per frame in memory. At the stop only the words that changed during
// the recording are written, with the path that reaches each one, to
// PTDE-FPS-Unlock-deep-N.csv. It finds state (angles, blend weights, timers) without knowing
// where it lives.
void deep_set_dir(const wchar_t* dll_path);

// Called from the render thread once per frame.
void deep_frame(double dt_ms, int target_fps);
