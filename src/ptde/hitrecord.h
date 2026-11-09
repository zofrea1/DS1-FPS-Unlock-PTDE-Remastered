#pragma once

// Bullet hit records on the original game's 30 FPS frame grid (INI FixBulletHitTiming; see effectlife.h).
//
// Every hit-record entry gets its time in fn 0xFD7CF0 (pool in esi, [esp+4] target id, [esp+8] the record's
// lifetime): `movss xmm0, [esp+0Ch] ; mov [eax], ebx ; movss [eax+4], xmm0 ; pop ebx ; ret 8` at 0xFD7D54. That
// tail jumps to a stub that stores the id, stores the adjusted time and returns the entry the same way.
bool hitrecord_install();
