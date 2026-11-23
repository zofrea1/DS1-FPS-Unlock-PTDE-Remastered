#pragma once

// CacheInputReads: the game reads the keyboard and the mouse four times a frame, once for each of its four input
// users (DLUID::VirtualMultiDevice: each has its own DirectInput keyboard and mouse). On Windows a read is a quick
// copy. Under Wine (phones with GameNative or Winlator, Linux, the Steam Deck) every IDirectInputDevice8::
// GetDeviceState also has Wine look for new input events through its server (check_dinput_events): on a Snapdragon 888
// phone about 0.3 ms each, 2.5 ms of every frame for the eight reads. With the cache, the game thread's first
// successful read of a kind (mouse 20 bytes, keyboard 256 bytes) in a frame goes to DirectInput and the others in the
// same frame get the same bytes; a failed read is never cached (the game's re-acquire path runs as before).
//
// mode: 0 off, 1 on, 2 auto (on under Wine).
void inputcache_configure(int mode);
// Game thread, at the start of each frame (before the frame function): the frame number, and the hook once the
// game's DirectInput devices exist.
void inputcache_game_frame();
// RemoteControl: on (1) / off (0) for comparisons; -1 only reads. Returns the state.
int inputcache_set(int on);
// For the log: reads answered from the cache and reads passed on, since the start.
void inputcache_stats(unsigned long* cached, unsigned long* passed);
// FPS Unlock Settings: the hook goes in whatever CacheInputReads says, and while blank the game reads no keyboard or
// mouse input.
void inputcache_require_hook();
void inputcache_set_blank(bool on);
