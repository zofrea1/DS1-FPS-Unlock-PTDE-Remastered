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
// Before a device Reset: releases the reduced copies (default pool).
void resolution_reset();
