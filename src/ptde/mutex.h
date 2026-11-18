#pragma once

// FastMutexes: the game's mutex (DLKR::DLPlainMutex) without kernel calls.
//
// The original holds a Windows mutex (CreateMutexW in Create, 0x633320, the game's only one): every Lock (0x633390),
// TryLock (0x633400) and Unlock (0x633450) is a system call, even with nobody else waiting. In a Blighttown view
// (2026-11-17, 4K) the game thread took one about 217 times a frame at about 0.9 us a call each way: 0.39 ms of its
// 3.0 ms frame.
//
// Create gets a small lock of ours instead (its CreateMutexW call), kept where the mutex's handle was with bit 0 set
// (a kernel handle always has its low two bits clear), and Destroy and the destructor free it (their CloseHandle
// calls). Lock, TryLock and Unlock take it with one interlocked instruction: recursive for the thread that holds it,
// as a kernel mutex is; -4 for an Unlock by another thread (ReleaseMutex fails there); a short spin, then a sleep on
// the owner word (WaitOnAddress), never more than 5 ms at a time, for a thread that has to wait; finite timeouts
// return -3 as the original's do. A mutex holding a kernel handle (none, when installed as below) still goes to the
// kernel, unchanged. The game uses the handle nowhere else (every WaitForSingleObject, WaitForMultipleObjects and
// CloseHandle of the game checked, 2026-11-17).
//
// Installed from DllMain while this DLL is loaded as one of the game's own imports (static_load), before the game's
// entry point: no mutex exists yet. Under x86 translation (Box64, FEX, Windows on ARM) it is left out unless
// `even_translated` (FastMutexes = always), as FastReadLocks.
void mutex_install_early(bool static_load, bool even_translated = false);
// `wanted`: FastMutexes was asked for (the INI), for the log line when it was not tried.
void mutex_log(bool wanted);
