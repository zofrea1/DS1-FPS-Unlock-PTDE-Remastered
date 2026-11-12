#pragma once

// PreloadTextures: the game's textures are uploaded to the graphics card on the thread that loads them, right after
// D3DX makes them, instead of at their first draw on the render thread (where a map part's new textures stalled the
// frame). See texpreload.cpp.
void texpreload_install();
// The uploads since the last call, for the log (nothing when there were none).
void texpreload_report();
