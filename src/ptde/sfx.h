#pragma once

// Particle spawn intervals. Effects (.ffx) give their emitters a spawn interval (pond3 type 2 at +0x10,
// type 3 at +0x0C, in seconds) that the game was authored for at 30 FPS: 1/60 s, or 0 for "every frame",
// meant one spawn per 30 FPS frame. At a higher frame rate those emitters spawn once per frame, so at
// 120 FPS smoke, sparks and embers come out four times as dense. Remastered fixed this in its data:
// every interval below 1/30 s was raised to exactly 1/30 s (3,329 uses of 1/60 and 200 of 0), and
// nothing else changed. This applies the same rule to every effect as the game loads it: a detour on
// fn 0xD19CF0 (stdcall(name, data, size), which registers an effect file with the FFX library before
// it is parsed) walks the raw file through its pointer-offset table and raises those intervals.
bool sfx_install(bool raise = true);

// Effect actions on the 30 FPS frame grid. When an effect node changes state the FX library runs that state's
// actions (FXBasicActionHandler, routines by id at 0x12DDC60), and they read their parameters at the node's clock
// [node+0x7C]. At 30 FPS a state change only ever happens on a frame boundary, so that clock reads a whole number
// of 1/30 s frames; effects were authored around that. The undead dragon's acid (16012): its emitter resets the
// clock on one state, moves on the next frame, and spawns its ground part with a count curve read at that clock:
// 1/30 s at 30 FPS, but 1/60 or 1/120 s at higher rates, where the count is 0 and the acid on the ground never
// appears. While an action runs, the clock is rounded up to the next 30 FPS frame and then put back (unless the
// action set it). At 30 FPS nothing changes.
bool sfx_actions_install();

// Heat haze (distortion particles, FXParticleAppearance_Distortion) on the 30 FPS frame grid. The haze scrolls its
// normal map across the particle by a phase the game integrates in real time: phase += speed(t) * (t - t_prev) at
// 0xB94BB0 (node clock now and at the previous update; speed 3 texture widths a second for Quelaag's lava). At 30 FPS
// that is a jump of a tenth of the texture each frame, which reads as a rolling wave; at 240 FPS the same motion is
// drawn eight times finer and reads as shaking. Here the step is taken between the two clock values rounded to the
// 30 FPS grid: at 30 FPS it is the same step, above it the pattern jumps 30 times a second as at 30 FPS. Size,
// colour and position still follow every frame.
bool sfx_distortion_install();
