# DS1 FPS Unlock: PTDE and Remastered

High-FPS unlock with gameplay physics fixes for **Dark Souls: Remastered** and **Dark Souls: Prepare to Die Edition**. Movement, physics, camera, animation and menus stay correct at any frame rate, not just faster. Both games run in real time at 120, 240 or whatever your PC holds, and keep real-time speed when the frame rate dips.

The mods are proxy DLLs. The game's files on disk are never modified, and each mod has an INI with a switch for every fix so you can compare against the retail behaviour.

| | Remastered | Prepare to Die Edition |
| --- | --- | --- |
| Native frame rate | 60 | 30 |
| Release | `v1.1.0-REMASTERED-DX11` | `v1.1.0-PTDE-DX9` |
| Files | `dinput8.dll`, `DSR-FPS-Unlock.ini` | `xinput1_3.dll`, `PTDE-FPS-Unlock.ini` |
| Install next to | `DarkSoulsRemastered.exe` | `DARKSOULS.exe` |
| Architecture | 64-bit | 32-bit |
| Works with DSfix | n/a | yes (set `unlockFPS 0` in `DSfix.ini`) |

## What is fixed

Everything below is on by default and has an INI switch.

| Fix | What it cures at a high frame rate | Remastered | PTDE |
| --- | --- | :---: | :---: |
| Frame-rate unlock, variable frame time | The frame cap (`MaxFPS`) only limits the rate; the game's step is the measured frame time | yes | yes |
| Slope slide | Slide gravity and friction were counted per frame | `FixSlide` | `FixSlide` |
| Airborne momentum | Jumps and walk-offs lost momentum several times too fast | `FixDamping` | `FixDamping` |
| Sprint slowdown (graze) | Running was treated as "stuck on a wall" and slowed | `FixGraze` | `FixGraze` |
| Ladders | Sliding down dropped you through the floor | `FixMoveDt`, `FixStepDown` | `FixLadder` |
| Ledges and lips | The ground snap-down glued you to a curved lip and dragged you down several times faster | `FixStepDown` | `FixLadder` |
| Lock-on camera | Switching targets, or locking onto an enemy near the screen edge, snapped instead of panning | `FixCamera` | `FixCamera` |
| Timers | Per-frame fades and countdowns ran too fast | (variable step) | `FixTimers` |
| Smoothing | A per-frame interpolation factor | (variable step) | `FixSmoothing` |
| Menus and mouse | Menu animation, menu input repeat and mouse look scaled to the real frame time | yes (`MenuInputFilter`) | n/a |
| Cloth | Havok step scaled to the real frame time | yes | n/a |
| Fullscreen refresh rate | The game only offers 59/60 Hz; ask for the display's real rate, or use borderless | n/a | `FullscreenRefreshRate`, `BorderlessFullscreen` |

The original game's 30 FPS physics were tuned for 30 frames per second; both mods are exact at the native rate and very close at every other rate.

**Accuracy.** This is very close to the game at its native rate, and more than good enough for casual and serious play. It is not a perfect 1:1 match: retail decides some of these things on a fixed frame grid, so a borderline slope or lip can behave slightly differently at 180 or 240 FPS. Speedrunners and anyone who needs stock-exact precision should not use this.

## Supported versions

Only the current Steam builds are supported. Anything else is unsupported and may refuse to patch (the log says why) or misbehave.

- **Remastered:** Steam build, SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0` (1.03.1, 2022-10-11).
- **PTDE:** Steam build, `SizeOfImage` `0x11C2000`.

Play offline while this is in use, and back up your saves. Multiplayer has not been tested.

## Install

Copy the two files from the release for your game next to the game's exe (see the table above). To uninstall, delete them. `FPSUnlock = false` leaves the game unchanged.

**Remastered.** The game imports `dinput8.dll`, so that file name has to stay. Another mod that also installs `dinput8.dll` needs a chain loader before the two can load together. In `System -> PC Settings -> Display`, set Frequency to the monitor refresh rate and Vertical sync to off, then restart the game after changing `MaxFPS`.

**PTDE.** The mod loads as `xinput1_3.dll` so it can sit beside DSfix. In exclusive fullscreen it can ask for your display's highest refresh rate instead of the 60 Hz the game offers (`FullscreenRefreshRate`), and it has an optional borderless mode (`BorderlessFullscreen`; set the game to windowed first).

`MaxFPS` defaults to 120 in both. Pick what your PC can usually hold; dips below it do not slow the game.

## Notes and known limits

- **Remastered lock-on camera speed.** Remastered pans the lock-on camera twice as fast as the original. `CameraPtdeSpeed = true` in the Remastered INI pans at the original speed instead.
- **PTDE cutscenes.** In-engine cutscenes can still look like 30 FPS at a high frame rate (their content is stepped at 30 Hz); the pre-rendered movies are 30 FPS by nature.
- **Lock-on body turn.** The character's upper body and head turn toward a lock-on target on the animation clock and may not match the retail timing exactly at every frame rate.
- **First walk-to-run (Remastered).** The retail game has a short hitch on the first walk-to-run after standing still. At a high frame rate it reads as a brief freeze of the character and camera; it is left as it is.
- **Bonfire softlock.** A few users have reported a softlock while resting at the Firelink bonfire in Remastered. It has not been reproduced. If it happens, try `MenuInputFilter = false` and send the log.
- **Very high frame rates.** 240 FPS is not sustainable on every PC. Below the cap the game still plays in real time.
- **Weapon durability and hit windows.** Worth checking if you depend on exact timing.

## Logs and bug reports

Each mod writes a log next to its DLL (`DSR-FPS-Unlock.log` or `PTDE-FPS-Unlock.log`) with a heartbeat every 30 seconds. The first lines show the version, the exe check and each patch applied. If the game freezes, the log says when the simulation stopped advancing; a crash writes a `*-crash.dmp` next to the DLL and a `CRASH:` line to the log. Send both with a bug report.

The `[Diagnostics]` INI section has development tools (recordings, watchpoints). Leave them off.

## Build

Visual Studio 2022 (MSVC, `/MT`):

```
build.bat
```

builds both targets. `build_dsr.bat` and `build_ptde.bat` build one each.

| Game | Output |
| --- | --- |
| Dark Souls Remastered (64-bit) | `build\dsr\dinput8.dll` |
| Dark Souls: Prepare to Die Edition (32-bit) | `build\ptde\xinput1_3.dll` |

## How it works

**Remastered.** The retail frame pacer multiplies the performance counter by 60 and the simulation, effect, Havok and menu steps are given a fixed 1/60 s each displayed frame. The mod raises the pacer to `MaxFPS` and hands those steps the real time each frame took. The movement fixes scale the per-frame constants found in the executable.

**PTDE.** The game runs at a fixed 1/30 s step. The mod writes the measured frame time into that step every frame, caps the rate with a precise wait, and stops the render thread waiting two vertical blanks per frame (the change DSfix makes). The same physics constants were found in the PTDE executable by the same method, and are scaled with the frame time.

Both mods check the bytes they patch before writing, so on an unsupported build they refuse rather than corrupt anything.

## License

MIT. See [LICENSE](LICENSE).
