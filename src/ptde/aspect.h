#pragma once

#include <d3d9.h>

// 16:10 (and any display taller than 16:9). The game's projection is locked to 16:9,
// so the extra vertical pixels are black bars. Off by default only when the INI says
// so; a 16:9 display is left alone either way.
void aspect_set_enabled(bool on);

// True when the loaded settings asked for the fix. The display may still be 16:9.
bool aspect_is_enabled();

// True once a taller-than-16:9 display has been seen and the fix is applying.
bool aspect_is_active();

// Size the swap chain to the display when the game asked for 16:9 on a taller screen,
// and point the game's camera constants at that aspect (horizontal view unchanged).
void aspect_adjust_present(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp, const char* where);

// Viewport, scissor, stretch and Present hooks so a 16:9 letterbox is opened up.
// SetVertexShaderConstantF stays owned by d3d.cpp, which calls aspect_correct_constants.
void aspect_hook_device(IDirect3DDevice9* device);

// If data begins with a 16:9 perspective matrix, write a Vert+ version into out
// (out must hold count*4 floats) and return true. out is filled only on success.
bool aspect_correct_constants(const float* data, UINT count, float* out);
