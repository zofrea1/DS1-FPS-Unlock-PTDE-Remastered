#pragma once

#include <d3d9.h>

// OcclusionCulling (see occl.cpp): the map pieces DrawMapPieces adds are skipped while something in front hides them
// completely, found with occlusion queries of their boxes against the scene's depth.
bool occl_install();
// 0 off, 1 the pieces DrawMapPieces adds (not shown from the player's own room), 2 every map piece.
void occl_set_mode(int mode);
int occl_mode();
void occl_set_vista_cap(int on);  // diagnostics: boxes over 150 m on a side left alone
void occl_set_show(int on);   // diagnostics
// Right after the device is made, before any hook: the device's own DrawPrimitiveUP (the test boxes go straight to it).
void occl_capture_device(IDirect3DDevice9* device);
// After the other hooks: the render target and depth surface tracking.
void occl_set_device(IDirect3DDevice9* device);
// Before the device's Reset: its queries are released.
void occl_reset();
// Game thread, at each frame's start: the table of the map pieces' models.
void occl_game_frame();
// Render thread, at each frame's start: results collected, the main camera chosen.
void occl_render_frame();
// RemoteControl `entdump`: the next frame's passes (camera, target, model draws) in the log.
void occl_request_dump(int max_lines);
