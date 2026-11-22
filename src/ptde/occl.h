#pragma once

#include <d3d9.h>

// OcclusionCulling (see occl.cpp): the map pieces DrawMapPieces adds are skipped while something in front hides them
// completely, found with occlusion queries of their boxes against the scene's depth.
bool occl_install();
// 0 off, 1 the pieces DrawMapPieces adds (not shown from the player's own room), 2 every map piece.
void occl_set_mode(int mode);
int occl_mode();
// DrawMapPieces adjacent/all: a piece the player's room does not show is not drawn while its world box holds the
// player (another room's piece reaching into this one). Needs occl_install.
void occl_set_inside(bool on);
// DrawMapPieces adjacent/all: the pieces only it adds drawn with a small depth bias, so the game's own pieces win where
// both cover the same surfaces (no z-fighting): world = metres moved back at the piece's nearest point, max_bias = the
// most depth bias. A negative value keeps that parameter. Needs occl_install.
void occl_set_zbias(bool on, float world = -1.0f, float max_bias = -1.0f);
void occl_set_zprobe(int seed, float eps, int which);
// The depth levels' share of the distance per level through the viewport's depth range (0: the DEPTHBIAS way).
void occl_set_zvp(float c);
float occl_zvp();
void occl_set_fx(int mode);
void occl_set_objshadow(int mode);  // diagnostics: added objects out of the shadow maps (1) / other cameras (2)
// The map pieces of a mirrored pass that SkipUnusedReflection does not send are not drawn either (1, default) or are
// (0); 100 + N: the next N draws that read the mirrored scene in the log (diagnostics).
void occl_set_refl(int on);
// Diagnostics: the first draw of a frame that has the mirrored scene's texture bound (passcull.cpp).
void occl_note_reflection_read(unsigned stages);
// Render thread: one of OcclusionCulling's occlusion queries is counting a draw now (another may not begin).
bool occl_query_active();  // experiment: DrawMapPieces' added pieces left out of the water's reflection
void occl_watch(float x, float y, float z, float r, int frames);  // diagnostics: entwatch  // effect draws: 0 normal, 9 all left out (diagnostics)
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
