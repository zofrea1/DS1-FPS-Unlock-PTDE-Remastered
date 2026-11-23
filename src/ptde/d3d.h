#pragma once

#include <windows.h>

// Fullscreen refresh rate override.
//
// In exclusive fullscreen the game only ever gets 59 or 60 Hz. The game imports just
// Direct3DCreate9, so this wraps that import (early, from DllMain, before the game
// calls it) and hooks CreateDevice on the object it returns and Reset on the devices
// it creates. That works whether the objects are the real Direct3D 9 ones or DSfix's
// wrappers, because the hook goes on whatever the game is actually handed. When the
// game asks for a fullscreen device, the refresh rate in the present parameters is
// replaced with the chosen one (the highest the display offers at that resolution, or
// the configured rate).
//
// With DXVK = true the same wrapper first routes the system Direct3DCreate9 to DXVK
// (dxvk.h), so the objects above are DXVK's, still wrapped by DSfix if it is there.
void d3d_install_early(HMODULE self);

// Whether the game's Direct3D 9 will be DXVK's (DXVK = true, not under Proton / Wine, dxvk_d3d9.dll beside this DLL),
// before Direct3DCreate9 is called; dxvk_route's later checks (another Direct3D 9 replacement) can still say no.
bool d3d_dxvk_expected();
// Proton / Wine: the game's Direct3D 9 is the prefix's own (Proton's DXVK), whatever DXVK says.
bool d3d_under_proton();
// After the game's first Direct3DCreate9: whether it went to this mod's DXVK (BorderlessFullscreen = auto).
bool d3d_created();
bool d3d_dxvk_active();

// Content-cadence probe: a hash of every vertex shader constant upload since the previous call
// (matrices, skinning palettes). The present hooks compare consecutive frames to see how often
// the scene actually changes, which tells a 30 Hz update from a 120 Hz one.
unsigned long long d3d_take_frame_hash(unsigned long long* small_hash, unsigned long long* big_hash);
