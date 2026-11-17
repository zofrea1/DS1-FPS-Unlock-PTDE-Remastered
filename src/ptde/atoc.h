#pragma once

struct IDirect3DDevice9;

// AlphaToCoverage: with the game's own anti-aliasing on (Antialiasing in DarkSouls.ini: the scene in a multisampled
// target), alpha-tested surfaces (grass, leaves, hair, fences) write coverage from their alpha, sharpened around the
// alpha test's reference, instead of the hard alpha test, so their edges are smoothed by the multisampling like any
// other edge while they stay solid inside (see atoc.cpp).
void atoc_hook_device(IDirect3DDevice9* dev);
// RemoteControl `atoc 0/1/2`: off, plain coverage, sharpened coverage (the default).
void atoc_set_mode(int mode);
// AntialiasingSamples: the samples of the game's anti-aliasing (2 = the game's, 4 or 8). Before the game makes its
// render targets. False when the game's table does not match this build.
bool atoc_set_msaa_samples(int samples);
// FixAntialiasingDepth: the game's multisampled packed depth and the texture it is resolved into get 16 bits a channel
// (see atoc.cpp), so the resolve's average decodes to the right depth at the edges of grass and leaves. Install before
// the game makes its render targets (samples: AntialiasingSamples); check once the device exists (puts the game's
// format back when the driver cannot multisample it).
bool atoc_fix_depth_install(int samples);
void atoc_fix_depth_check(IDirect3DDevice9* dev);
