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
// Every new shader's bytecode and disassembly written to fpsu-shaders\ (default on). Off where the disk is slow (an
// SD card under Android): RemoteShaderDumps = false.
void remote_set_shader_dumps(bool on);
// RemoteDeviceHooks (default true): false leaves the Direct3D device's methods unhooked (for speed measurements).
void remote_set_device_hooks(int mode);  // 0 none, 1 all, 2 Present only (screenshots: `present` in the INI)
bool remote_enabled();
// Commands of a module built together with this one (Prepare to Co-op's diagnostics): a line this file does not know
// goes to `fn`, which writes its answer to `reply` (at most `size` bytes) and returns true when it knew the command.
// Called on the remote control's poll thread (set flags there; act on the game thread).
using RemoteExtension = bool (*)(const char* line, char* reply, size_t size);
void remote_set_extension(RemoteExtension fn);

// The name a created shader was saved under (vs_/ps_<hash>), "none" or "unknown".
#include <string>
std::string remote_shader_name(void* shader);

// From CreateDevice, after the other hooks and before the profiler's.
void remote_hook_device(IDirect3DDevice9* device);

// From the XInput read: merges the injected pad into slot 0. `result` is the real call's return value.
void remote_pad(DWORD index, void* state, DWORD* result);

// From the game thread at the start of each update: runs the commands that touch game state
// (flag ID, setflag ID 0|1, tp X Y Z, warp AREA BLOCK POINT).
void remote_game_frame();
