#pragma once

#include "settings.h"

// Hooks the draw thread's command fetch, the one place that runs once per rendered
// frame, and drives frame timing from it:
//  * limits the frame rate to MaxFPS with a precise wait, either before input is read
//    or just before Present (FPSLimitPoint);
//  * writes the measured frame time into the engine's fixed 1/30 step constant, so
//    game speed stays correct at any frame rate (and when the frame rate drops);
//  * optionally stops the render thread from waiting two vertical blanks per frame.
// It also logs a per-second survey for the first `survey_seconds`.
//
// Works with or without DSfix: if the call is already detoured by DSfix's FPS
// unlock, this hook sits in front, forwards to it, and writes its value afterwards.
bool frame_install(const Settings& settings);

// Diagnostics (RemoteControl "forcedt"): every frame steps the world by `seconds` whatever the measured frame time
// (0 = measured again). Lets two runs differ only in the step, so code that moves by a fixed amount per frame shows.
void frame_force_step(float seconds);

// When the draw thread last began a frame (performance counter ticks; 0 before the first). The game's time step is
// the time between two of these, so it is the time the frame's picture shows (dxgipresent.cpp paces flips by it).
long long frame_boundary_qpc();
