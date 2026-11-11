#pragma once

// FastReadLocks: the reader side of the game's reader/writer lock (DLKR::DLPlainReadWriteLock) without kernel calls.
//
// The lock keeps an auto-reset event that stands for "free" (+4), a writer mutex (+8), a reader mutex (+0xC) and the
// reader count (+0x10, -1 = no readers). Writers take the writer mutex and the event; the first reader takes the event
// for all readers, the last gives it back. Every read lock and unlock waits on the reader mutex and releases it around
// a plain add on the count (two system calls each, more for the first and last reader): 3 to 6% of the render thread.
//
// ReadLock (0x60D0F0), TryReadLock (0x60D1B0) and ReadUnlock (0x60D220) are replaced: the count is changed with
// compare-and-swap instead of under the reader mutex, and while the first reader waits for the event the count holds a
// marker that makes the others wait for it (WaitOnAddress), as they waited on the mutex before. The event, the writers
// and the return values (including the original's quirks) stay as they were.
//
// Installed from DllMain while this DLL is loaded as one of the game's own imports (static_load: DllMain's reserved
// argument is not null), before the game's entry point runs: no thread can be inside the three functions yet, so no
// call ever mixes the two ways of counting, and no thread has to be paused. Loaded any later (a chain loader, an
// injector), it is left out. Nothing is logged there; rwlock_log() says what happened once the log is open.
void rwlock_install_early(bool static_load);
// `wanted`: FastReadLocks was asked for (the INI), for the log line when it was not tried.
void rwlock_log(bool wanted);
