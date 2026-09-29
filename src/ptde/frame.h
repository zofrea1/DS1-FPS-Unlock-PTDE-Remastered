#pragma once

#include "settings.h"

// Hooks the draw thread's command fetch, the one place that runs once per rendered
// frame, and drives frame timing from it:
//  * limits the frame rate to TargetFPS with a precise wait;
//  * writes the measured frame time into the engine's fixed 1/30 step constant, so
//    game speed stays correct at any frame rate (and when the frame rate drops);
//  * optionally stops the render thread from waiting two vertical blanks per frame.
// It also logs a per-second survey for the first `survey_seconds`.
//
// Works with or without DSfix: if the call is already detoured by DSfix's FPS
// unlock, this hook sits in front, forwards to it, and writes its value afterwards.
bool frame_install(const Settings& settings);
