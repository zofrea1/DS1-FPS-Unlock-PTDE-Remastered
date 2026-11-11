#pragma once

#include <cstdint>

// SteamPauseGuard (INI). Steam's Windows client checks Wi-Fi about every 5 s on the thread that answers games
// (WlanOpenHandle, an RPC to the WLAN AutoConfig service: 15 ms on a PC with an Intel AX200; measured from outside with
// tools/test_rendermath steamthread and wlantime). Every Steam call a game makes meanwhile waits for it (one thread's
// call waits on Steam's reply, every other thread's on the lock that call holds). The game calls Steam every frame -
// SteamAPI_RunCallbacks, and its matchmaking task's AddRequestLobbyListDistanceFilter (0xBE9330 / 0xBE93B0), with no
// search pending - and Prepare to Co-op reads its packets every frame, so at 200+ FPS a frame waits every 5 s; at the
// game's 30 FPS it fit inside the frame. Steam offline and Steam Input made no difference; the Steam Deck's client has
// no such pause.
//
// Learning the rhythm and predicting it did not hold up in the game (Steam also answered slowly at other moments). So
// instead a canary thread asks Steam the time every 10 ms, and the game's every-frame Steam calls go ahead only in the
// 1.5 ms after a quick answer (a pause would have to start inside that sliver to catch one: about 2% of pauses) or,
// when none came for 150 ms, whenever no canary question is stuck. SteamAPI_RunCallbacks then runs about 30 times a
// second (Steam keeps its callbacks meanwhile), Prepare to Co-op's packets go out and are read within ~10 ms. The
// matchmaking filter is kept here and handed to Steam just before the game's next lobby search, the one call that
// uses it.

// From the start-up thread. hook_game_imports: put the guard on the game's own SteamAPI_RunCallbacks and
// SteamMatchmaking imports (the FPS Unlock on its own; Prepare to Co-op hooks those imports itself and calls the
// functions below).
void steamguard_start(bool enabled, bool hook_game_imports);

// For a Steam call made every frame: true when it may go ahead now (and then *last is set to now). With the guard
// off it always may.
bool steamguard_frame_call(int64_t* last);
// True while a canary question is stuck: Steam is in a pause. For occasional calls that can use an older answer.
bool steamguard_paused();
// A Steam call made elsewhere and how long it took (any thread); slow ones are counted for the log.
void steamguard_note(double ms, int64_t end_qpc);

// The matchmaking interface's vtable copy (ISteamMatchmaking009, 38 slots, already in place on Steam's object): its
// AddRequestLobbyListDistanceFilter (slot 9) is replaced with one that only keeps the value.
void steamguard_take_distance_filter(void** vtable_copy);
// Just before RequestLobbyList on that object: hands Steam the distance filter kept since the last search.
void steamguard_before_lobby_request(void* matchmaking);
