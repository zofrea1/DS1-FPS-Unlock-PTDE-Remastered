#pragma once

#include <d3d9.h>

// ReversedDepth (experimental, default false; 2026-11-22). The game's depth buffers are 24-bit with the near plane at
// 5 cm and the far plane at 3000 m: a depth step is about 1.2e-6 * distance^2 metres, 1.2 cm at 100 m, 30 cm at 500 m,
// metres at the horizon. Surfaces closer than a few steps fight as the camera moves (the horizon against the cloud
// sea from the Undead Burg, distant towers): the game's own, and more visible at 4K. Reversed depth (far = 0, near =
// 1) in a 32-bit float buffer spends the float's precision where the 24 bits were short: about 1e-7 of the distance.
//
// Nothing of the game's matrices changes: every vertex shader's position output is rewritten (z = w - z) as it is
// created, the depth buffers are kept in 32-bit float by DXVK (d3d9.useD32forD24), and at the device the depth compares are mirrored (LESS <->
// GREATER), the depth clears (1 -> 0), the depth biases (negated) and the viewports' depth ranges; the pixel shaders
// that write depth (the game's depth copies) write 1 - depth, and a draw that samples a depth texture (the effects'
// upsample reads the 512x360 depth) gets a copy of its pixel shader that reads 1 - depth. Fixed-function projections
// (SetTransform) get the same z = w - z. Hooked first, beneath every other hook of this mod, so they all see the
// game's own values.
void depthrev_hook_device(IDirect3DDevice9* device, bool enabled);
// D3DPRESENT_PARAMETERS before CreateDevice/Reset: the automatic depth buffer's format.
void depthrev_adjust_present(D3DPRESENT_PARAMETERS* pp, bool enabled);
bool depthrev_active();
