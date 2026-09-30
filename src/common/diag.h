#pragma once

#include <cstdint>

// Crash and stall diagnostics shared by both mods. Nothing here changes the game's behavior;
// it only makes a failure leave evidence in the mod's log.
//
//  * A vectored exception handler logs the first few access violations and other fatal-class
//    exceptions with the faulting module and offset. They are logged because some are handled
//    by the game itself, so a line in the log does not by itself mean a crash.
//  * An unhandled-exception filter logs the crash and writes a small minidump
//    (<dll folder>/<name>-crash.dmp) before the process goes down.
//  * A watchdog thread logs a heartbeat every 30 s and reports when the game stops advancing
//    (a softlock looks like "no simulation step for N s" instead of silence).
void diag_install(const wchar_t* dll_path, const wchar_t* dump_name);

// `steps` returns a counter that increases once per simulation/render step; `frame_ms`
// returns the most recent frame time in milliseconds (for the heartbeat line).
// `extra` (optional) appends mod-specific counters to the heartbeat line.
// `tick` (optional) runs once a second on the watchdog thread.
void diag_watchdog_start(uint64_t (*steps)(), float (*frame_ms)(), void (*extra)(char* out, unsigned size) = nullptr,
                         void (*tick)() = nullptr);
