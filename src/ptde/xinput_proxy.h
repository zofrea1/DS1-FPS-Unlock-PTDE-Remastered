#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// The game imports XINPUT1_3 by ordinal (2 = XInputGetState, 3 = XInputSetState).
// This DLL uses that name so it can sit beside DSfix, which occupies DINPUT8.dll.
// Exports are forwarded to a real XInput implementation. That implementation must
// not be this DLL: loading xinput1_3 by name returns the copy already mapped, and
// under Wine a native override (Steam Deck: WINEDLLOVERRIDES="xinput1_3=n,b")
// resolves the system copy to this DLL as well.

// Real XInputGetState. 0 on success. 1167 (ERROR_DEVICE_NOT_CONNECTED) when the
// slot is empty or no real library could be loaded. `state` is 16 bytes.
DWORD xinput_get_state(DWORD index, void* state);

// Real XInputGetCapabilities. Same return codes. `caps` is 20 bytes; byte 1 is the
// device subtype. 1167 when the loaded library has no such export.
DWORD xinput_get_caps(DWORD index, void* caps);

// Loads the real library if that has not happened yet, then writes its path to the
// log. The game can call XInput before the log file is open, so call this again
// after log_init.
void xinput_log_target();
