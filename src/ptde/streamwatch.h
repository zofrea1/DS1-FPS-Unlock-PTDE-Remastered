#pragma once

// Diagnostics (Profile): times the game thread's synchronous jobs (0x577640) and logs the slow ones by class, with the
// game's stack, and a summary every 10 s. Nothing in a release build.
void streamwatch_install();
