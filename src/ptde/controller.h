#pragma once

#include "settings.h"

// Teach the game to keep a real XInput gamepad when its four Windows 7 device ids
// do not match, and to survive a slot going quiet for a moment. No game memory is
// written when FixController is false.
bool controller_install(const Settings& settings);
