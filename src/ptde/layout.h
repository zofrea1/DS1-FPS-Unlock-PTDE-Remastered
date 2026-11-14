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
// RemoteControl `uidump N` (diagnostics builds): the next N draws of the layout in the log, and the game's state.
void layout_dump(int draws);
// own_scissor: no render-resolution hooks in front, so the menus' clipping rectangles are mapped here.
void layout_hook_device(IDirect3DDevice9* dev, bool own_scissor);
// A clipping rectangle the menus set in 1280x720 units, mapped onto the frame buffer (frame_w x frame_h) as the layout
// is drawn. False when the rectangle is not one of those (the caller passes the game's on).
bool layout_map_scissor(IDirect3DDevice9* dev, const RECT* r, int frame_w, int frame_h, RECT* out);
// The game's four menu clipping calls (its 2D renderer's commands, 0xF4A270..0xF4A450) go through a function of ours
// that maps the rectangle before the device sees it, so the mapping does not depend on a hook of the device's
// SetScissorRect being on the path the game's call takes (on the Steam Deck no call of the game reached it). The
// rectangle is then passed on through the device as before, marked as mapped.
void layout_patch_scissor_sites();
// True while a rectangle mapped at the call site is on its way through the device (the SetScissorRect hooks pass it
// on unchanged).
bool layout_scissor_premapped();
