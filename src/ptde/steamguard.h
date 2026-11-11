#pragma once

#include <cstdint>

// SteamPauseGuard (INI). Steam's Windows client checks Wi-Fi about every 5 s on the thread that answers games
// (WlanOpenHandle, an RPC to the WLAN AutoConfig service: 15 ms on a PC with an Intel AX200; measured from outside with
// tools/test_rendermath steamthread and wlantime). Every Steam call a game makes meanwhile waits for it. The game calls
// SteamAPI_RunCallbacks every frame (Prepare to Co-op also reads its packets every frame), so at 200+ FPS that is a
// 15 ms hitch every 5 s; at the game's 30 FPS it fit inside the frame. Steam offline and Steam Input made no
// difference; the Steam Deck's client has no such pause.
//
// The guard learns that rhythm: a probe thread asks Steam the time every 10 ms until it has seen the pause twice at a
// steady interval (Steam calls timed elsewhere, steamguard_note, count too). Then it keeps the game thread's Steam
// calls out of each predicted pause: the gate closes shortly before the pause is due, and opens when the probe (now
// asking every millisecond) sees the pause end, or a margin after it was due. A closed gate skips the game's
// SteamAPI_RunCallbacks for those frames (Steam keeps the callbacks; the next call delivers them), about 70 ms every
// 5 s. Where Steam never pauses, the gate never closes and the probe soon asks only rarely.

// From the start-up thread. hook_run_callbacks: put the gate on the game's own SteamAPI_RunCallbacks import (the FPS
// Unlock on its own; Prepare to Co-op hooks that import itself and asks steamguard_closed()).
void steamguard_start(bool enabled, bool hook_run_callbacks);
// True while a Steam call would likely wait for Steam's pause: skip it or put it off.
bool steamguard_closed();
// A Steam call made elsewhere and how long it took (any thread); slow ones help find the pause.
void steamguard_note(double ms, int64_t end_qpc);
