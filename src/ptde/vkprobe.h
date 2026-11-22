#pragma once

#include <windows.h>

#include <cstddef>

// HighPrecisionOutput under Proton / Wine: the game's Direct3D 9 is Proton's own DXVK, which picks the Vulkan surface
// format that fits the back buffer from those the window's surface offers, and falls back to an 8-bit one, silently,
// when none fits. A temporary Vulkan instance and surface on the game window (the same Vulkan loader and layers as
// DXVK's, gamescope's included) tell beforehand whether a 10-bit format in the SDR colour space is offered.
// 1 offered, 0 not, -1 unknown (no Vulkan, or a call failed). `text` gets the formats for the log.
int vkprobe_10bit_surface(HWND window, char* text, size_t size);
