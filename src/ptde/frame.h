#pragma once

#include "settings.h"

// Hooks the draw thread's command fetch, the one place that runs once per rendered
// frame, so the mod can see (and later drive) frame timing.
//
// Phase 1b: survey only. Counts each of the six draw-thread commands and the time
// between them and logs a summary once a second, to find which command marks a
// frame boundary. The game is not changed.
bool frame_install(const Settings& settings);
