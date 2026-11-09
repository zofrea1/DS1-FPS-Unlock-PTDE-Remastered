#pragma once

// Roll direction at any frame rate (INI FixRollDirection; see rollhistory.h).
//
// PTDE fn 0xE17600 (PadManipulator, ebx = manipulator, [ebp+0Ch] = frame time) writes this update's stick entry
// into its ring and then walks the ring at 0xE17709..0xE1783B. The walk is replaced by a jump to a stub that
// fills the walk's results the way the game's code leaves them: [esp+18h] weighted forward/back (also in xmm4),
// [esp+1Ch] weighted side, [esp+20h] this update's forward/back, [esp+40h] this update's side (also in xmm3),
// xmm0 = 0, and goes on at 0xE1783C.
bool roll_install();
