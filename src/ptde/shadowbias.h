#pragma once

struct IDirect3DDevice9;

// ShadowBias: the shadows on characters and objects compared a little nearer to the light, so lit, curved surfaces no
// longer show the shadow map's texels as a fine weave (see shadowbias.cpp). 0 = the game's own comparison.
void shadowbias_set(float bias);
void shadowbias_hook_device(IDirect3DDevice9* dev);
