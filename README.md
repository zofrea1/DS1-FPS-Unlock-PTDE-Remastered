# DS1 FPS Unlock: PTDE and Remastered

High-FPS unlock with gameplay physics fixes for **Dark Souls: Remastered** and **Dark Souls: Prepare to Die Edition**. Movement, physics, camera, animation and menus stay correct at any frame rate, not just faster. Both games run in real time at 120, 240 or whatever your PC holds, and keep real-time speed when the frame rate dips.

The mods are proxy DLLs. The game's files on disk are never modified, and each mod has an INI with a switch for every fix so you can compare against the retail behaviour.

| | Remastered | Prepare to Die Edition |
| --- | --- | --- |
| Native frame rate | 60 | 30 |
| Release | `v1.5.0-REMASTERED-DX11` | `v1.5.0-PTDE-DX9` |
| Files | `dinput8.dll`, `DSR-FPS-Unlock.ini` | `xinput1_3.dll`, `PTDE-FPS-Unlock.ini` (plus `dxvk_d3d9.dll` for the experimental `DXVK`) |
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
| Running jumps (low frame rate) | At about 30 FPS a running jump could come out about half as long | `FixJump` | n/a (native rate) |
| Sprint slowdown (graze) | Running was treated as "stuck on a wall" and slowed | `FixGraze` | `FixGraze` |
| Ladders | Sliding down dropped you through the floor | `FixMoveDt`, `FixStepDown` | `FixStepDown` |
| Ledges and lips | The ground snap-down glued you to a curved lip and dragged you down several times faster | `FixStepDown` | `FixStepDown` |
| Lock-on camera | Switching targets, locking on, or locking onto an enemy near the screen edge snapped or finished early instead of panning | `FixCamera` | `FixCamera` |
| Lock-on body turn | The torso, head and hips (and enemies' head and upper body when they track you) swung round several times too fast | `FixLockOnTurn` | `FixLockOnTurn` |
| Ghost replays | Bloodstain and wandering ghost replays ran several times too fast, your own replay data was recorded too densely (it played back in slow motion for other players), ghosts jerked between samples, and ghosts and other players' characters turned too far between updates and snapped back | `FixGhosts` | `FixGhosts` |
| Particle effects | Emitters that spawn once per frame (smoke, sparks, embers, magic) came out several times too dense | n/a (Remastered's effect data already has the fix) | `FixSfxSpawnRate` |
| Estus / health bar fill | The bar animation advanced a fixed amount per frame and finished in a few frames | `FixUI` | `FixUI` |
| Sprint stamina drain | Each 0.1 s drain tick was rounded up to a whole frame and the remainder thrown away, so the drain ran slow at frame rates that do not divide 0.1 s evenly (8.7 per second at 61 FPS instead of 10) | `FixStaminaTick` | `FixStaminaTick` |
| Loading screen | The bonfire swirl on the loading screen turned at the display rate | `FixUI` | `FixUI` |
| D-pad hold (down: back to the first quick item) | The hold is counted in frames (15), so it fired during an ordinary tap | `FixDpadHold` | n/a (the hold does not exist) |
| Bonfire softlock | After "Reverse hollowing" the character could stay seated with no menu open | not available | `BonfireUnstick` |
| Timers | Per-frame fades and countdowns ran too fast: weapon buff glow, ragdoll blend-in, the glow on dropped items | `FixTimers` | `FixTimers` |
| Smoothing | The follow camera's pivot and look-at point eased toward their targets by a fixed fraction per frame | `FixSmoothing` | `FixSmoothing` |
| Velocities | The powered ragdoll's pull and the Doppler pitch of moving sounds were worked out from one frame's movement | `FixVelocity` | `FixVelocity` |
| Menus and mouse | Menu animation, menu input repeat and mouse look scaled to the real frame time | yes (`MenuInputFilter`) | n/a |
| Cloth | Havok step scaled to the real frame time | yes | n/a |
| Fullscreen refresh rate | The game only offers 59/60 Hz; ask for the display's real rate, or use borderless | n/a | `FullscreenRefreshRate`, `BorderlessFullscreen` |
| Direct3D 9 on Vulkan (experimental) | Runs the game through DXVK instead of the Windows Direct3D 9 driver; may or may not help the frame rate, and can have color issues | n/a | `DXVK` (off by default) |
| Controller | The game only accepts four old XInput device ids, so a pad is missed or another connected device is used instead | n/a | `FixController` |
| 16:10 and other taller screens | The picture stays 16:9, so the extra height is black bars. The in-game list often has no 1280x800 | n/a | `FixAspect` |

The original game's 30 FPS physics were tuned for 30 frames per second; both mods are exact at the native rate and very close at every other rate. See [Fix details](#fix-details) for what each fix does.

**Accuracy.** This is very close to the game at its native rate, and more than good enough for casual and serious play. It is not a perfect 1:1 match: retail decides some of these things on a fixed frame grid, so a borderline slope or lip can behave slightly differently at 180 or 240 FPS. Speedrunners and anyone who needs stock-exact precision should not use this.

## Fix details

**Taller screens (`FixAspect`, Prepare to Die Edition).** The projection is locked to 16:9, so on a 16:10 display (the Steam Deck's 1280x800, or 1920x1200) the extra height is black bars and the picture is not stretched. The in-game list often stops at 1280x720, which is why DSfix's borderless mode cannot be pointed at 1280x800 from the menu. When the display is taller than 16:9 and the game asked for a 16:9 mode, the swap chain is sized to the display and the vertical field of view is opened so the horizontal view stays the same. A 16:9 display is left alone. Leave DSfix `presentWidth` and `presentHeight` at 0. On a Steam Deck whose game resolution is set to 1280x720, the mod asks for 1280x800; if that picture looks squashed, set the game resolution in Steam to 1280x800.

**Controller (`FixController`, Prepare to Die Edition).** The game decides a DirectInput device is an XInput pad only when its id is one of four values from Windows 7, one per player slot. A current pad does not have those ids, so the game reads it through DirectInput, which is unreliable, and whichever device was enumerated first wins. A second controller, a wheel or a virtual pad is enough to make the real one go dead. The mod still honours those four ids when the device behind one is actually connected. Otherwise it binds a gamepad, preferring the one being held, and a slot that drops out for a moment is kept for half a second before another connected gamepad is tried. Wheels, flight sticks and music controllers are not taken. On the Steam Deck the same DLL has to forward XInput to Wine's controller backend (`xinput1_4`); forwarding to `xinput1_3` calls this mod again and the deck's pad never appears. That forward happens whether or not `FixController` is on.

**Lock-on camera (`FixCamera`).** The follow camera smooths several things toward a target by a fixed fraction every frame, tuned for the native frame rate: the yaw and pitch, the point it looks at (0.4 sideways, 0.3 vertically while locked on), the camera distance, the pivot that follows your character, the settling back after you let go of the stick, the automatic turn toward your walking direction, stick smoothing and the blend between camera settings when locking on. Every one of them now takes the same real time at any frame rate. Earlier versions scaled only the first two, so a pan from target to target still finished noticeably early.

Turning the camera with the stick or mouse also raises how tightly it follows, for two seconds, then fades that boost out over one second. The timer is real time and keeps running after you lock on. Before 1.5.0 only the base weight was scaled, so during those seconds the camera followed several times too fast above the native rate. The boost is scaled the same way now.

**Lock-on body turn (`FixLockOnTurn`).** A small bone controller bends the hips, spine and head toward an angle: your torso and head toward the locked-on target (including while strafing), your upper body when aiming a bow or crossbow, and an enemy's head and upper body while it tracks you. It moved each bone a fixed fraction of the way every frame (0.6 hips, 0.2 head, 0.1 spine), so at 120 FPS the bending happened four times faster than in the game. Weapons ride on the skeleton, so this is not only cosmetic: enemies' upper bodies tracked you more sharply than in the game. The fix restores the native timing. The whole-body turn (turn speed in degrees per second) was already correct and is unchanged.

**Remastered lock-on speed (`LockOnPtdeSpeed`).** Remastered kept the original game's per-frame camera and body-turn values but runs them at 60 FPS, so its lock-on pan and body turn are twice as fast as Prepare to Die Edition. The INI included with 1.5.0 sets `LockOnPtdeSpeed = true`, which uses the original game's speed for both (that includes enemies' head and upper-body tracking). Set it to `false` for Remastered's own speed. The old name `CameraPtdeSpeed` still works. If the key is missing, the mod keeps Remastered's speed, as in 1.4.0 and earlier.

**Running jumps (`FixJump`, Remastered).** A running jump does not leave the ground. For the first frames it follows the jump animation, then it keeps the speed from the end of that take-off. At about 30 FPS the take-off is only a few frames, and one of them can have no forward motion. The jump then alternated between stopped and full speed and covered about half the distance, and the sprint-slowdown check cut it further. `FixJump` keeps the faster of those two speeds for the rest of the jump. The height of the arc is unchanged. At 60 FPS and above the two speeds already match, so those jumps are left as they are. Prepare to Die Edition's native rate is 30, and it does not have this key.

**Ghost replays (`FixGhosts`).** Bloodstain replays and wandering ghosts step to the next recorded sample every 10 frames in Prepare to Die Edition and every 20 in Remastered, one sample per third of a second at the native rate. At 120 FPS they played several times too fast. The recorder that captures your own bloodstain and the ghost data sent to other players counts in frames too, so a high frame rate recorded it too densely and other players saw your ghosts in slow motion. Playback and recording now follow real time, matching the native rate exactly.

Between samples a replay turns toward the next recorded facing by a stored amount every frame: a tenth of the turn in Prepare to Die Edition (ten frames per sample) and a twentieth in Remastered. Other players' characters do the same with each network update: a fifth (Prepare to Die Edition) or a tenth (Remastered) of the turn per frame, an update arriving every sixth of a second at the native rate. At a higher frame rate those amounts added up to several times the turn before the next update came, so ghosts and other players spun past their facing and snapped back. The amount is now scaled by the frame time, so each turn takes the time between updates, as at the native rate.

The same sample also stores a step toward the next recorded position. The game reads that step as a direction and a walk or run speed, and it also moves the body by a copy of it every frame. Above the native rate a ghost crossed the gap in the first few frames and then stood still until the next sample, while the walk or run animation kept playing. The copy is now scaled by the frame time, so the ghost moves evenly across the third of a second between samples. The direction and the walk or run speed still come from the unscaled step.

**Particle effects (`FixSfxSpawnRate`, Prepare to Die Edition).** Effect files give each emitter a spawn interval. Many use 1/60 s, or 0 for "every frame", which at the native 30 FPS meant one spawn per frame. At a higher frame rate those emitters spawn once per frame, so at 120 FPS smoke, sparks and embers came out up to four times as thick and bright. Remastered fixed this in its data when it moved to 60 FPS: every interval below 1/30 s became exactly 1/30 s (about 3,500 values) and nothing else changed. The mod applies the same rule to each effect as the game loads it, so emitters spawn as densely as at the native rate. Remastered needs nothing: its effect data already has the fix.

**Sprint graze (`FixGraze`).** The game slows a character that barely moved last frame (it assumes a wall). The test is per frame, so at a high frame rate ordinary running failed it. The fix applies the game's own test to real speed with no slack, and the slowdown and recovery per second match the native rate exactly. Earlier Remastered builds gave it some slack, which let a character climb a small step after a few seconds of jitter, which the game never allows.

**Ground snap and ladders (`FixStepDown`).** Every physics step the game lifts the character by its step height, moves it, then pulls it back down onto the ground, at most a fixed distance per step. At a high frame rate the same pull is applied many more times per second, which glued the character to curved lips and pulled it through the floor at the bottom of a ladder slide. The pull is now limited to the native rate per second. (Prepare to Die Edition called this `FixLadder`; that name still works.)

In Remastered, a ladder slide at about 30 FPS moved far enough in one frame to look like that lift had been skipped. The snap was then cut on every frame of the slide, and the character rose instead of sliding down. The lift is now read from the lifted position, so a real slide is left alone.

**Sprint stamina drain (`FixStaminaTick`).** One point every 0.1 s, but the game rounded each tick up to a whole frame and dropped the remainder. The remainder is kept, so the drain is 10 points a second at any frame rate.

**Timers, camera easing and velocities (`FixTimers`, `FixSmoothing`, `FixVelocity`).** Every place in both executables that loads a frame-like constant (30, 60, 1/30, 1/60) was checked, and the matching code of the two games was compared: FromSoftware changed such a constant from 30 to 60 in Remastered only where it counts per frame, which points out the per-frame ones. These were still tied to one fixed frame:

- `FixTimers`: the weapon buff glow fading in and out, the blend from animation into ragdoll, the glow on dropped items, and (Remastered) a load queue countdown. Each advanced 1/30 s (PTDE) or 1/60 s (Remastered) per frame.
- `FixSmoothing`: the follow camera's pivot and look-at point move a fixed fraction of the remaining gap each frame. They now cover the same fraction per second at any frame rate.
- `FixVelocity`: the powered ragdoll (which pulls a body toward a pose) and the speed of moving sounds, which FMOD uses for the Doppler pitch shift, were worked out as one frame's movement times 30 (PTDE) or 60 (Remastered). At 120 FPS the ragdoll pull and the pitch shift were a quarter (PTDE) or half (Remastered) as strong as intended. They now divide by the real frame time.

In PTDE the engine's 1/30 step is rewritten with the frame time every frame, so the few places that copy it once into a setting (the Havok world setup, an image filter, the input repeat intervals registered after a key rebind) now read a fixed 1/30 instead of whatever the frame time was at that moment.

**D-pad hold (`FixDpadHold`, Remastered).** Holding down on the D-pad returns to the first quick item after 15 frames, a quarter of a second at 60 FPS but an eighth at 120, so an ordinary tap could trigger it. The hold is timed in real time: a quarter of a second at any frame rate, including below 60 FPS.

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

Copy the files from the release for your game next to the game's exe (see the table above; PTDE's `dxvk_d3d9.dll` is only needed for the experimental `DXVK`). To uninstall, delete them. `FPSUnlock` is the master switch: `false` turns every part of the mod off and leaves the game unchanged.

**Remastered.** The game imports `dinput8.dll`, so that file name has to stay. Another mod that also installs `dinput8.dll` needs a chain loader before the two can load together. In `System -> PC Settings -> Display`, set Frequency to the monitor refresh rate and Vertical sync to off, then restart the game after changing `MaxFPS`.

**PTDE.** The mod loads as `xinput1_3.dll` so it can sit beside DSfix. In exclusive fullscreen it can ask for your display's highest refresh rate instead of the 60 Hz the game offers (`FullscreenRefreshRate`), and it has an optional borderless mode (`BorderlessFullscreen`; set the game to windowed first).

**PTDE: DXVK (experimental).** `DXVK = true` runs the game's Direct3D 9 on Vulkan through [DXVK](https://github.com/doitsujin/dxvk) (version 3.1.1, included as `dxvk_d3d9.dll`; keep it next to `xinput1_3.dll`). It is experimental and off by default. It may or may not improve the frame rate on your system: it helps most where the Windows Direct3D 9 driver is the bottleneck, and on a fast GPU with a fast driver the difference can be small. It can also have other issues, such as wrong colors, crushed blacks or an inaccurate gamma curve (seen with DXVK's own `d3d9.dll` installed by hand too, so it is not specific to this mod). DSfix keeps working on top of it. It needs a graphics driver with Vulkan 1.3. If DXVK cannot start, the game runs on Direct3D 9 as usual and the log says why. The first time an area loads, DXVK compiles its shaders, which can cause brief hitches; they are cached for later runs. DXVK writes its own log, `DARKSOULS_d3d9.log`, next to the game, and reads an optional `dxvk.conf` from there. If the game folder already has a `d3d9.dll` (DXVK or ReShade installed by hand), that one stays in charge and `DXVK` does nothing. With `DXVK = false` (the default) or `FPSUnlock = false`, DXVK is not loaded at all.

`MaxFPS` defaults to 120 in both and accepts 10 to 1000; a value outside that range is clamped to the nearest end. Pick what your PC can usually hold; dips below it do not slow the game. A low cap works too: both games run in real time at any frame rate down to 10 FPS, so a slower PC can cap at 30 for steady frame pacing. (In Remastered before 1.4.0 a cap below 60 broke the frame pacer, which then waited longer every frame; it now holds any cap exactly. Remastered also no longer drops a frame when a frame comes in a little late, which made a frame rate held below the cap, or a cap set outside the game, hitch.) (Frames longer than 1/8 s, below 8 FPS, are each treated as 1/8 s, so a hitch or a loading stall does not become one huge step.)

## Notes and known limits

- **Remastered lock-on speed.** Remastered pans the lock-on camera and turns the body toward the target twice as fast as the original (it kept the original game's per-frame values at 60 FPS). The INI included with 1.5.0 sets `LockOnPtdeSpeed = true`, which uses the original speed for both (the old name `CameraPtdeSpeed` still works). Set it to `false` for Remastered's own speed.
- **Short steps after riding the edge (PTDE).** Walking along a short step at a sharp angle can leave the character riding its edge. At 30 FPS turning into the step then crosses it; at a high frame rate the character can get stuck against it like a low wall. The game decides step-ups once per physics step, and stepping the physics every frame changes the outcome on this borderline case. Running the player's physics at a fixed 30 steps a second does fix it, but needs interpolation that adds visible delay, so it is not included.
- **PTDE cutscenes.** In-engine cutscenes can still look like 30 FPS at a high frame rate (their content is stepped at 30 Hz); the pre-rendered movies are 30 FPS by nature.
- **Camera speeding up (Remastered, reports with 1.2.0).** Players reported the free camera suddenly speeding up, especially when running at an angle to it. Remastered's camera already swings toward your running direction faster than the original game's (it kept the original per-frame values at 60 FPS), and in 1.2.0 the blend between the camera's follow settings (how tightly the pivot and the look-at point follow your character) still ran once per frame, so at a high frame rate those changes finished two to four times sooner. 1.3.0 fixed that blend (`FixSmoothing`). 1.5.0 also scales the extra follow that lasts for two seconds after you turn the camera with the stick or mouse (`FixCamera`); without it, that part still finished several times too fast above 60 FPS, including just after locking on. If the camera still misbehaves, send the log: it has a mouse line once a minute while the mouse moves. `LockOnPtdeSpeed = true` (on in the 1.5.0 INI) also makes the camera settle at the original game's slower speed.
- **First walk-to-run (Remastered).** The retail game has a short hitch on the first walk-to-run after standing still. At a high frame rate it reads as a brief freeze of the character and camera; it is left as it is.
- **Bonfire softlock (Remastered).** A few users have reported a softlock while resting at the Firelink bonfire in Remastered. PTDE has a watchdog for the same symptom (`BonfireUnstick`); Remastered does not, because its code is protected and the cause is not known. It has not been reproduced. If it happens, try `MenuInputFilter = false` and send the log.
- **Very high frame rates.** 240 FPS is not sustainable on every PC. Below the cap the game still plays in real time.
- **Very low frame rates.** Down to 10 FPS the game keeps real time, but each physics step is long (six times a 60 FPS step at 10 FPS). The game pulls a character down onto the ground by at most a fixed distance per step, so around 10 to 15 FPS a character sprinting down steep stairs or slopes can briefly leave the ground. This is the same rule that makes the original 30 FPS game slightly looser than Remastered's 60.
- **Weapon durability and hit windows.** Worth checking if you depend on exact timing.

## Logs and bug reports

Each mod writes a log next to its DLL (`DSR-FPS-Unlock.log` or `PTDE-FPS-Unlock.log`) with a heartbeat every 30 seconds. The first lines show the version, the exe check and each patch applied. With `DXVK = true` the PTDE log says whether DXVK started and on which GPU, and DXVK's own `DARKSOULS_d3d9.log` is in the game folder. If the game freezes, the log says when the simulation stopped advancing; a crash writes a `*-crash.dmp` next to the DLL and a `CRASH:` line to the log. Send both with a bug report.

The `[Diagnostics]` INI section has development tools (recordings, watchpoints). Leave them off.

## Build

Visual Studio 2022 (MSVC, `/MT`):

```
build.bat
```

builds both targets. `build_dsr.bat` and `build_ptde.bat` build one each. `build_ptde.bat` also downloads DXVK 3.1.1 from its GitHub release once (`tools\fetch_dxvk.ps1`, SHA-256 checked) and puts its 32-bit `d3d9.dll` in `build\ptde\dxvk_d3d9.dll`.

| Game | Output |
| --- | --- |
| Dark Souls Remastered (64-bit) | `build\dsr\dinput8.dll` |
| Dark Souls: Prepare to Die Edition (32-bit) | `build\ptde\xinput1_3.dll` |

## How it works

**Remastered.** The retail frame pacer counts frames as the performance counter times 60, measures how many have passed, and waits for the next one. The simulation, effect, Havok and menu steps are given a fixed 1/60 s each displayed frame. The mod gives the pacer's measurement and its wait one clock that runs at `MaxFPS`/60 of real time, so it paces at `MaxFPS`, and reports a frame that is only one to three frames late as on time instead of letting the game drop the next frame to catch up. It hands the steps the real time each frame took. The movement fixes scale the per-frame constants found in the executable.

**PTDE.** The game runs at a fixed 1/30 s step. The mod writes the measured frame time into that step every frame, caps the rate with a precise wait, and stops the render thread waiting two vertical blanks per frame (the change DSfix makes). The same physics constants were found in the PTDE executable by the same method, and are scaled with the frame time.

Both mods check the bytes they patch before writing, so on an unsupported build they refuse rather than corrupt anything.

**PTDE with DXVK.** The game and DSfix both bind to the Windows `d3d9.dll` before the mod loads, and DSfix hooks its `Direct3DCreate9`. When the game first calls it, the mod loads `dxvk_d3d9.dll`, has DXVK create its Direct3D 9 object once on a separate thread (DXVK ends the process if it cannot start, so this test runs where a failure only ends that thread), and only then makes the Windows `Direct3DCreate9` jump to DXVK just past its first instruction bytes. Whatever hooks that function, DSfix included, then wraps DXVK's objects instead of the Windows ones.

## Acknowledgements

- `BonfireUnstick` (PTDE): the idea, the memory addresses and the bonfire animation ids come from [FPSFix+](https://github.com/SeanPesce/FPSFix-Plus) by Sean Pesce, itself a remake of NullBy7e's FPSFix, both for this same bug. FPSFix+ is GPL-3.0; the code here is written from scratch and no code was copied.

- `DXVK` (PTDE): [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors, included unmodified (`x32/d3d9.dll` from the v3.1.1 release, renamed `dxvk_d3d9.dll`) under the zlib/libpng license; see `DXVK-LICENSE.txt` in the PTDE release or [third_party/dxvk/LICENSE](third_party/dxvk/LICENSE).

## License

MIT. See [LICENSE](LICENSE).
