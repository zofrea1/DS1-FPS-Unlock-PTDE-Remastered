#pragma once

// Optional DXVK: the game's Direct3D 9 runs on Vulkan through DXVK's d3d9.dll, shipped
// beside this DLL as dxvk_d3d9.dll (INI DXVK = true).
//
// The game and DSfix both bind to the system d3d9.dll before this mod runs, and DSfix
// detours that DLL's Direct3DCreate9 (Detours copies its 5-byte prologue into a
// trampoline that jumps back to entry+5). So instead of replacing the game's call, this
// rewrites the system function just after its prologue to jump to DXVK. Every path into
// the system Direct3DCreate9 then ends in DXVK: the plain call, DSfix's trampoline, or a
// hotpatch-style hook, and DSfix goes on wrapping the object it gets back. The function is
// found through d3d9.dll's export table, not GetProcAddress: with a compatibility mode set on
// the game, Windows' shim engine answers GetProcAddress with its own stub (which in turn calls
// the function).
//
// DXVK reports an unusable PC (no Vulkan 1.3 GPU with the features it needs) by letting
// a C++ exception escape Direct3DCreate9, which ends the process. So before anything is
// rerouted, DXVK creates its Direct3D 9 object once on a guarded thread; if that fails,
// the system Direct3D 9 is left alone and the game runs as usual.
//
// Call once, on the game's Direct3DCreate9 call, before forwarding it. Returns true when
// Direct3D 9 now goes to DXVK. Logs why not otherwise.
//
// exclusive_fullscreen (INI DxvkExclusiveFullscreen, DxvkFse below): DXVK's dxvk.allowFse,
// passed in DXVK_CONFIG before DXVK loads. Without it, Windows never gives the Vulkan swap
// chain exclusive fullscreen and G-SYNC / variable refresh stays off (DXVK's dxvk.conf:
// "Certain features, such as variable refresh rate or HDR, will not work without this
// setting"). With it, Windows switches the display out of HDR for as long as the game holds
// exclusive fullscreen (the Vulkan swap chain is SDR), so the monitor changes mode and the
// picture changes. Measured on an RTX 3090 and an AW3225QF at 240 Hz, HDR on, a probe at
// 75-150 fps (tools/test_rendermath: vrrprobe, scanmon reading the scan line):
//   system Direct3D 9, fullscreen:        refresh follows the frames, HDR stays on
//   DXVK, dxvk.allowFse:                  refresh follows the frames, HDR off meanwhile
//   DXVK, no allowFse, NVIDIA Vulkan present method auto: fixed 240 Hz, HDR on
//   DXVK, no allowFse, native or layered on DXGI: ~61 Hz whatever the frame rate, HDR on
// So auto allows it only while Windows HDR is off for the primary display. Since 2.3.0 the default is to present
// through DXGI instead (dxgipresent.h: DXVK draws, a DXGI flip-model swap chain shows the frames, like Direct3D 9's
// own fullscreen), which needs no exclusive fullscreen for variable refresh and keeps HDR on.
// A DXVK_CONFIG the player set that already names the option is left as it is.
enum DxvkFse {
    kDxvkFseAuto = 0,
    kDxvkFseOn = 1,
    kDxvkFseOff = 2,
};
// dxgi_present (INI DxvkPresentThroughDxgi): DXVK's frames go out through DXGI (dxgipresent.h); exclusive_fullscreen
// is then not used (no Vulkan swap chain to make exclusive).
bool dxvk_route(const wchar_t* dxvk_path, int exclusive_fullscreen, bool dxgi_present);

// Under Proton / Wine (the Steam Deck, Linux, GameNative), before the game's Direct3DCreate9: options for the DXVK
// that runs the game there (Proton's own), through DXVK_CONFIG. sample_shading (INI ProtonSampleShading):
// d3d9.forceSampleRateShading, for the black specks on grass and fine detail with the game's anti-aliasing on.
// Nothing on Windows. A DXVK older than the variable (GameNative's 1.10) ignores it.
void dxvk_proton_options(bool sample_shading);

// DXVK's 32-bit d3d9.dll, renamed, in the folder of this DLL.
inline constexpr wchar_t kDxvkFileName[] = L"dxvk_d3d9.dll";
