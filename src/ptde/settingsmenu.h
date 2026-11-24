#pragma once

#include "settingspage.h"

// FPS Unlock Settings: an in-game page with the settings a player would change while playing (see settingsmenu.cpp).

struct IDirect3DDevice9;

// After the settings are read (start-up thread). key: the virtual key that opens the page (0 = none); the
// PC settings screen opens it with Y on a controller either way.
void settingsmenu_install(const wchar_t* ini_path, int key);
bool settingsmenu_enabled();
// The device's Present, before the frame goes out (render thread): reads the keys, draws the page.
void settingsmenu_present(IDirect3DDevice9* device);
// Before the device's Reset.
void settingsmenu_reset();
// Every XInput read (xinput_proxy.cpp): takes the buttons for the page, and gives the game none while it is open.
void settingsmenu_pad(unsigned long index, void* state, unsigned long result);
bool settingsmenu_is_open();
void settingsmenu_set_open(bool open);  // RemoteControl `settings 0|1`

// The rows of a page (SettingsMenuRow, SettingsMenuChoice) are settingspage.h's. A bundled mod (Prepare to Co-op
// Edition) adds a page of its own.
void settingsmenu_add_page(const wchar_t* title, const SettingsMenuRow* rows, int count);
