#pragma once

struct IDirect3DDevice9;

// AlphaToCoverage: with the game's own anti-aliasing on (Antialiasing in DarkSouls.ini: the scene in a multisampled
// target), alpha-tested surfaces (grass, leaves, hair, fences) write coverage from their alpha, sharpened around the
// alpha test's reference, instead of the hard alpha test, so their edges are smoothed by the multisampling like any
// other edge while they stay solid inside (see atoc.cpp).
void atoc_hook_device(IDirect3DDevice9* dev);
// RemoteControl `atoc 0/1/2`: off, plain coverage, sharpened coverage (the default).
void atoc_set_mode(int mode);
// RemoteControl `aadepth N` (diagnostics): FixAntialiasingDepth's pieces left out for comparisons (atoc.cpp).
void atoc_set_depth_diag(int bits);
// AntialiasingSamples: the samples of the game's anti-aliasing (2 = the game's, 4 or 8). Before the game makes its
// render targets. False when the game's table does not match this build.
bool atoc_set_msaa_samples(int samples);
// FixAntialiasingDepth: the game's multisampled packed depth and the texture it is resolved into hold one float a
// sample, written by converted copies of the shaders that draw it (16 bits a channel where the driver cannot use a
// float target; see atoc.cpp), so the resolve's average is the right depth at the edges of grass and leaves. Install
// before the game makes its render targets (samples: AntialiasingSamples); check once the device exists (picks the
// format, hooks the draws, or puts the game's format back).
bool atoc_fix_depth_install(int samples, bool force16 = false);
void atoc_fix_depth_check(IDirect3DDevice9* dev);
// Before a device Reset: our float copy of a depth resolved into an 8-bit texture goes (made again when needed).
void atoc_reset();
