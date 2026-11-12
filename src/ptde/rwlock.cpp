#include "rwlock.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uintptr_t kImageBase = 0x400000;
constexpr uintptr_t kReadLock = 0x60D0F0;
constexpr uintptr_t kTryReadLock = 0x60D1B0;
constexpr uintptr_t kReadUnlock = 0x60D220;
constexpr uintptr_t kWriteLock = 0x60CFA0;      // and its jump thunk 0x60D300 (vtable)
constexpr uintptr_t kTryWriteLock = 0x60D050;   // and 0x60D310
constexpr uintptr_t kWriteUnlock = 0x60D0B0;
constexpr uintptr_t kWriteUnlock2 = 0x60D320;   // the vtable's own copy
constexpr uintptr_t kCodeLo = 0x60CFA0;  // the functions
constexpr uintptr_t kCodeHi = 0x60D360;

// The whole lock lives in its count (+0x10): -1 free (the constructor's value), n >= 0 for n + 1 readers, kWriter for
// a writer. Taking and giving it back is one interlocked instruction; only a thread that has to wait sleeps
// (WaitOnAddress on the count) and is woken when the count changes. The event and the two mutexes the originals used
// are no longer touched (the destructor still closes them). As in the original:
//  * readers keep joining while readers hold the lock, even with a writer waiting;
//  * a writer waiting for readers gets the lock as soon as the last of them leaves, before any new reader (the
//    original's event went to the writer waiting on it): it marks the count (kWaiting) while it waits, and the last
//    reader leaves the count at kReserved, which only a writer takes;
//  * a reader waits for a writer; the same thread taking the lock twice for writing, or for reading under its own
//    write, would wait forever (the original deadlocked on its event).
constexpr LONG kFree = -1;
constexpr LONG kWriter = 0x40000000;
constexpr LONG kWaiting = 0x20000000;          // on a reader count: a writer waits
constexpr LONG kReserved = kWaiting + kFree;   // the readers are gone and a writer waits: for writers only

inline bool is_readers(LONG c) { return c >= 0 && c != kReserved && c < kWriter; }  // with or without kWaiting

struct Lock {
    void* vtable;
    HANDLE event;         // signalled = free (no longer used)
    HANDLE writer;        // (no longer used)
    HANDLE reader_mutex;  // (no longer used)
    volatile LONG count;  // see above
};

// The game's assert reporter (file, line, message), called where the originals call it, with their strings.
using AssertFn = void(__cdecl*)(const char*, int, const char*);
const AssertFn game_assert = reinterpret_cast<AssertFn>(0x559E60);
inline void check(const Lock* lock, uintptr_t file, int line, uintptr_t message) {
    if (!lock->event) game_assert(reinterpret_cast<const char*>(file), line, reinterpret_cast<const char*>(message));
}

using WaitOnAddressFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressAllFn = void(WINAPI*)(PVOID);
WaitOnAddressFn g_wait_on_address = nullptr;
WakeByAddressAllFn g_wake_all = nullptr;
volatile LONG g_waiters = 0;  // threads asleep on any lock (wakes are only made while there are some)

// Sleeps until the count is no longer `seen` (or a spurious wake), unless `timeout` ms after `start` have passed.
bool wait_change(Lock* lock, LONG seen, DWORD timeout, ULONGLONG start) {
    DWORD left = INFINITE;
    if (timeout != INFINITE) {
        const ULONGLONG spent = GetTickCount64() - start;
        if (spent >= timeout) return false;
        left = static_cast<DWORD>(timeout - spent);
    }
    InterlockedIncrement(&g_waiters);
    g_wait_on_address(&lock->count, &seen, sizeof(seen), left);
    InterlockedDecrement(&g_waiters);
    return true;
}

void wake(Lock* lock) {
    if (g_waiters) g_wake_all(const_cast<LONG*>(&lock->count));
}

// thiscall (lock, timeout), pops 4: 0 = locked; 0xFFFFFFFD (-3) on a timeout, and also, like the original, for a reader
// that joined others on a finite timeout (it is counted all the same).
int __fastcall read_lock(Lock* lock, void*, DWORD timeout) {
    check(lock, 0x10DCFF0, 0x13F, 0x10DCFD4);
    if (timeout == 0) game_assert(reinterpret_cast<const char*>(0x10DD030), 0x140, reinterpret_cast<const char*>(0x10DD018));
    const ULONGLONG start = timeout == INFINITE ? 0 : GetTickCount64();
    for (;;) {
        const LONG c = lock->count;
        if (c != kFree && !is_readers(c)) {  // a writer holds it or has it reserved
            if (!wait_change(lock, c, timeout, start)) return -3;
            continue;
        }
        if (InterlockedCompareExchange(&lock->count, c + 1, c) == c) {
            return c == kFree || timeout == INFINITE ? 0 : -3;
        }
    }
}

// thiscall (lock): 0 = locked as the first reader; 0xFFFFFFFE (-2) otherwise, and, like the original, also for a reader
// that joined others (counted).
int __fastcall try_read_lock(Lock* lock) {
    check(lock, 0x10DD074, 0x17E, 0x10DD058);
    for (;;) {
        const LONG c = lock->count;
        if (c != kFree && !is_readers(c)) return -2;
        if (InterlockedCompareExchange(&lock->count, c + 1, c) == c) return c == kFree ? 0 : -2;
    }
}

// thiscall (lock): 0. The last reader wakes a waiting writer.
int __fastcall read_unlock(Lock* lock) {
    check(lock, 0x10DD0B8, 0x1A3, 0x10DD09C);
    const LONG n = InterlockedDecrement(&lock->count);
    if (n == kFree || n == kReserved) wake(lock);  // the last reader: a waiting writer may go
    return 0;
}

// A writer that stops waiting (timed out) takes its mark back; writers still waiting wake on the change and mark the
// count again.
void unmark_waiting(Lock* lock) {
    for (;;) {
        const LONG c = lock->count;
        LONG n;
        if (c == kReserved) {
            n = kFree;
        } else if (is_readers(c) && (c & kWaiting)) {
            n = c & ~kWaiting;
        } else {
            return;
        }
        if (InterlockedCompareExchange(&lock->count, n, c) == c) {
            wake(lock);
            return;
        }
    }
}

// thiscall (lock, timeout), pops 4: 0 = locked; 0xFFFFFFFD (-3) on a timeout.
int __fastcall write_lock(Lock* lock, void*, DWORD timeout) {
    check(lock, 0x10DCEE4, 0xC7, 0x10DCEC8);
    if (timeout == 0) game_assert(reinterpret_cast<const char*>(0x10DCF24), 0xC8, reinterpret_cast<const char*>(0x10DCF0C));
    const ULONGLONG start = timeout == INFINITE ? 0 : GetTickCount64();
    for (;;) {
        const LONG c = lock->count;
        if (c == kFree || c == kReserved) {
            if (InterlockedCompareExchange(&lock->count, kWriter, c) == c) return 0;
            continue;
        }
        LONG seen = c;
        if (is_readers(c) && !(c & kWaiting)) {  // readers hold it: mark that a writer waits
            if (InterlockedCompareExchange(&lock->count, c | kWaiting, c) != c) continue;
            seen = c | kWaiting;
        }
        if (!wait_change(lock, seen, timeout, start)) {
            unmark_waiting(lock);
            return -3;
        }
    }
}

// thiscall (lock): 0 = locked; 0xFFFFFFFE (-2) when it is held.
int __fastcall try_write_lock(Lock* lock) {
    check(lock, 0x10DCF68, 0xFA, 0x10DCF4C);
    for (;;) {
        const LONG c = lock->count;
        if (c != kFree && c != kReserved) return -2;
        if (InterlockedCompareExchange(&lock->count, kWriter, c) == c) return 0;
    }
}

// thiscall (lock): 0. Readers and writers waiting are woken.
int __fastcall write_unlock(Lock* lock) {
    check(lock, 0x10DCFAC, 0x11B, 0x10DCF90);
    InterlockedExchange(&lock->count, kFree);
    wake(lock);
    return 0;
}

const uint8_t kReadLockHead[] = {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x57, 0x75, 0x17, 0x68, 0xD4, 0xCF, 0x0D, 0x01};
const uint8_t kTryReadLockHead[] = {0x53, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x57, 0x75, 0x17, 0x68, 0x58, 0xD0, 0x0D, 0x01};
const uint8_t kReadUnlockHead[] = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x75, 0x17, 0x68, 0x9C, 0xD0, 0x0D, 0x01};
const uint8_t kWriteLockHead[] = {0x53, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x57, 0x75, 0x17, 0x68, 0xC8, 0xCE, 0x0D, 0x01};
const uint8_t kTryWriteLockHead[] = {0x53, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x57, 0x75, 0x17, 0x68, 0x4C, 0xCF, 0x0D, 0x01};
const uint8_t kWriteUnlockHead[] = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x75, 0x17, 0x68, 0x90, 0xCF, 0x0D, 0x01};
const uint8_t kThunks[] = {0xE9, 0x9B, 0xFC, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 0x60D300 jmp 0x60CFA0
                           0xE9, 0x3B, 0xFD, 0xFF, 0xFF};                                  // 0x60D310 jmp 0x60D050
// The counting itself, which the replacements take over (checked so a different build is left alone).
const uint8_t kReadLockCount[] = {0x83, 0x46, 0x10, 0x01};    // 0x60D146 add dword ptr [esi+10h], 1
const uint8_t kReadUnlockCount[] = {0x83, 0x46, 0x10, 0xFF};  // 0x60D24C add dword ptr [esi+10h], -1

void make_jump(uint8_t* out, uintptr_t site, const void* target) {
    out[0] = 0xE9;
    const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(out + 1, &rel, 4);
}

// The jump goes in with one atomic 8-byte write (the functions start 16-byte aligned), keeping bytes 5 to 7.
void write_jump_atomic(uintptr_t site, const uint8_t* jump) {
    auto* q = reinterpret_cast<volatile LONGLONG*>(site);
    LONGLONG old_value = *q;
    LONGLONG new_value = old_value;
    std::memcpy(&new_value, jump, 5);
    InterlockedCompareExchange64(q, new_value, old_value);
}

// What the install in DllMain found, for rwlock_log() once the log is open.
enum Result { kNotTried, kInstalled, kLateLoad, kWrongBase, kNoWaitOnAddress, kMismatch, kProtectFailed };
Result g_result = kNotTried;
DWORD g_error = 0;

}  // namespace

void rwlock_install_early(bool static_load) {
    if (!static_load) {
        g_result = kLateLoad;
        return;
    }
    // The game's image at its preferred base (its headers there, linked for that base).
    MEMORY_BASIC_INFORMATION mbi{};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(kImageBase);
    if (!VirtualQuery(dos, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(kImageBase + dos->e_lfanew)->OptionalHeader.ImageBase != kImageBase) {
        g_result = kWrongBase;
        return;
    }
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
        g_wait_on_address = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(kernelbase, "WaitOnAddress"));
        g_wake_all = reinterpret_cast<WakeByAddressAllFn>(GetProcAddress(kernelbase, "WakeByAddressAll"));
    }
    if (!g_wait_on_address || !g_wake_all) {
        g_result = kNoWaitOnAddress;
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kReadLock), kReadLockHead, sizeof(kReadLockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kTryReadLock), kTryReadLockHead, sizeof(kTryReadLockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kReadUnlock), kReadUnlockHead, sizeof(kReadUnlockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kWriteLock), kWriteLockHead, sizeof(kWriteLockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kTryWriteLock), kTryWriteLockHead, sizeof(kTryWriteLockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kWriteUnlock), kWriteUnlockHead, sizeof(kWriteUnlockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kWriteUnlock2), kWriteUnlockHead, sizeof(kWriteUnlockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(0x60D300), kThunks, 5) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(0x60D310), kThunks + 16, 5) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(0x60D146), kReadLockCount, sizeof(kReadLockCount)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(0x60D24C), kReadUnlockCount, sizeof(kReadUnlockCount)) != 0) {
        g_result = kMismatch;
        return;
    }
    constexpr int kSites = 7;
    const uintptr_t sites[kSites] = {kReadLock, kTryReadLock, kReadUnlock, kWriteLock, kTryWriteLock, kWriteUnlock,
                                     kWriteUnlock2};
    const void* targets[kSites] = {reinterpret_cast<const void*>(&read_lock), reinterpret_cast<const void*>(&try_read_lock),
                                   reinterpret_cast<const void*>(&read_unlock), reinterpret_cast<const void*>(&write_lock),
                                   reinterpret_cast<const void*>(&try_write_lock),
                                   reinterpret_cast<const void*>(&write_unlock),
                                   reinterpret_cast<const void*>(&write_unlock)};
    uint8_t jumps[kSites][5];
    for (int i = 0; i < kSites; ++i) make_jump(jumps[i], sites[i], targets[i]);
    DWORD old = 0;
    auto* code = reinterpret_cast<void*>(kCodeLo);
    if (!VirtualProtect(code, kCodeHi - kCodeLo, PAGE_EXECUTE_READWRITE, &old)) {
        g_error = GetLastError();
        g_result = kProtectFailed;
        return;
    }
    for (int i = 0; i < kSites; ++i) write_jump_atomic(sites[i], jumps[i]);
    FlushInstructionCache(GetCurrentProcess(), code, kCodeHi - kCodeLo);
    DWORD ignored = 0;
    VirtualProtect(code, kCodeHi - kCodeLo, old, &ignored);
    g_result = kInstalled;
}

void rwlock_log(bool wanted) {
    switch (g_result) {
    case kInstalled:
        LOG_INFO("FastReadLocks: the game's read/write locks take and give back without kernel calls (put in place before "
                 "the game started)");
        break;
    case kLateLoad:
        LOG_INFO("FastReadLocks: this DLL was loaded after the game started (not as one of its own imports), when the "
                 "game's code may already be inside its locks; they are left as they are");
        break;
    case kWrongBase:
        LOG_ERROR("FastReadLocks needs the game at its preferred base. Not patching.");
        break;
    case kNoWaitOnAddress:
        LOG_ERROR("FastReadLocks: WaitOnAddress is not available here; the game's locks are left as they are");
        break;
    case kMismatch:
        LOG_ERROR("FastReadLocks: the lock functions do not match this build. Not patching.");
        break;
    case kProtectFailed:
        LOG_ERROR("FastReadLocks: could not unprotect the lock functions (Win32=%lu)", g_error);
        break;
    case kNotTried:
        if (wanted) LOG_INFO("FastReadLocks: not put in place at start-up; the game's read locks keep their kernel calls");
        else LOG_INFO("FastReadLocks is false. The game's read locks keep their kernel calls.");
        break;
    }
}
