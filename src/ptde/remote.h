#pragma once

#include <windows.h>

struct IDirect3DDevice9;

// Remote control for development (INI [Diagnostics] RemoteControl = true; diagnostics builds only, a release build
// compiles these as stubs). Lets a script drive and inspect the running game without touching the PC's keyboard or
// mouse.
//
// Commands are lines written to fpsu-cmd.txt beside DARKSOULS.exe; the file is read and deleted every 100 ms and
// every result is appended to fpsu-remote.txt there:
//   say TEXT                     echo TEXT (a marker for the script)
//   fps N                        frame rate cap N (0 = none), as MaxFPS
//   pad BUTTONS MS [LT RT]       hold buttons on controller slot 0 for MS ms (A B X Y START BACK LB RB LS RS UP DOWN
//                                LEFT RIGHT, joined with +; or none), the triggers 0-255
//   stick LX LY RX RY MS         hold the sticks (-32768..32767) for MS ms
//   pos                          the player's position
//   shot NAME                    the next picture as fpsu-shots\NAME.png
//   rtdump NAME                  every render target texture as fpsu-shots\NAME-T<n>.png
//   frame NAME                   one frame's Direct3D calls (targets, viewports, shaders, constants, draws, queries)
//                                as fpsu-shots\NAME.txt, and its picture as NAME.png
// Slot 0 always reads as a connected controller while this is on, with the real pad on slot 0 merged in. Pixel and
// vertex shaders are saved as the game creates them (fpsu-shaders\ps_<hash>.txt, disassembled) so a frame log's
// shader names can be read.
bool remote_start();
bool remote_enabled();

// From CreateDevice, after the other hooks and before the profiler's.
void remote_hook_device(IDirect3DDevice9* device);

// From the XInput read: merges the injected pad into slot 0. `result` is the real call's return value.
void remote_pad(DWORD index, void* state, DWORD* result);

// From the game thread at the start of each update: runs the commands that touch game state
// (flag ID, setflag ID 0|1, tp X Y Z, warp AREA BLOCK POINT).
void remote_game_frame();
