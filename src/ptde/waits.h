#pragma once

// Hooks the game's imports of Sleep, WaitForSingleObject and WaitForMultipleObjects
// (through its import table, not the DLLs). During the startup intro run it records,
// per call site, how often it was called and how long it waited, and can shorten the
// waits so the intro's per-frame pacing does not keep it running for ~11 seconds.
bool waits_install();

// Logs the per-call-site table recorded during the intro, busiest first.
void waits_report();
