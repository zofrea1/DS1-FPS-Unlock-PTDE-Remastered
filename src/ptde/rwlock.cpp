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
constexpr uintptr_t kCodeLo = 0x60D0F0;  // the three functions
constexpr uintptr_t kCodeHi = 0x60D270;

// The count while a first reader waits for the event (the original held the reader mutex meanwhile).
constexpr LONG kAcquiring = static_cast<LONG>(0x80000000u);

struct Lock {
    void* vtable;
    HANDLE event;        // signalled = free
    HANDLE writer;       // writers only
    HANDLE reader_mutex;  // no longer used by readers
    volatile LONG count;  // -1 = no readers, n = n + 1 readers
};

// The game's assert reporter (file, line, message), called where the originals call it, with their strings.
using AssertFn = void(__cdecl*)(const char*, int, const char*);
const AssertFn game_assert = reinterpret_cast<AssertFn>(0x559E60);

using WaitOnAddressFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressAllFn = void(WINAPI*)(PVOID);
WaitOnAddressFn g_wait_on_address = nullptr;
WakeByAddressAllFn g_wake_all = nullptr;

// Waits until the count is no longer kAcquiring or `timeout` ms after `start` have passed (false then).
bool wait_while_acquiring(Lock* lock, DWORD timeout, ULONGLONG start) {
    DWORD left = INFINITE;
    if (timeout != INFINITE) {
        const ULONGLONG spent = GetTickCount64() - start;
        if (spent >= timeout) return false;
        left = static_cast<DWORD>(timeout - spent);
    }
    LONG marker = kAcquiring;
    g_wait_on_address(&lock->count, &marker, sizeof(marker), left);
    return true;
}

void finish_acquiring(Lock* lock, LONG value) {
    InterlockedExchange(&lock->count, value);
    g_wake_all(const_cast<LONG*>(&lock->count));
}

// thiscall (lock, timeout), pops 4: 0 = locked; 0xFFFFFFFD (-3) on a timeout, and also, like the original, for a reader
// that joined others on a finite timeout (it is counted all the same).
int __fastcall read_lock(Lock* lock, void*, DWORD timeout) {
    if (!lock->event) game_assert(reinterpret_cast<const char*>(0x10DCFF0), 0x13F, reinterpret_cast<const char*>(0x10DCFD4));
    if (timeout == 0) game_assert(reinterpret_cast<const char*>(0x10DD030), 0x140, reinterpret_cast<const char*>(0x10DD018));
    const ULONGLONG start = timeout == INFINITE ? 0 : GetTickCount64();
    for (;;) {
        const LONG c = lock->count;
        if (c == kAcquiring) {
            if (!wait_while_acquiring(lock, timeout, start)) return -3;
            continue;
        }
        if (c == -1) {
            if (InterlockedCompareExchange(&lock->count, kAcquiring, -1) != -1) continue;
            const DWORD r = WaitForSingleObject(lock->event, timeout);
            if (timeout == INFINITE || r == WAIT_OBJECT_0) {
                finish_acquiring(lock, 0);
                return 0;
            }
            finish_acquiring(lock, -1);
            return -3;
        }
        if (InterlockedCompareExchange(&lock->count, c + 1, c) == c) {
            return timeout == INFINITE ? 0 : -3;
        }
    }
}

// thiscall (lock): 0 = locked as the first reader; 0xFFFFFFFE (-2) otherwise, and, like the original, also for a reader
// that joined others (counted).
int __fastcall try_read_lock(Lock* lock) {
    if (!lock->event) game_assert(reinterpret_cast<const char*>(0x10DD074), 0x17E, reinterpret_cast<const char*>(0x10DD058));
    for (;;) {
        const LONG c = lock->count;
        if (c == kAcquiring) return -2;  // the reader mutex would have been held
        if (c == -1) {
            if (InterlockedCompareExchange(&lock->count, kAcquiring, -1) != -1) continue;
            if (WaitForSingleObject(lock->event, 0) == WAIT_OBJECT_0) {
                finish_acquiring(lock, 0);
                return 0;
            }
            finish_acquiring(lock, -1);
            return -2;
        }
        if (InterlockedCompareExchange(&lock->count, c + 1, c) == c) return -2;
    }
}

// thiscall (lock): the last reader gives the event back. 0.
int __fastcall read_unlock(Lock* lock) {
    if (!lock->event) game_assert(reinterpret_cast<const char*>(0x10DD0B8), 0x1A3, reinterpret_cast<const char*>(0x10DD09C));
    for (;;) {
        const LONG c = lock->count;
        if (c == kAcquiring) {
            wait_while_acquiring(lock, INFINITE, 0);
            continue;
        }
        const LONG n = c - 1;
        if (InterlockedCompareExchange(&lock->count, n, c) == c) {
            if (n < 0) SetEvent(lock->event);
            return 0;
        }
    }
}

const uint8_t kReadLockHead[] = {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x57, 0x75, 0x17, 0x68, 0xD4, 0xCF, 0x0D, 0x01};
const uint8_t kTryReadLockHead[] = {0x53, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x57, 0x75, 0x17, 0x68, 0x58, 0xD0, 0x0D, 0x01};
const uint8_t kReadUnlockHead[] = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x75, 0x17, 0x68, 0x9C, 0xD0, 0x0D, 0x01};
// The counting itself, which the replacements take over (checked so a different build is left alone).
const uint8_t kReadLockCount[] = {0x83, 0x46, 0x10, 0x01};    // 0x60D146 add dword ptr [esi+10h], 1
const uint8_t kReadUnlockCount[] = {0x83, 0x46, 0x10, 0xFF};  // 0x60D24C add dword ptr [esi+10h], -1

void make_jump(uint8_t* out, uintptr_t site, const void* target) {
    out[0] = 0xE9;
    const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(out + 1, &rel, 4);
}

// Threads are listed with NtGetNextThread, which allocates nothing: while threads are suspended, one of them may hold
// the heap lock.
using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
NtGetNextThreadFn g_next_thread = nullptr;

constexpr int kMaxThreads = 1024;
struct Suspended {
    HANDLE handles[kMaxThreads];
    DWORD ids[kMaxThreads];
    int count = 0;
};

void resume_all(Suspended& s) {
    for (int i = 0; i < s.count; ++i) {
        ResumeThread(s.handles[i]);
        CloseHandle(s.handles[i]);
    }
    s.count = 0;
}

// True when the thread runs in, or returns into, the three functions (its EIP, or a return address within 4 KB of its
// stack pointer).
bool inside_lock_code(HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &ctx)) return true;  // unknown: treat as busy
    if (ctx.Eip >= kCodeLo && ctx.Eip < kCodeHi) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(ctx.Esp)), &mbi, sizeof(mbi))) return true;
    const uintptr_t top = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    uintptr_t end = static_cast<uintptr_t>(ctx.Esp) + 4096;
    if (end > top) end = top;
    for (uintptr_t p = static_cast<uintptr_t>(ctx.Esp) & ~3u; p + 4 <= end; p += 4) {
        const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
        if (v > kCodeLo && v < kCodeHi) return true;
    }
    return false;
}

// Suspends every other thread; false (all resumed) when one is in the lock code or the list could not be completed.
bool suspend_others(Suspended& s) {
    const DWORD self = GetCurrentThreadId();
    HANDLE prev = nullptr;
    HANDLE next = nullptr;
    bool ok = true;
    while (g_next_thread(GetCurrentProcess(), prev, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                        THREAD_QUERY_LIMITED_INFORMATION,
                         0, 0, &next) >= 0) {
        if (prev) CloseHandle(prev);
        prev = next;
        const DWORD id = GetThreadId(next);
        if (id == self || id == 0) continue;
        if (s.count >= kMaxThreads) {
            ok = false;
            break;
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), next, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            ok = false;
            break;
        }
        if (SuspendThread(dup) == static_cast<DWORD>(-1)) {
            CloseHandle(dup);  // exiting
            continue;
        }
        s.handles[s.count] = dup;
        s.ids[s.count] = id;
        ++s.count;
        if (inside_lock_code(dup)) {
            ok = false;
            break;
        }
    }
    if (prev) CloseHandle(prev);
    if (!ok) resume_all(s);
    return ok;
}

// With the others suspended: true when no thread exists that is not among them (none started meanwhile).
bool all_suspended(const Suspended& s) {
    const DWORD self = GetCurrentThreadId();
    HANDLE prev = nullptr;
    HANDLE next = nullptr;
    bool ok = true;
    while (g_next_thread(GetCurrentProcess(), prev, THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &next) >= 0) {
        if (prev) CloseHandle(prev);
        prev = next;
        const DWORD id = GetThreadId(next);
        if (id == self || id == 0) continue;
        bool known = false;
        for (int i = 0; i < s.count && !known; ++i) known = s.ids[i] == id;
        if (!known) {
            ok = false;
            break;
        }
    }
    if (prev) CloseHandle(prev);
    return ok;
}

// The jump goes in with one atomic 8-byte write (the functions start 16-byte aligned), keeping bytes 5 to 7.
void write_jump_atomic(uintptr_t site, const uint8_t* jump) {
    auto* q = reinterpret_cast<volatile LONGLONG*>(site);
    LONGLONG old_value = *q;
    LONGLONG new_value = old_value;
    std::memcpy(&new_value, jump, 5);
    InterlockedCompareExchange64(q, new_value, old_value);
}

}  // namespace

bool rwlock_install() {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        LOG_ERROR("FastReadLocks needs the game at its preferred base. Not patching.");
        return false;
    }
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
        g_wait_on_address = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(kernelbase, "WaitOnAddress"));
        g_wake_all = reinterpret_cast<WakeByAddressAllFn>(GetProcAddress(kernelbase, "WakeByAddressAll"));
    }
    if (!g_wait_on_address || !g_wake_all) {
        LOG_ERROR("FastReadLocks: WaitOnAddress is not available here; the game's locks are left as they are");
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kReadLock), kReadLockHead, sizeof(kReadLockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kTryReadLock), kTryReadLockHead, sizeof(kTryReadLockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kReadUnlock), kReadUnlockHead, sizeof(kReadUnlockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(0x60D146), kReadLockCount, sizeof(kReadLockCount)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(0x60D24C), kReadUnlockCount, sizeof(kReadUnlockCount)) != 0) {
        LOG_ERROR("FastReadLocks: the lock functions do not match this build. Not patching.");
        return false;
    }
    uint8_t jumps[3][5];
    make_jump(jumps[0], kReadLock, reinterpret_cast<const void*>(&read_lock));
    make_jump(jumps[1], kTryReadLock, reinterpret_cast<const void*>(&try_read_lock));
    make_jump(jumps[2], kReadUnlock, reinterpret_cast<const void*>(&read_unlock));
    const uintptr_t sites[3] = {kReadLock, kTryReadLock, kReadUnlock};

    g_next_thread = reinterpret_cast<NtGetNextThreadFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread"));
    if (!g_next_thread) {
        LOG_ERROR("FastReadLocks: NtGetNextThread is not available here; the game's locks are left as they are");
        return false;
    }
    DWORD old = 0;
    auto* code = reinterpret_cast<void*>(kCodeLo);
    if (!VirtualProtect(code, kCodeHi - kCodeLo, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("FastReadLocks: could not unprotect the lock functions (Win32=%lu)", GetLastError());
        return false;
    }
    static Suspended suspended;
    int attempts = 0;
    int threads_seen = 0;
    bool done = false;
    for (; attempts < 500 && !done; ++attempts) {
        if (suspend_others(suspended)) {
            if (all_suspended(suspended)) {
                for (int i = 0; i < 3; ++i) write_jump_atomic(sites[i], jumps[i]);
                FlushInstructionCache(GetCurrentProcess(), code, kCodeHi - kCodeLo);
                done = true;
                threads_seen = suspended.count;
            }
            resume_all(suspended);
        }
        if (!done) Sleep(1);
    }
    DWORD ignored = 0;
    VirtualProtect(code, kCodeHi - kCodeLo, old, &ignored);
    if (!done) {
        LOG_ERROR("FastReadLocks: a thread was always inside the game's lock code (%d tries); left as it is", attempts);
        return false;
    }
    LOG_INFO("FastReadLocks: the game's read locks count without kernel calls (installed with %d other threads "
             "paused, try %d)",
             threads_seen, attempts);
    return true;
}
