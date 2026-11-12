#pragma once

// FastReadLocks: the game's reader/writer lock (DLKR::DLPlainReadWriteLock) without kernel calls.
//
// The original keeps an auto-reset event that stands for "free" (+4), a writer mutex (+8), a reader mutex (+0xC) and
// the reader count (+0x10, -1 = no readers). Writers take the writer mutex and the event; the first reader takes the
// event for all readers, the last gives it back; every read lock and unlock also waits on the reader mutex. Each
// lock and unlock is two to six system calls. The game frees memory through such a lock (the heap list, 0x563160):
// as an area streams in and the last one is freed, tens of thousands of frees in one frame spent nearly 50 ms in
// those calls on the game thread.
//
// All of them are replaced: ReadLock (0x60D0F0), TryReadLock (0x60D1B0), ReadUnlock (0x60D220), WriteLock (0x60CFA0,
// also through the jump at 0x60D300), TryWriteLock (0x60D050, and 0x60D310) and WriteUnlock (0x60D0B0, and its copy
// 0x60D320). The whole lock lives in the count, changed with compare-and-swap; only a thread that has to wait sleeps
// (WaitOnAddress). The original's rules and return values (including its quirks) are kept, and so is its hand-off: a
// writer waiting for readers gets the lock before any new reader once they are gone (see rwlock.cpp). The event and
// the mutexes are no longer used. tools/test_rendermath/test_rwlock compares every call with the game's own code.
// Installed from DllMain while this DLL is loaded as one of the game's own imports (static_load: DllMain's reserved
// argument is not null), before the game's entry point runs: no thread can be inside the functions yet, so no
// call ever mixes the two ways of counting, and no thread has to be paused. Loaded any later (a chain loader, an
// injector), it is left out. Nothing is logged there; rwlock_log() says what happened once the log is open.
void rwlock_install_early(bool static_load);
// `wanted`: FastReadLocks was asked for (the INI), for the log line when it was not tried.
void rwlock_log(bool wanted);
