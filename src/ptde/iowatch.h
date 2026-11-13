#pragma once

// Diagnostics (Profile with ProfileStreams): the game's file reads, timed per thread. The game's imports of
// CreateFileW/A, ReadFile, GetOverlappedResult and CloseHandle are wrapped; every 5 s an [io] line gives, per thread
// (game, render, the others by id), the reads, bytes and time spent, and each read or wait of 30 ms or more is logged
// with its file, offset and size. For loading and streaming hitches on slow storage (a phone's SD card): it says
// whether the threads the game waits for are waiting on the storage. Nothing in a release build.
void iowatch_install();
