#pragma once

// Ghost replays (bloodstains, wandering ghosts) and the recorder that produces them count in frames.
//
//  * Playback: ReplayManipulator::Update (fn 0xE16500, esi = manipulator, [ebp+8] = dt) steps to the
//    next recorded sample every 10 frames ([esi+0x274] counts down, reloaded with 10). At 30 FPS that
//    is a sample every 1/3 s; at 120 FPS ghosts ran four times too fast.
//  * Recording: the player's PadManipulator (fn 0xE1DC60, ebx = manipulator) takes a replay sample
//    every 10 frames ([ebx+0x334]) and a second, network-side sample every 5 frames ([ebx+0x234]).
//    At a high frame rate the recorded data is too dense, so it plays back slowly for other players
//    (and for this one once playback is fixed).
//
// All three counters are rescaled to 1/65536 of a 30 FPS frame: the reloads add 10 (or 5) frames'
// worth instead of setting it, so no fraction is lost, and each frame subtracts dt * 30 * 65536.
// At exactly 30 FPS this is the original behaviour.
//
//  * Turning: replays and other players' characters (NetworkManipulator) turn by a stored amount every frame,
//    a tenth (replays) or a fifth (network samples) of the turn to the next sample, i.e. 10 or 5 frames at
//    30 FPS. That amount is scaled by dt * 30 so the turn takes the time between samples at any frame rate.
bool ghost_install();

// Called on the render thread at every frame boundary with the frame time in seconds.
void ghost_frame(double dt);
