#pragma once

// Internal render resolution (work in progress, 2.3.0).
//
// The game draws its 3D scene into render targets the render target manager (NS_FRPG::RenderTargetManImp, constructor
// 0x100A1E0) creates at fixed sizes: 1024x720 for the scene, its depth and copies, 512x360 and 256x180 for depth of
// field and glare, 128x90 for the bloom. Before the constructor runs, the sizes written into it are replaced: 1024x720
// becomes width x height and the smaller ones keep their ratio to it (1/2, 1/4, 1/8).
// Returns false and changes nothing when the code does not match this build.
bool resolution_install(int width, int height, bool scale_post = false);

struct IDirect3DDevice9;
// From CreateDevice: the hooks that hand the post-processing copies a 1024x720 version of the scene.
void resolution_hook_device(IDirect3DDevice9* device);
// DSfix is there: the original-size post targets are made under its device wrapper (before resolution_hook_device).
void resolution_bypass_dsfix(bool on);

// Before a device Reset: releases the reduced copies (default pool).
void resolution_reset();
// The frame's last scale-down when it is larger than the screen (Supersampling): exact area average (on by default;
// RemoteControl "reduce 0/1" for comparisons).
void resolution_set_present_reduce(bool on);
// Diagnostics: switch single replacements off (bit i = kSwaps[i], bit 7 = the area-average copies).
void resolution_set_swap_off_mask(int mask);

// The depth of field's smooth composite on or off (RemoteControl `dof`, for comparing with the game's own).
void resolution_set_smooth_dof(bool on);
// DepthOfFieldStrength: the depth of field's blur weight times this (1 = the game's).
void resolution_set_dof_strength(float strength);
// DepthOfFieldResolution: 100 = the depth of field's blur levels made at half (and a quarter of) the render size
// with the game's widths; other values scale those widths in percent (RemoteControl `dofhr`); 0 = the game's 512x360
// and 256x180 levels (read smoothly).
void resolution_set_dof_resolution(int percent);
// Diagnostics: the composite shows only blur level 1-3 everywhere (0: the game's weights).
void resolution_force_dof_level(int level);

// EffectsResolution: the game's low-resolution effects (glows, mist, fog, smoke, drawn into a 512x360 target and
// stretched over the scene) drawn larger. 0 the game's; 1 at the scene's size against its own depth, 8-bit target; 2
// the same in a 16-bit float target laid over the scene with a fine dither (8-bit without float blending); 3 at half
// the scene's size in float, over a half-size depth, laid over by the game's own upsample (the default). Before
// resolution_install (which turns it off with RenderScalePost); RemoteControl `effects` any time.
void resolution_set_effects(int mode);

// HighPrecisionColor: the scene's colour targets in 16-bit float and the final picture dithered into 8 bits (smooth
// fog, smoke and glows). Before resolution_install; not with RenderScalePost or DSfix.
void resolution_set_high_precision_color(bool on);

// Sun and moon with DSfix's render resolution. The lens flare (sun, moon) measures how much of a 24x24 pixel probe at the
// sun is visible with an occlusion query, and takes a new count only while it is below 1.5 x the probe's expected
// area (w x h x 0.8; 0.8 = the 1024 wide scene in the 1280 wide frame buffer; 0xFD3A20). DSfix enlarges the scene and
// its viewports behind the game's back, so the probe grows with it: once the sun is uncovered its count is far above
// that limit and is never taken, and the sun or moon stays hidden until it leaves the screen and comes back. Here the
// expected area grows by DSfix's enlargement too. (Our own RenderResolution needs nothing: the game sizes its frame
// buffer and viewports itself and the probe stays 24x24.)
bool resolution_dsfix_sun(int render_width, int render_height);
