#pragma once

// ShadowMapResolution: the side of the shadow map (the game's is 2048; 4096 or 8192). Before the game sets up its
// graphics. False when the game's code does not match this build or the size is not one of those.
bool shadowmap_set_resolution(int size);
