#pragma once

#include "settings.h"

// Instruments the game's five Present call sites (three thin wrappers around
// IDirect3DDevice9::Present) and, optionally, disables the catch-up loop that
// re-presents a frame until the display's vblank counter reaches the game's
// schedule (a frame-repeat scheme written for 30 FPS content on a 60 Hz display).
//
// Logs once a second for `survey_seconds`: how many presents each site made, how
// long they blocked, and a histogram of the gaps between consecutive presents, so
// an uneven cadence (what makes a VRR display bounce between refresh rates) shows.
bool present_install(const Settings& settings);
