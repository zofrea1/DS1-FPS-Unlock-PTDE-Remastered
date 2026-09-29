#pragma once

#include <cstdint>

// Opt-in movement tracer (INI: Trace=1). Opens a per-launch
// DSR-FPS-Unlock-trace-<pid>.csv in %TEMP% and hooks the three ChrIns::Update
// call sites so every character update logs position, physics position,
// animation and move-control state before and after the call.
bool trace_start(const wchar_t* dll_path);
bool trace_active();

// Flushes and closes the CSV. Called from DllMain on process detach so the
// tail of the capture survives an orderly game exit.
void trace_stop();

// Called from the generated cave. Signature position matches the game's own
// ChrIns::Update(chr, dt): rcx, xmm1, then rdx, r8.
void trace_log(void* chr, float dt, uint64_t site, int phase);
