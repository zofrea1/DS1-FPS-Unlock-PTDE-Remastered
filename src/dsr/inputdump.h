#pragma once

// Diagnostic (INI InputLog = true): F12 starts and stops a recording of the game's input state
// object (the singleton the HUD asks about held buttons), sampled about every millisecond, to
// DSR-FPS-Unlock-input-N.csv. Only the words that changed are written.
void inputdump_start(const wchar_t* dll_path);
