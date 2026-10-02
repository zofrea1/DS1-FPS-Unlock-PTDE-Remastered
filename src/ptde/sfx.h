#pragma once

// Particle spawn intervals. Effects (.ffx) give their emitters a spawn interval (pond3 type 2 at +0x10,
// type 3 at +0x0C, in seconds) that the game was authored for at 30 FPS: 1/60 s, or 0 for "every frame",
// meant one spawn per 30 FPS frame. At a higher frame rate those emitters spawn once per frame, so at
// 120 FPS smoke, sparks and embers come out four times as dense. Remastered fixed this in its data:
// every interval below 1/30 s was raised to exactly 1/30 s (3,329 uses of 1/60 and 200 of 0), and
// nothing else changed. This applies the same rule to every effect as the game loads it: a detour on
// fn 0xD19CF0 (stdcall(name, data, size), which registers an effect file with the FFX library before
// it is parsed) walks the raw file through its pointer-offset table and raises those intervals.
bool sfx_install();
