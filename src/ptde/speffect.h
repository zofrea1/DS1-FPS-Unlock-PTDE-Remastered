#pragma once

// SpEffect lifetimes on the original game's 30 FPS frame grid (INI FixSpEffectTiming; see effectlife.h).
//
// The two places that store a node's remaining time (endurance * time scale), PTDE fn 0xDBE5A0 (new node, at
// 0xDBE5E6, node in ebx) and fn 0xDBE520 (refreshed node, at 0xDBE560, node in esi), each `comiss xmm1, xmm0 ;
// movss [node], xmm0`, call a stub that stores the adjusted time and then does the same comiss with the game's
// value, so the instant-effect test after it is unchanged.
bool speffect_install();
