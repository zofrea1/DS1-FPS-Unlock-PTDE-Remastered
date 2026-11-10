#pragma once

struct IDirect3DDevice9;

// Render thread CPU savings on the game's Direct3D 9 device (INI AsyncReadback, FilterSamplerStates).
//
//  * AsyncReadback: the game copies a render target into a texture through system memory once a frame (the draw
//    plan's resolve step: a new system-memory surface, GetRenderTargetData, LockRect, row copies; 0x590290). The
//    lock waits until the GPU has finished everything queued before it, 0.6 to 1.1 ms of the render thread at 4K
//    in the Firelink profile, and the GPU then idles while the render thread catches up. Each such source gets three
//    system-memory copies in rotation: GetRenderTargetData goes into the newest and the game's surface is filled from
//    the one requested two readbacks earlier, which the GPU has finished, so the copy is two frames old. A source that
//    is not read every frame (two readbacks more than 100 ms apart) is copied the original way.
//  * FilterSamplerStates: the game sets six sampler states for every texture it binds, about 17,000 calls a frame,
//    almost all repeating what is already set. A call that sets the value already in place is answered without
//    reaching Direct3D. The filter sits on the Direct3D device itself (below DSfix when DSfix wraps it), so every
//    caller's changes pass through it; state blocks, a device reset and another thread clear what it knows.
void d3dperf_hook_device(IDirect3DDevice9* game_device, bool readback, bool filter);
// Before IDirect3DDevice9::Reset: drops the readback copies and everything the filter knows.
void d3dperf_reset();

// For the profiler: sampler state calls answered by the filter, and readback copies that were not ready (the lock
// waited after all), since the start.
unsigned long d3dperf_filtered_samplers();
unsigned long d3dperf_readback_async();
unsigned long d3dperf_readback_waits();
