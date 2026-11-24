#pragma once

#include "settingspage.h"

// Pictures drawn over Remastered's finished frame (FPS Unlock Settings): the swap chain's Present is hooked (the
// method table of DXGI's swap chain class, found through the swap chain the game makes), and each frame
// FRAME(width, height) is called on the game's render thread with the back buffer's size; it queues what to draw with
// overlay_draw. Drawn with Direct3D 11 on the game's own device; every piece of state touched is put back.
bool overlay_install(void (*frame)(int width, int height));
// From DllMain, before the game makes its DXGI factory: its CreateDXGIFactory import is watched (nothing else).
bool overlay_early();
// During FRAME: IMG drawn with its top left corner at X, Y (pixels of the back buffer). SLOT 0..3 keeps a texture each.
void overlay_draw(int slot, const SettingsPageImage* img, int x, int y);
// Diagnostics builds: FILE (a full path) gets the next frame as a 32-bit BMP, the pictures drawn over it included.
void overlay_request_shot(const wchar_t* file);
// Diagnostics builds: presents a second over the last 2 s or so (0 before the first frames).
double overlay_present_rate();
