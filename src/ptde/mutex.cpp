#include "mutex.h"

#include "log.h"
#include "rwlock.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <new>

namespace {

// DLKR::DLPlainMutex (vtable 0x10DEAB4; DLUT::DLLifecycleAdapter<DLPlainMutex> 0x10D4538 shares its methods): +0 the
// vtable, +4 the mutex. Lock (slot 2), TryLock (3) and Unlock (4) are only ever called through the tables.
constexpr uintptr_t kLock = 0x633390;
constexpr uintptr_t kTryLock = 0x633400;
constexpr uintptr_t kUnlock = 0x633450;
constexpr uintptr_t kCreateCall = 0x633334;    // Create (0x633320): call [CreateMutexW] (the game's only one)
constexpr uintptr_t kDestroyClose = 0x633362;  // Destroy (0x633350): call [CloseHandle]
constexpr uintptr_t kDtorClose = 0x6334E4;     // the destructor (0x6334A0): call [CloseHandle]
constexpr uintptr_t kCodeLo = 0x633330;
constexpr uintptr_t kCodeHi = 0x633500;

struct GameMutex {
    void* vtable;
    volatile uintptr_t handle;  // the kernel mutex, or ours with bit 0 set (kernel handles have their low two bits clear)
};

// Ours: the owning thread's id (0 = free), how many times it holds it (the kernel mutex's recursion), and the
// threads asleep on it.
struct Fast {
    volatile LONG owner;
    LONG depth;
    volatile LONG waiters;
    LONG reserved;
};

inline bool is_fast(uintptr_t h) { return (h & 1) != 0; }
inline Fast* fast_of(uintptr_t h) { return reinterpret_cast<Fast*>(h & ~static_cast<uintptr_t>(1)); }
inline LONG self_id() { return static_cast<LONG>(__readfsdword(0x24)); }  // the TEB's thread id (GetCurrentThreadId)

// The game's assert reporter (file, line, message), where the originals call it, with their strings.
using AssertFn = void(__cdecl*)(const char*, int, const char*);
const AssertFn game_assert = reinterpret_cast<AssertFn>(0x559E60);
inline void report(uintptr_t file, int line, uintptr_t message) {
    game_assert(reinterpret_cast<const char*>(file), line, reinterpret_cast<const char*>(message));
}

using WaitOnAddressFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressFn = void(WINAPI*)(PVOID);
WaitOnAddressFn g_wait_on_address = nullptr;
WakeByAddressFn g_wake_single = nullptr;

// A thread that finds the lock taken spins briefly (most holds are short), then sleeps on the owner word. It never
// sleeps more than kMaxSleep ms at a time and looks again (a missed wake costs that much, as in rwlock.cpp).
constexpr int kSpins = 256;
constexpr DWORD kMaxSleep = 5;

inline bool take(Fast* f, LONG me) {
    if (f->owner == 0 && InterlockedCompareExchange(&f->owner, me, 0) == 0) {
        f->depth = 1;
        return true;
    }
    return false;
}

// Waits for the lock (it was taken when we looked): 0 locked, -3 timed out.
int contended(Fast* f, LONG me, DWORD timeout) {
    for (int i = 0; i < kSpins; ++i) {
        _mm_pause();
        if (take(f, me)) return 0;
    }
    const DWORD error = GetLastError();  // the original's WaitForSingleObject leaves it alone when it succeeds
    const ULONGLONG start = timeout == INFINITE ? 0 : GetTickCount64();
    InterlockedIncrement(&f->waiters);
    int result = 0;
    for (;;) {
        LONG seen = f->owner;
        if (seen == 0) {
            if (InterlockedCompareExchange(&f->owner, me, 0) == 0) {
                f->depth = 1;
                break;
            }
            continue;
        }
        DWORD slice = kMaxSleep;
        if (timeout != INFINITE) {
            const ULONGLONG spent = GetTickCount64() - start;
            if (spent >= timeout) {
                result = -3;
                break;
            }
            if (timeout - spent < slice) slice = static_cast<DWORD>(timeout - spent);
        }
        g_wait_on_address(&f->owner, &seen, sizeof(seen), slice);
    }
    InterlockedDecrement(&f->waiters);
    SetLastError(result == 0 ? error : WAIT_TIMEOUT);
    return result;
}

// thiscall (mutex, timeout), pops 4: 0 locked; -3 (0xFFFFFFFD) timed out; -4 failed.
int __fastcall lock(GameMutex* m, void*, DWORD timeout) {
    if (!m->handle) report(0x10DE9E8, 0x88, 0x10DE9D0);
    if (timeout == 0) report(0x10DEA20, 0x89, 0x10DEA08);
    const uintptr_t h = m->handle;
    if (!is_fast(h)) {
        const DWORD r = WaitForSingleObject(reinterpret_cast<HANDLE>(h), timeout);
        return r == WAIT_OBJECT_0 ? 0 : r == WAIT_TIMEOUT ? -3 : -4;
    }
    Fast* f = fast_of(h);
    const LONG me = self_id();
    if (f->owner == me) {
        ++f->depth;
        return 0;
    }
    if (take(f, me)) return 0;
    return contended(f, me, timeout);
}

// thiscall (mutex): 0 locked; -2 (0xFFFFFFFE) held by another thread; -4 failed.
int __fastcall try_lock(GameMutex* m) {
    if (!m->handle) report(0x10DEA58, 0xA4, 0x10DEA40);
    const uintptr_t h = m->handle;
    if (!is_fast(h)) {
        const DWORD r = WaitForSingleObject(reinterpret_cast<HANDLE>(h), 0);
        return r == WAIT_OBJECT_0 ? 0 : r == WAIT_TIMEOUT ? -2 : -4;
    }
    Fast* f = fast_of(h);
    const LONG me = self_id();
    if (f->owner == me) {
        ++f->depth;
        return 0;
    }
    return take(f, me) ? 0 : -2;
}

// thiscall (mutex): 0 released; -4 when this thread does not hold it (ReleaseMutex's ERROR_NOT_OWNER).
int __fastcall unlock(GameMutex* m) {
    if (!m->handle) report(0x10DEA90, 0xBE, 0x10DEA78);
    const uintptr_t h = m->handle;
    if (!is_fast(h)) return ReleaseMutex(reinterpret_cast<HANDLE>(h)) ? 0 : -4;
    Fast* f = fast_of(h);
    if (f->owner != self_id() || f->depth <= 0) {
        SetLastError(ERROR_NOT_OWNER);
        return -4;
    }
    if (--f->depth > 0) return 0;
    InterlockedExchange(&f->owner, 0);
    // An interlocked read: ordered after the exchange on every CPU (see rwlock.cpp's wake()).
    if (InterlockedCompareExchange(&f->waiters, 0, 0)) g_wake_single(const_cast<LONG*>(&f->owner));
    return 0;
}

// In place of Create's CreateMutexW(NULL, FALSE, NULL): ours, tagged; the kernel's if it cannot be allocated.
HANDLE WINAPI fast_create(LPSECURITY_ATTRIBUTES attributes, BOOL owned, LPCWSTR name) {
    Fast* f = (!owned && !name) ? new (std::nothrow) Fast() : nullptr;
    if (!f) return CreateMutexW(attributes, owned, name);
    return reinterpret_cast<HANDLE>(reinterpret_cast<uintptr_t>(f) | 1);
}

// In place of Destroy's and the destructor's CloseHandle.
BOOL WINAPI fast_close(HANDLE h) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(h);
    if (!is_fast(v)) return CloseHandle(h);
    delete fast_of(v);
    return TRUE;
}

// Each patched place, with the bytes it must hold first.
const uint8_t kLockHead[] = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x57, 0x75, 0x17, 0x68, 0xD0, 0xE9, 0x0D, 0x01};
const uint8_t kTryLockHead[] = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x75, 0x17, 0x68, 0x40, 0xEA, 0x0D, 0x01};
const uint8_t kUnlockHead[] = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x04, 0x00, 0x75, 0x17, 0x68, 0x78, 0xEA, 0x0D, 0x01};
const uint8_t kCallCreate[] = {0xFF, 0x15, 0x38, 0xC2, 0x0C, 0x01};  // call dword ptr [CreateMutexW]
const uint8_t kCallClose[] = {0xFF, 0x15, 0x28, 0xC2, 0x0C, 0x01};   // call dword ptr [CloseHandle]

void make_rel(uint8_t* out, uint8_t op, uintptr_t site, const void* target) {
    out[0] = op;
    const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(out + 1, &rel, 4);
}

enum Result { kNotTried, kInstalled, kLateLoad, kNoWaitOnAddress, kMismatch, kProtectFailed, kTranslated };
Result g_result = kNotTried;
bool g_forced_translated = false;
DWORD g_error = 0;

}  // namespace

void mutex_install_early(bool static_load, bool even_translated) {
    if (!static_load) {
        g_result = kLateLoad;
        return;
    }
    if (cpu_translated()) {
        if (!even_translated) {
            g_result = kTranslated;
            return;
        }
        g_forced_translated = true;
    }
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
        g_wait_on_address = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(kernelbase, "WaitOnAddress"));
        g_wake_single = reinterpret_cast<WakeByAddressFn>(GetProcAddress(kernelbase, "WakeByAddressSingle"));
    }
    if (!g_wait_on_address || !g_wake_single) {
        g_result = kNoWaitOnAddress;
        return;
    }
    // The game's image at its preferred base, its lock code as this build's.
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(kLock), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        std::memcmp(reinterpret_cast<const void*>(kLock), kLockHead, sizeof(kLockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kTryLock), kTryLockHead, sizeof(kTryLockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kUnlock), kUnlockHead, sizeof(kUnlockHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kCreateCall), kCallCreate, sizeof(kCallCreate)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kDestroyClose), kCallClose, sizeof(kCallClose)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kDtorClose), kCallClose, sizeof(kCallClose)) != 0) {
        g_result = kMismatch;
        return;
    }
    DWORD old = 0;
    auto* code = reinterpret_cast<uint8_t*>(kCodeLo);
    if (!VirtualProtect(code, kCodeHi - kCodeLo, PAGE_EXECUTE_READWRITE, &old)) {
        g_error = GetLastError();
        g_result = kProtectFailed;
        return;
    }
    // Before the game's entry point (no thread runs its code yet): plain writes.
    uint8_t patch[6];
    make_rel(patch, 0xE8, kCreateCall, reinterpret_cast<const void*>(&fast_create));
    patch[5] = 0x90;
    std::memcpy(reinterpret_cast<void*>(kCreateCall), patch, 6);
    make_rel(patch, 0xE8, kDestroyClose, reinterpret_cast<const void*>(&fast_close));
    std::memcpy(reinterpret_cast<void*>(kDestroyClose), patch, 6);
    make_rel(patch, 0xE8, kDtorClose, reinterpret_cast<const void*>(&fast_close));
    std::memcpy(reinterpret_cast<void*>(kDtorClose), patch, 6);
    make_rel(patch, 0xE9, kLock, reinterpret_cast<const void*>(&lock));
    std::memcpy(reinterpret_cast<void*>(kLock), patch, 5);
    make_rel(patch, 0xE9, kTryLock, reinterpret_cast<const void*>(&try_lock));
    std::memcpy(reinterpret_cast<void*>(kTryLock), patch, 5);
    make_rel(patch, 0xE9, kUnlock, reinterpret_cast<const void*>(&unlock));
    std::memcpy(reinterpret_cast<void*>(kUnlock), patch, 5);
    FlushInstructionCache(GetCurrentProcess(), code, kCodeHi - kCodeLo);
    DWORD ignored = 0;
    VirtualProtect(code, kCodeHi - kCodeLo, old, &ignored);
    g_result = kInstalled;
}

void mutex_log(bool wanted) {
    const char* cpu = nullptr;
    cpu_translated(&cpu);
    switch (g_result) {
    case kInstalled:
        LOG_INFO("FastMutexes: the game's mutexes (DLPlainMutex) are taken and given back without kernel calls (put in "
                 "place before the game started)%s%s%s", g_forced_translated ? "; the game runs translated for another "
                 "CPU (" : "", g_forced_translated ? cpu : "", g_forced_translated ? "), on by FastMutexes = always" : "");
        break;
    case kLateLoad:
        LOG_INFO("FastMutexes: this DLL was loaded after the game started; the game's mutexes are left as they are");
        break;
    case kNoWaitOnAddress:
        LOG_ERROR("FastMutexes: WaitOnAddress is not available here; the game's mutexes are left as they are");
        break;
    case kMismatch:
        LOG_ERROR("FastMutexes: the mutex functions do not match this build. Not patching.");
        break;
    case kProtectFailed:
        LOG_ERROR("FastMutexes: could not unprotect the mutex functions (Win32=%lu)", g_error);
        break;
    case kTranslated:
        LOG_INFO("FastMutexes: off: the game runs translated for another CPU (%s); its mutexes keep their kernel calls "
                 "(FastMutexes = always puts them in place there too)", cpu ? cpu : "?");
        break;
    case kNotTried:
        if (wanted) LOG_INFO("FastMutexes: not put in place at start-up; the game's mutexes keep their kernel calls");
        else LOG_INFO("FastMutexes is false. The game's mutexes keep their kernel calls.");
        break;
    }
}
