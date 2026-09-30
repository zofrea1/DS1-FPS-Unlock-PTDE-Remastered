#pragma once

// Diagnostic recorder (INI Trace = true). F9 starts and stops a capture; each capture
// writes PTDE-FPS-Unlock-trace-N.csv next to the DLL with one row per rendered frame:
// the player's position and raw dwords of its character/movement/physics structures.
// Called from the render thread at every frame boundary.
void trace_frame(double dt_ms, int target_fps);
void trace_set_dir(const wchar_t* dll_path);
