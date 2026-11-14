#pragma once

struct IDirect3DDevice9;

// SkipUnusedReflection: the small mirrored second scene the game draws for mirroring surfaces (256x180, every frame
// in some maps) is not drawn while nothing on screen reads it (passcull.cpp).
//   mode 0 = off, 1 = skip while unused, 2 = always skip (development: a mirror then shows a stale picture).
void passcull_set_mode(int mode);
int passcull_mode();
// After the other device hooks. Hooks SetRenderTarget, Clear, SetTexture and the four draws.
void passcull_hook_device(IDirect3DDevice9* dev);
// Before a device Reset and for a new device: our copy goes, everything is learned again.
void passcull_reset();
// Development: the kept copy of the last mirror scene that was drawn (render thread; null when there is none), and
// for how many frames in a row nothing has read the scene.
struct IDirect3DTexture9;
IDirect3DTexture9* passcull_copy();
int passcull_unused_frames();
// Passes skipped and drawn and draws that read a kept copy since the last call.
void passcull_take_stats(long* skipped, long* drawn, long* stale);
