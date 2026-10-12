#pragma once

// Game scripts (Lua 5.0): garbage collection pacing and uncaught script errors.
//
//  * The enemy AI (AiLuaMan) and a second script state are collected in full on a countdown driven by the
//    frame step: 0.5 s (AI) and 1 s. At the native 30 FPS that was every 15 and 30 frames, and the scripts
//    make their garbage every frame, so at a high frame rate many times as much piled up between
//    collections. With FixScriptMemory every countdown takes at least 1/30 s a frame: a collection at least
//    every 15 / 30 frames as at 30 FPS, and still every 0.5 / 1 s below 30 FPS.
//  * A script error that nothing catches makes the game call exit(1) with no message. The mod logs it first
//    (a SCRIPT: line: which state, which error, how much script memory was in use).
//  * script_sample / script_describe add each state's script memory (lowest and highest since the last
//    heartbeat) to the heartbeat line.
bool script_install(bool fix_gc);

// Called on the render thread at every frame boundary.
void script_sample();

// Heartbeat text: "script memory MB (low-high) AI 3.10-4.20, other 0.50-0.60".
void script_describe(char* out, unsigned size);
