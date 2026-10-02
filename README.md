# DS1 FPS Unlock: PTDE and Remastered

High-FPS unlock with gameplay physics fixes for **Dark Souls: Remastered** and **Dark Souls: Prepare to Die Edition**. Movement, physics, camera, animation and menus stay correct at any frame rate, not just faster. Both games run in real time at 120, 240 or whatever your PC holds, and keep real-time speed when the frame rate dips.

The mods are proxy DLLs. The game's files on disk are never modified, and each mod has an INI with a switch for every fix so you can compare against the retail behaviour.

| | Remastered | Prepare to Die Edition |
| --- | --- | --- |
| Native frame rate | 60 | 30 |
| Release | `v1.2.0-REMASTERED-DX11` | `v1.2.0-PTDE-DX9` |
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
| Ladders | Sliding down dropped you through the floor | `FixMoveDt`, `FixStepDown` | `FixStepDown` |
| Ledges and lips | The ground snap-down glued you to a curved lip and dragged you down several times faster | `FixStepDown` | `FixStepDown` |
| Lock-on camera | Switching targets, locking on, or locking onto an enemy near the screen edge snapped or finished early instead of panning | `FixCamera` | `FixCamera` |
| Lock-on body turn | The torso, head and hips (and enemies' head and upper body when they track you) swung round several times too fast | `FixLockOnTurn` | `FixLockOnTurn` |
| Ghost replays | Bloodstain and wandering ghost replays ran several times too fast, and your own replay data was recorded too densely (it played back in slow motion for other players) | `FixGhosts` | `FixGhosts` |
| Particle effects | Emitters that spawn once per frame (smoke, sparks, embers, magic) came out several times too dense | n/a (Remastered's effect data already has the fix) | `FixSfxSpawnRate` |
| Estus / health bar fill | The bar animation advanced a fixed amount per frame and finished in a few frames | `FixUI` | `FixUI` |
| Sprint stamina drain | Each 0.1 s drain tick was rounded up to a whole frame and the remainder thrown away, so the drain ran slow at frame rates that do not divide 0.1 s evenly (8.7 per second at 61 FPS instead of 10) | `FixStaminaTick` | `FixStaminaTick` |
| Loading screen | The bonfire swirl on the loading screen turned at the display rate | `FixUI` | `FixUI` |
| D-pad hold (down: back to the first quick item) | The hold is counted in frames (15), so it fired during an ordinary tap | `FixDpadHold` | n/a (the hold does not exist) |
| Bonfire softlock | After "Reverse hollowing" the character could stay seated with no menu open | not available | `BonfireUnstick` |
| Timers | Per-frame fades and countdowns ran too fast | (variable step) | `FixTimers` |
| Smoothing | A per-frame interpolation factor | (variable step) | `FixSmoothing` |
| Menus and mouse | Menu animation, menu input repeat and mouse look scaled to the real frame time | yes (`MenuInputFilter`) | n/a |
| Cloth | Havok step scaled to the real frame time | yes | n/a |
| Fullscreen refresh rate | The game only offers 59/60 Hz; ask for the display's real rate, or use borderless | n/a | `FullscreenRefreshRate`, `BorderlessFullscreen` |

The original game's 30 FPS physics were tuned for 30 frames per second; both mods are exact at the native rate and very close at every other rate. See [Fix details](#fix-details) for what each fix does.

**Accuracy.** This is very close to the game at its native rate, and more than good enough for casual and serious play. It is not a perfect 1:1 match: retail decides some of these things on a fixed frame grid, so a borderline slope or lip can behave slightly differently at 180 or 240 FPS. Speedrunners and anyone who needs stock-exact precision should not use this.

## Fix details

**Lock-on camera (`FixCamera`).** The follow camera smooths several things toward a target by a fixed fraction every frame, tuned for the native frame rate: the yaw and pitch, the point it looks at (0.4 sideways, 0.3 vertically while locked on), the camera distance, the pivot that follows your character, the settling back after you let go of the stick, the automatic turn toward your walking direction, stick smoothing and the blend between camera settings when locking on. Every one of them now takes the same real time at any frame rate. Earlier versions scaled only the first two, so a pan from target to target still finished noticeably early.

**Lock-on body turn (`FixLockOnTurn`).** A small bone controller bends the hips, spine and head toward an angle: your torso and head toward the locked-on target (including while strafing), your upper body when aiming a bow or crossbow, and an enemy's head and upper body while it tracks you. It moved each bone a fixed fraction of the way every frame (0.6 hips, 0.2 head, 0.1 spine), so at 120 FPS the bending happened four times faster than in the game. Weapons ride on the skeleton, so this is not only cosmetic: enemies' upper bodies tracked you more sharply than in the game. The fix restores the native timing. The whole-body turn (turn speed in degrees per second) was already correct and is unchanged.

**Remastered lock-on speed (`LockOnPtdeSpeed`).** Remastered kept the original game's per-frame camera and body-turn values but runs them at 60 FPS, so its lock-on pan and body turn are twice as fast as Prepare to Die Edition. By default the mod keeps Remastered's 60 FPS speed; `LockOnPtdeSpeed = true` uses the original game's speed for both (that includes enemies' head and upper-body tracking). The old name `CameraPtdeSpeed` still works.

**Ghost replays (`FixGhosts`).** Bloodstain replays and wandering ghosts step to the next recorded sample every 10 frames in Prepare to Die Edition and every 20 in Remastered, one sample per third of a second at the native rate. At 120 FPS they played several times too fast. The recorder that captures your own bloodstain and the ghost data sent to other players counts in frames too, so a high frame rate recorded it too densely and other players saw your ghosts in slow motion. Playback and recording now follow real time, matching the native rate exactly.

**Particle effects (`FixSfxSpawnRate`, Prepare to Die Edition).** Effect files give each emitter a spawn interval. Many use 1/60 s, or 0 for "every frame", which at the native 30 FPS meant one spawn per frame. At a higher frame rate those emitters spawn once per frame, so at 120 FPS smoke, sparks and embers came out up to four times as thick and bright. Remastered fixed this in its data when it moved to 60 FPS: every interval below 1/30 s became exactly 1/30 s (about 3,500 values) and nothing else changed. The mod applies the same rule to each effect as the game loads it, so emitters spawn as densely as at the native rate. Remastered needs nothing: its effect data already has the fix.

**Sprint graze (`FixGraze`).** The game slows a character that barely moved last frame (it assumes a wall). The test is per frame, so at a high frame rate ordinary running failed it. The fix applies the game's own test to real speed with no slack, and the slowdown and recovery per second match the native rate exactly. Earlier Remastered builds gave it some slack, which let a character climb a small step after a few seconds of jitter, which the game never allows.

**Ground snap and ladders (`FixStepDown`).** Every physics step the game lifts the character by its step height, moves it, then pulls it back down onto the ground, at most a fixed distance per step. At a high frame rate the same pull is applied many more times per second, which glued the character to curved lips and pulled it through the floor at the bottom of a ladder slide. The pull is now limited to the native rate per second. (Prepare to Die Edition called this `FixLadder`; that name still works.)

**Sprint stamina drain (`FixStaminaTick`).** One point every 0.1 s, but the game rounded each tick up to a whole frame and dropped the remainder. The remainder is kept, so the drain is 10 points a second at any frame rate.

## Supported versions

Only the current Steam builds are supported. Anything else is unsupported and may refuse to patch (the log says why) or misbehave.

- **Remastered:** Steam build, SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0` (1.03.1, 2022-10-11).
- **PTDE:** Steam build, `SizeOfImage` `0x11C2000`.

Back up your saves. **Remastered is well tested with Seamless Co-op; PTDE is well tested with DSfix.** Other multiplayer (the official servers) has not been tested, so play offline there.

**Online play: no guarantees, use at your own risk.** The mods change the game's code in memory while it runs. Some fixes also change data the game exchanges with other players (for example the ghost replay data), now matching the native frame rate. FromSoftware's anti-cheat could still treat any modification as cheating, and a ban (including a soft ban) is possible. The authors accept no responsibility for bans, lost progress or anything else that results from using these mods; see also the [LICENSE](LICENSE) (provided "as is", without warranty).

## Recommended display mode

- **Remastered: borderless fullscreen** gives the best compatibility with monitors, frame pacing and VRR.
- **PTDE: exclusive fullscreen** gives the best compatibility with monitors, frame pacing and VRR (with `FullscreenRefreshRate = 0` to use your display's real refresh rate).

## Install

Copy the two files from the release for your game next to the game's exe (see the table above). To uninstall, delete them. `FPSUnlock` is the master switch: `false` turns every part of the mod off and leaves the game unchanged.

**Remastered.** The game imports `dinput8.dll`, so that file name has to stay. Another mod that also installs `dinput8.dll` needs a chain loader before the two can load together. In `System -> PC Settings -> Display`, set Frequency to the monitor refresh rate and Vertical sync to off, then restart the game after changing `MaxFPS`.

**PTDE.** The mod loads as `xinput1_3.dll` so it can sit beside DSfix. In exclusive fullscreen it can ask for your display's highest refresh rate instead of the 60 Hz the game offers (`FullscreenRefreshRate`), and it has an optional borderless mode (`BorderlessFullscreen`; set the game to windowed first).

`MaxFPS` defaults to 120 in both and accepts 10 to 1000; a value outside that range is clamped to the nearest end. Pick what your PC can usually hold; dips below it do not slow the game. A low cap works too: both games run in real time at any frame rate down to 10 FPS, so a slower PC can cap at 30 for steady frame pacing. (Frames longer than 1/8 s, below 8 FPS, are each treated as 1/8 s, so a hitch or a loading stall does not become one huge step.)

## Notes and known limits

- **Remastered lock-on speed.** Remastered pans the lock-on camera and turns the body toward the target twice as fast as the original (it kept the original game's per-frame values at 60 FPS). `LockOnPtdeSpeed = true` in the Remastered INI uses the original speed for both instead (the old name `CameraPtdeSpeed` still works).
- **Short steps after riding the edge (PTDE).** Walking along a short step at a sharp angle can leave the character riding its edge. At 30 FPS turning into the step then crosses it; at a high frame rate the character can get stuck against it like a low wall. The game decides step-ups once per physics step, and stepping the physics every frame changes the outcome on this borderline case. Running the player's physics at a fixed 30 steps a second does fix it, but needs interpolation that adds visible delay, so it is not included.
- **PTDE cutscenes.** In-engine cutscenes can still look like 30 FPS at a high frame rate (their content is stepped at 30 Hz); the pre-rendered movies are 30 FPS by nature.
- **First walk-to-run (Remastered).** The retail game has a short hitch on the first walk-to-run after standing still. At a high frame rate it reads as a brief freeze of the character and camera; it is left as it is.
- **Bonfire softlock (Remastered).** A few users have reported a softlock while resting at the Firelink bonfire in Remastered. PTDE has a watchdog for the same symptom (`BonfireUnstick`); Remastered does not, because its code is protected and the cause is not known. It has not been reproduced. If it happens, try `MenuInputFilter = false` and send the log.
- **Very high frame rates.** 240 FPS is not sustainable on every PC. Below the cap the game still plays in real time.
- **Very low frame rates.** Down to 10 FPS the game keeps real time, but each physics step is long (six times a 60 FPS step at 10 FPS). The game pulls a character down onto the ground by at most a fixed distance per step, so around 10 to 15 FPS a character sprinting down steep stairs or slopes can briefly leave the ground. This is the same rule that makes the original 30 FPS game slightly looser than Remastered's 60.
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

## Acknowledgements

- `BonfireUnstick` (PTDE): the idea, the memory addresses and the bonfire animation ids come from [FPSFix+](https://github.com/SeanPesce/FPSFix-Plus) by Sean Pesce, itself a remake of NullBy7e's FPSFix, both for this same bug. FPSFix+ is GPL-3.0; the code here is written from scratch and no code was copied.

## License

MIT. See [LICENSE](LICENSE).
