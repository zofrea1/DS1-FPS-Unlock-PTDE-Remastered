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
bool dxvk_route(const wchar_t* dxvk_path);

// DXVK's 32-bit d3d9.dll, renamed, in the folder of this DLL.
inline constexpr wchar_t kDxvkFileName[] = L"dxvk_d3d9.dll";
