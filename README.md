# DS1 Remastered FPS Unlock

Unlocks the framerate in Dark Souls Remastered and keeps gameplay, particles, cloth, cinematics, and menus on the same clock as the displayed frames.

The retail game is capped at 60. Its frame pacer multiplies the performance counter by 60, and the simulation, effect, Havok, and menu steps are still given a fixed 1/60 second on every displayed frame. This mod raises that pace to `TargetFPS` and scales those steps by `60 / TargetFPS`.

Supported executable: Steam build SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0` (1.03.1, 2022-10-11). The file on disk is not modified.

## Install

Copy `dinput8.dll` and `DSR-FPS-Unlock.ini` next to `DarkSoulsRemastered.exe`. The game imports `dinput8.dll`, so that filename has to stay. Controller and keyboard calls go to the system DirectInput. Another mod that also installs `dinput8.dll` needs a chain loader before the two can load together.

In `System -> PC Settings -> Display`, set Frequency to the monitor refresh and set Vertical sync to off. `TargetFPS` is the pace the game aims for. A driver cap, vsync, or a PC that cannot hold that number will hold the picture lower, and the game will slow down with it. Pick a target the machine can keep.

Play offline while this is in use, and back up saves. Multiplayer has not been tested.

`FPSUnlock = false` leaves the game unchanged.

## What to watch

The first log lines in `DSR-FPS-Unlock.log` should include the simulation step. The incoming value on that build is `0.016666668` (one sixtieth of a second). The corrected value is `1 / TargetFPS`.

Movement, rolls, and animation time follow that step. The sprint-slowdown check is the same idea as Dark Souls III: it treats a short step as "stuck on geometry" and multiplies sprint speed by 0.8. Retail compares the step against 1/60 of a unit. The mod compares it against the real frame time, and stretches the 0.8 slowdown and 1.2 recovery so they still take the same amount of real time. Above 90 FPS the distance test has 20% of slack and recovery compounds from 1.5 instead of 1.2, so leaving a wall does not sit in the slow sprint.

The first walk-to-run after you have been standing still has a short hitch in the original game. At a high frame rate that hitch reads as a brief freeze of the character and the camera together. Toggling a weapon once warms it, and it stays gone until you stop moving completely. This release leaves that behavior as it is.

Some actions are still counted once per displayed frame. Sliding down a ladder, jump distance, weapon durability, and hit windows are the ones worth checking. Menu and HUD animations take the scaled menu step, so they keep the retail pace. Menu navigation and mouse look are scaled back toward the 60 FPS rate. Cloth uses the same scale on the separate Havok step. The sprint constants are written a few seconds after startup, once the executable will keep a code edit.

## Build

Visual Studio 2022:

```
build.bat
```

`build\dinput8.dll` is the proxy.

## License

MIT. See [LICENSE](LICENSE).
