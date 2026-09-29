#pragma once

// Optional borderless fullscreen. The game must be in windowed mode (in-game
// setting); this strips the frame and stretches the window over its monitor, which
// sidesteps exclusive fullscreen and its refresh-rate selection (the game always
// picks 60 Hz there). The display then runs at the desktop's refresh rate.
//
// A watcher thread finds the game window, applies the change, and re-applies it if
// the game (or anything else) restores the frame, for example after alt-tab.
bool borderless_start();
