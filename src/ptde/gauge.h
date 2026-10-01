#pragma once

// Diagnostic (INI Trace = true): Scroll Lock starts and stops a recording of every HUD gauge object
// (the player gauge and the generic and smooth gauges it is built from), one sample per frame, to
// PTDE-FPS-Unlock-gauge-N.csv. Only the words that changed are written.
void gauge_set_dir(const wchar_t* dll_path);

// Called from the render thread once per frame.
void gauge_frame(double dt_ms);
