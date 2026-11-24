#pragma once

// FPS Unlock Settings in Remastered: the in-game page (src/common/settingspage.cpp) with the settings a player would
// change while playing: the frame rate cap, and which game's feel the movement and timing switches keep (the
// original game's or Remastered's). Opened with SettingsMenuKey (F1) or Y on a controller in the PC settings screen;
// drawn over the frame with Direct3D 11 (overlay.cpp).
struct Settings;

// From the start-up thread, after the settings are read and before the game makes its swap chain.
void settingsmenu_install(const wchar_t* dll_path, const Settings& settings);
