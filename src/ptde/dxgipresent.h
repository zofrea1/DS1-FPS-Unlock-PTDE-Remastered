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

// How frames went out since the last call (Profile / diagnostics), or nothing when unused: the present calls, how
// far apart the flips were, and on NVIDIA whether the display is following the frames (G-SYNC).
void dxgipresent_report();

// Timed presenting (INI DxvkPacedPresent, default true): a presenting thread of ours flips each frame a steady delay
// after the game's frame boundary for it, so the flips are spaced like the game's time steps (dxgipresent.cpp has the
// rule). mode 0 = off (Direct3D 11 flips each frame when the GPU is done), 1 = timed, 2 = the presenting thread flips
// each frame as soon as it is ready (for measuring). cap_fps gives the current cap (0 = none); null keeps the last.
void dxgipresent_set_pacing(int mode, int (*cap_fps)());
int dxgipresent_pacing();
// The game presents a picture (its draw thread, before Direct3D 9 sees the Present): boundary_qpc is the frame
// boundary it belongs to (frame_boundary_qpc), the time the flip is timed from.
void dxgipresent_frame_presented(long long boundary_qpc);
// Whether a Vulkan device of DXVK's presents through DXGI.
bool dxgipresent_in_use();
// DxvkHdrSwapChain: on a display Windows runs in HDR, an FP16 scRGB swap chain with the SDR picture converted the way
// Windows composes it, so the frames can be flipped straight to the display (0 off, 1 on; RemoteControl `dxgihdr`).
void dxgipresent_set_hdr_chain(int on);
int dxgipresent_hdr_chain();
// Development (scanwatch.h): told the time of each flip as it is asked for; a mark in the top right corner for
// `seconds` showing the presenting mode (green timed, red the GPU when done, yellow when ready) and the swap chain
// (blue scRGB, grey 8-bit).
// Development (RemoteControl dxgiprecopy): timed presenting puts each frame into the back buffer as soon as the flip
// before it is done and flips it when that copy is finished (1, the default), or copies it just before its flip (0).
void dxgipresent_set_precopy(int on);
void dxgipresent_set_flip_observer(void (*observer)(long long qpc));
void dxgipresent_show_mark(double seconds);
// Development: pace (and count close flips) as if the display refreshed at hz; 0 = the real display again.
void dxgipresent_test_refresh(int hz);

// How Windows showed the last sampled frame (DXGI frame statistics): composed, overlay, flipped straight to the display.
const char* dxgipresent_composition();
