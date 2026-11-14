#pragma once

#include <windows.h>

struct IDirect3DDevice9;

// The HUD's and the menus' 1280x720 layout on screens that are not 16:9, and its size (see layout.cpp).
//  - FixAspectUI: with FixAspect active the layout keeps its 16:9 shape in the middle of the picture instead of being
//    stretched to the screen's shape.
//  - UIScale: the HUD's corner groups drawn smaller (percent, 50..100).
void layout_configure(bool keep_shape, int scale_percent);
// RemoteControl `uiscale N`.
void layout_set_scale(int scale_percent);
// own_scissor: no render-resolution hooks in front, so the menus' clipping rectangles are mapped here.
void layout_hook_device(IDirect3DDevice9* dev, bool own_scissor);
// A clipping rectangle the menus set in 1280x720 units, mapped onto the frame buffer (frame_w x frame_h) as the layout
// is drawn. False when the rectangle is not one of those (the caller passes the game's on).
bool layout_map_scissor(IDirect3DDevice9* dev, const RECT* r, int frame_w, int frame_h, RECT* out);
