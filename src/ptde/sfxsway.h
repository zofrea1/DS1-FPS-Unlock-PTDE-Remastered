#pragma once

// Diagnostic (with Trace = true): how often the effect library's two swaying particle movements run, once a
// second, for a frame rate check. FXClusterMovement_Yurayura (vtable 0x113041C) and FXClusterMovement_PartialFollow
// (0x113039C) turn every particle's velocity by a random angle (0xA50100) once their counter (+0x10) passes the
// node's limit (+0x40 of its data: 10 or 20 in effect 13401, the Blighttown spider offspring's magma tentacle), and
// a per-call function (0x9E6050, 0x9E50C0) adds one to that counter. PartialFollow also carries its particles along
// with the emitter's owner. If those per-call functions run once a frame, the sway turns 8x as often at 240 FPS.
// Logged: the per-call functions, the updates (0x9E6180, 0x9E5350) and the turns, each per class, and the highest
// counter an update saw.
bool sfxsway_install();
// Once a frame, on the game thread.
void sfxsway_tick();
