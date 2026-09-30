# DS1 Remastered FPS Unlock

Unlocks the framerate in Dark Souls Remastered and keeps gameplay, particles, cloth, cinematics, and menus on the same clock as the displayed frames.

The retail game is capped at 60. Its frame pacer multiplies the performance counter by 60, and the simulation, effect, Havok, and menu steps are still given a fixed 1/60 second on every displayed frame. This mod raises the pacer to `TargetFPS` (now a frame cap) and hands those steps the real time each frame took.

Supported executable: Steam build SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0` (1.03.1, 2022-10-11). The file on disk is not modified.

## Install

Copy `dinput8.dll` and `DSR-FPS-Unlock.ini` next to `DarkSoulsRemastered.exe`. The game imports `dinput8.dll`, so that filename has to stay. Controller and keyboard calls go to the system DirectInput. Another mod that also installs `dinput8.dll` needs a chain loader before the two can load together.

In `System -> PC Settings -> Display`, set Frequency to the monitor refresh and set Vertical sync to off. `TargetFPS` is a cap: the game never runs faster than that. If the PC cannot hold it, the picture runs lower but the game keeps real-time speed, because every step is the measured frame time (`VariableFrameTime = true`, the default). Earlier releases used a fixed `1 / TargetFPS` step and slowed down with the frame rate; setting `VariableFrameTime = false` brings that behavior back.

Play offline while this is in use, and back up saves. Multiplayer has not been tested.

`FPSUnlock = false` leaves the game unchanged.

## What to watch

The first log lines in `DSR-FPS-Unlock.log` should include the simulation step. The incoming value on that build is `0.016666668` (one sixtieth of a second). The corrected value is the frame time, about `1 / TargetFPS` while the frame rate is held.

The log also shows a heartbeat every 30 seconds (steps per second, frame time). If the game freezes, the log says when the simulation stopped advancing. A crash writes `DSR-FPS-Unlock-crash.dmp` next to the DLL and a `CRASH:` line to the log; send both with a bug report.

Movement, rolls, and animation time follow that step. The sprint-slowdown check is the same idea as Dark Souls III: it treats a short step as "stuck on geometry" and multiplies sprint speed by 0.8. Retail compares the step against 1/60 of a unit. The mod compares it against the real frame time, and stretches the 0.8 slowdown and 1.2 recovery so they still take the same amount of real time. Above 90 FPS the distance test has 20% of slack and recovery compounds from 1.5 instead of 1.2, so leaving a wall does not sit in the slow sprint.

The first walk-to-run after you have been standing still has a short hitch in the original game. At a high frame rate that hitch reads as a brief freeze of the character and the camera together. Toggling a weapon once warms it, and it stays gone until you stop moving completely. This release leaves that behavior as it is.

## Physics

Several movement systems in the retail game are counted once per displayed frame instead of per second, so they misbehave above 60 FPS. This release corrects the ones that were found:

- **Airborne momentum.** Jump distance and walk-off momentum decayed several times too fast. The per-frame damping is now raised to `60 × frame time`.
- **Slope slide.** The slide gravity and its air friction were per frame. They are scaled the same way.
- **Ladders.** The slide-down used a fixed 1/60 step, and at the bottom the ground snap pulled the character through the floor. Both are fixed, so sliding down a ladder no longer drops you out of the world.
- **Ledges and lips.** The ground snap-down could keep the character glued to a curved lip and slide it down several times faster than at 60. It now follows retail's limit of 0.4 units of drop per 1/60 s, so a single step is still caught in one frame and a lip is released at the retail rate.

`FixMoveDt` and `FixStepDown` in the INI switch the last two on and off for comparison.

- **Lock-on camera.** Switching targets, or locking onto an enemy near the edge of the screen, used to snap at a high frame rate because the camera's smoothing is applied once per frame. `FixCamera` (on by default) makes the pan take the same time at any frame rate. The retail game keeps the weights it inherited from the 30 FPS version, so its pan at 60 FPS is twice as fast as the original's; `CameraPtdeSpeed = true` restores the original speed.

**Accuracy.** Physics is very close to the game running at 60 FPS, and more than good enough for casual and serious play. It is not a perfect 1:1 match: retail decides some of these things on a 1/60 s frame grid, so a borderline slope or lip can behave slightly differently at 180 or 240 FPS. Speedrunners and anyone who needs stock-exact precision should not use this.

Weapon durability and hit windows are still worth checking. Menu and HUD animations take the scaled menu step, so they keep the retail pace. Menu navigation and mouse look are scaled back toward the 60 FPS rate (mouse counts are divided by the time they were collected over, so a slow frame does not make the camera jump). Cloth uses the same scale on the separate Havok step. The sprint and movement constants are written a few seconds after startup, once the executable will keep a code edit.

`Trace` and `Watch` in the INI are diagnostics for development. Leave them off.

## Build

Visual Studio 2022:

```
build.bat
```

builds both targets. `build_dsr.bat` and `build_ptde.bat` build one each.

| Game | Output | Notes |
| --- | --- | --- |
| Dark Souls Remastered (64-bit) | `build\dsr\dinput8.dll` | |
| Dark Souls: Prepare to Die Edition (32-bit) | `build\ptde\xinput1_3.dll` | see below |

The PTDE build loads as `xinput1_3.dll`, not `dinput8.dll`, so it can sit next to DSfix, which uses `DINPUT8.dll`.

## Dark Souls: Prepare to Die Edition

Copy `xinput1_3.dll` and `PTDE-FPS-Unlock.ini` next to `DARKSOULS.exe` (Steam build, SizeOfImage `0x11C2000`). It works with or without DSfix; if you use DSfix, set `unlockFPS 0` in `DSfix.ini` so the two do not both write the game's timestep.

PTDE runs its update and render loop at a fixed 1/30 s. The mod writes the measured frame time into the game's 1/30 step constant every frame, so the game keeps real-time speed at any frame rate, caps the frame rate at `TargetFPS`, and stops the render thread from waiting two vertical blanks per frame (the same change DSfix makes). In exclusive fullscreen it can ask for the display's highest refresh rate instead of the 60 Hz the game offers, and it has an optional borderless fullscreen mode.

Movement was written for exactly 30 frames a second. The same physics fixes as the Remastered build apply, found in the PTDE executable by the same method: slope gravity and friction, airborne momentum, the sprint-slowdown check when grazing a wall or running at a high frame rate, the ground snap that pulled the character down through a ladder's floor and boosted walk-offs down sloped edges, and per-frame timers. Each can be switched off in the `[Physics]` section. They are scaled with the frame time, so they are exact at 30 FPS and close everywhere else; the same accuracy caveat as above applies.

Known limits: in-engine cutscenes can still look like 30 FPS at a high frame rate (the scene content is stepped at 30 Hz there); the pre-rendered movies are 30 FPS by nature. Lock-on camera speed has not been checked against the original.

The log (`PTDE-FPS-Unlock.log`) records a heartbeat every 30 seconds. A crash writes `PTDE-FPS-Unlock-crash.dmp` and a `CRASH:` line; send both with a bug report.

## License

MIT. See [LICENSE](LICENSE).
