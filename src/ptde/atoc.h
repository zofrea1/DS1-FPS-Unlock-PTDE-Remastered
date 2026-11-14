#pragma once

struct IDirect3DDevice9;

// AlphaToCoverage: with the game's own anti-aliasing on (Antialiasing in DarkSouls.ini: the scene in a multisampled
// target), alpha-tested surfaces (grass, leaves, hair, fences) write coverage from their alpha, sharpened around the
// alpha test's reference, instead of the hard alpha test, so their edges are smoothed by the multisampling like any
// other edge while they stay solid inside (see atoc.cpp).
// force: use NVIDIA's switch even when the driver does not report it (Proton's own DXVK on the Steam Deck answers no
// for both switches; a DXVK that does not know the value just keeps it as a render state).
void atoc_hook_device(IDirect3DDevice9* dev, bool force = false);
// RemoteControl `atoc 0/1/2`: off, plain coverage, sharpened coverage (the default).
void atoc_set_mode(int mode);
// AntialiasingSamples: the samples of the game's anti-aliasing (2 = the game's, 4 or 8). Before the game makes its
// render targets. False when the game's table does not match this build.
// 0: none (the option draws as if off; DSfix's resolution needs that).
bool atoc_set_msaa_samples(int samples);
