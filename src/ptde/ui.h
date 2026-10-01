#pragma once

#include "settings.h"

// HUD and menu fixes for code that counts frames instead of seconds, plus a watchdog for the
// bonfire softlock:
//
//  * The loading screen's bonfire swirl advances two counters by one every frame it is drawn;
//    they are advanced by elapsed time * 30 instead (swirl).
//  * Bonfire softlock watchdog: after "Reverse hollowing" at a bonfire the character can stay
//    seated with no menu open. When that state lasts a second the character is told to stand up
//    (bonfire).
//  * Diagnostic (INI InputLog = true): logs the HUD shortcut actions (D-pad) as the game
//    receives them, with timestamps (input).
bool ui_install(const Settings& settings);
