#pragma once

#include <d3d9.h>

// ShadowMapResolution: the side of the shadow map (the game's is 2048; 4096 or 8192). Before the game sets up its
// graphics. False when the game's code does not match this build or the size is not one of those.
bool shadowmap_set_resolution(int size);

// With a larger map and the game's device a wrapper's (DSfix 3.0): the shadow map's scissor rectangles are checked on
// the device beneath the wrapper and put back when it did not pass them on. Right after the device is made.
void shadowmap_hook_device(IDirect3DDevice9* dev);
