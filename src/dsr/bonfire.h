#pragma once

// Bonfire softlock watchdog. After rest or "Reverse hollowing" the menu can close while the
// character stays seated. When that lasts a second, the character is told to stand up.
// Only the supported 2022 executable is touched. Returns false if the exe does not match or
// the thread could not be created.
bool bonfire_start();
