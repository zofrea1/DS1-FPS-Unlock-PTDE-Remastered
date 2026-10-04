#pragma once

#include <d3d9.h>

// The game's brightness setting in every display mode.
//
// The game sets its brightness as a Direct3D gamma ramp (SetGammaRamp, from its gamma scene entity).
// Direct3D 9 applies a gamma ramp only to an exclusive fullscreen swapchain and ignores it when the game
// is windowed, borderless, or run through DXVK in a windowed swapchain (DXVK keeps the same rule: it
// applies the ramp only when the swapchain is not windowed). The picture then lacks the game's curve:
// darker, with crushed blacks, than the same game in fullscreen.
//
// With WindowedGamma on, whenever the swapchain is windowed and the game's ramp is not the identity, the
// ramp is applied to the finished frame right before it is presented: one full-screen draw that looks
// each channel up in a 256-entry texture made from the ramp (all device state is saved and restored).
// It runs on the device that actually presents (beneath DSfix), after everything else, as the display's
// ramp would. In exclusive fullscreen nothing changes: Direct3D (or DXVK) applies the ramp itself.

// After the game's device is created (the device the game holds, possibly DSfix's wrapper).
void gamma_attach(IDirect3DDevice9* game_device, bool enabled);
// Around IDirect3DDevice9::Reset on the game's device: resources that do not survive a reset go first,
// and the display mode is read again afterwards.
void gamma_before_reset();
void gamma_after_reset();
