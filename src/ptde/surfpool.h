#pragma once

struct IDirect3DDevice9;

// ReuseSurfaces: the game creates five depth-stencil surfaces and one system-memory offscreen surface every frame (the
// draw plan's target surfaces, 0x590920 from HGSurfaceEntity 0xBAD310, and the readback's temporary) and releases them
// again. Cheap in Direct3D 9 (under 1% of the render thread), about 9% under DXVK, which allocates GPU memory for each.
// Surfaces created through the device the game holds are kept (one reference of our own) when the game releases them,
// and handed out again for a creation with the same size, format, multisampling and pool. A new surface has undefined
// contents; a reused one has the previous use's, which the game clears or overwrites like any new one.
void surfpool_hook_device(IDirect3DDevice9* game_device);
// Before IDirect3DDevice9::Reset: every kept surface is let go.
void surfpool_reset();
// For the profiler: creations answered from the pool and creations passed on, since the start.
void surfpool_stats(unsigned long* reused, unsigned long* created);
