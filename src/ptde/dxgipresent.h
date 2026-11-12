#pragma once

#include <windows.h>

// DXVK presenting through DXGI (INI DxvkPresentThroughDxgi, default true with DXVK = true).
//
// DXVK draws the game with Vulkan and normally shows each frame through a Vulkan swap chain. On Windows that
// swap chain only gets variable refresh (G-SYNC / FreeSync) in Vulkan exclusive fullscreen, and exclusive
// fullscreen with an SDR swap chain makes Windows switch HDR off for as long as the game holds it (dxvk.h has the
// measurements). Direct3D 9 itself has no such problem: Windows turns its fullscreen into a DXGI flip-model swap
// chain ("fullscreen optimizations"), which keeps HDR on (Windows composes the SDR picture at the SDR brightness
// setting) and still flips straight to the display with variable refresh.
//
// So this gives DXVK a Vulkan swap chain of our own whose images are Direct3D 11 textures shared with Vulkan, and
// shows them through a DXGI flip-model swap chain on the game window:
//   - DXVK finds Vulkan through GetProcAddress(vulkan-1.dll, "vkGetInstanceProcAddr"); its import of
//     GetProcAddress is pointed here, so every Vulkan function it looks up passes through this file. The window
//     system functions (surface, swap chain, acquire, present) are answered here; everything else is Vulkan's.
//   - Swap chain images: Direct3D 11 textures (shared NT handles) imported into DXVK's Vulkan device as dedicated
//     memory. Two Direct3D 11 fences are imported as Vulkan timeline semaphores: "ready" (Vulkan has finished a
//     frame) and "release" (Direct3D 11 has finished copying it out).
//   - vkQueuePresentKHR (DXVK's submission thread): a submission waits for DXVK's frame, hands the image to the
//     outside (queue family ownership release) and signals "ready"; then Direct3D 11 waits for "ready" on the GPU,
//     copies the image into the DXGI back buffer, signals "release" and presents (sync interval 0 with tearing
//     allowed for DXVK's immediate mode, 1 for FIFO).
//   - vkAcquireNextImageKHR hands out the images in turn. Its semaphore is never signalled; instead the
//     submission that waits for it is changed to wait for "release" (Direct3D 11 done with that image) and to
//     take the image back first.
//   - Extensions this replacement does not provide (present id/wait, swap chain maintenance, exclusive
//     fullscreen, HDR metadata, NVIDIA low latency, incremental present, mutable swap chain formats, surface
//     capabilities 2) are left out of what DXVK sees, so it uses the plain swap chain path.
// When the graphics driver cannot share textures and fences between Direct3D 11 and Vulkan, the Vulkan
// device is created as usual and every window system call goes to Vulkan's own (logged once).
//
// Call right after DXVK's DLL is loaded, before it creates anything. Returns false (and logs) when DXVK's
// GetProcAddress import was not found.
bool dxgipresent_install(HMODULE dxvk);

// One line about how frames went out since the last call (Profile / diagnostics), or nothing when unused.
void dxgipresent_report();

// How Windows showed the last sampled frame (DXGI frame statistics): composed, overlay, flipped straight to the display.
const char* dxgipresent_composition();
