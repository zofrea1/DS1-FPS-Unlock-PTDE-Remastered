#pragma once

// FastEffects: the original game's effects library (FromSoftware's FFX: particle emitters, billboards, cluster
// movement, parameter curves; 761 functions between 0x960000 and 0xBA6000) was linked as a debug build. Besides being
// unoptimized, every one of its functions fills its stack frame with 0xCC on entry and has _RTC_CheckStackVars check
// guard bytes around its arrays on exit (MSVC /RTC1). The render thread builds the particles' vertices with this
// code, so a view with many particles in it (fog, fire, the dragons' acid) is slow out of proportion to what is
// drawn. The fills are skipped (their count set to 0) and the guard check returns at once. Both only exist to catch
// programming errors; what the effects compute is unchanged.
//
// Install last: it scans for the exact debug prologue, so any hook written later into one of those bytes would no
// longer find what it expects.
bool ffx_install();
