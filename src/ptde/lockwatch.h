#pragma once

// Diagnostics (Profile with ProfileStreams): the engine's DLPlainLightMutex (a critical section; Lock 0x567E70,
// TryLock 0x567EB0, Unlock 0x567ED0, shared by every instance) is replaced with timed versions. A thread that holds one
// for 40 ms or more is logged at its unlock with where it took it; a thread that waits 40 ms or more for one is logged
// with the thread that owned it. For the loading hitches where the game thread blocks on the resource manager's lock
// (DLResourceManagerImpl+0x1A8, taken at 0x632450 from the game update 0xBED7E0). Nothing in a release build.
void lockwatch_install();
