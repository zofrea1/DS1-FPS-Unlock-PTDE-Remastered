#pragma once

// Optional borderless fullscreen. The game must be in windowed mode (in-game
// setting); this strips the frame and stretches the window over its monitor, which
// sidesteps exclusive fullscreen and its refresh-rate selection (the game always
// picks 60 Hz there). The display then runs at the desktop's refresh rate.
//
// A watcher thread finds the game window, applies the change, and re-applies it if
// the game (or anything else) restores the frame, for example after alt-tab.
bool borderless_start();

// Per-monitor DPI awareness (INI DpiAware). On a display scaled above 100%, Windows otherwise has the DPI-unaware game
// draw at the scaled-down ("logical") size and stretches its window up: a blurry picture, RenderResolution auto drawing
// more pixels than are shown, and in borderless no direct flip (so no variable refresh). Called from DllMain before the
// game makes its window (SetProcessDpiAwarenessContext must come before any window); the log is not open yet, so the
// outcome is kept for dpi_aware_log.
void dpi_aware_early();
void dpi_aware_log(bool wanted);
