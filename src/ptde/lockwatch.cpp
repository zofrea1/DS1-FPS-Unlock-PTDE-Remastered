#include "lockwatch.h"

#include "fpsu_buildcfg.h"
#include "log.h"
#include "profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#if FPSU_DIAGNOSTICS

namespace {

constexpr uint32_t kLock = 0x00567E70;     // thiscall(this, timeout) ret 4: EnterCriticalSection(this + 4)
constexpr uint32_t kTryLock = 0x00567EB0;  // thiscall(this): TryEnterCriticalSection, 0 or -2
constexpr uint32_t kUnlock = 0x00567ED0;   // thiscall(this): LeaveCriticalSection(this + 4)
const uint8_t kLockHead[5] = {0x83, 0x7C, 0x24, 0x04, 0xFF};
const uint8_t kTryHead[5] = {0x56, 0x83, 0xC1, 0x04, 0x51};
const uint8_t kUnlockHead[5] = {0x83, 0xC1, 0x04, 0x51, 0xFF};

constexpr double kLogMs = 40.0;
constexpr LONG kMaxLogged = 400;
constexpr int kHeldMax = 24;

struct Held {
    void* m;
    LONGLONG since;
    uint32_t site;
    int depth;
};
struct PerThread {
    Held held[kHeldMax];
    int n;
};
thread_local PerThread t_held;

LARGE_INTEGER g_freq{};
volatile LONG g_logged = 0;
LONGLONG g_min_ticks = 0;

LONGLONG ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void label(DWORD tid, char* out, size_t n) {
    if (tid == g_game_thread_id) {
        std::snprintf(out, n, "game thread");
    } else if (tid == g_render_thread_id) {
        std::snprintf(out, n, "render thread");
    } else {
        std::snprintf(out, n, "thread %lu", tid);
    }
}

void took(void* m, uint32_t site, LONGLONG now) {
    PerThread& t = t_held;
    for (int i = t.n - 1; i >= 0; --i) {
        if (t.held[i].m == m) {
            ++t.held[i].depth;
            return;
        }
    }
    if (t.n < kHeldMax) t.held[t.n++] = Held{m, now, site, 1};
}

int __fastcall hk_lock(void* self, void*, int timeout) {
    (void)timeout;
    auto* cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(self) + 4);
    const uint32_t site = reinterpret_cast<uint32_t>(_ReturnAddress());
    const LONGLONG a = ticks();
    if (!TryEnterCriticalSection(cs)) {
        const DWORD owner = static_cast<DWORD>(reinterpret_cast<uintptr_t>(cs->OwningThread));
        EnterCriticalSection(cs);
        const LONGLONG b = ticks();
        if (b - a >= g_min_ticks && InterlockedIncrement(&g_logged) <= kMaxLogged) {
            char who[32], whom[32];
            label(GetCurrentThreadId(), who, sizeof(who));
            label(owner, whom, sizeof(whom));
            LOG_INFO("[lock] %s waited %.1f ms at %08X for mutex %p, owned by %s", who,
                     1000.0 * static_cast<double>(b - a) / g_freq.QuadPart, site, self, whom);
        }
        took(self, site, b);
        return 0;
    }
    took(self, site, a);
    return 0;
}

int __fastcall hk_trylock(void* self, void*) {
    auto* cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(self) + 4);
    if (!TryEnterCriticalSection(cs)) return -2;
    took(self, reinterpret_cast<uint32_t>(_ReturnAddress()), ticks());
    return 0;
}

int __fastcall hk_unlock(void* self, void*) {
    auto* cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(self) + 4);
    PerThread& t = t_held;
    for (int i = t.n - 1; i >= 0; --i) {
        if (t.held[i].m != self) continue;
        if (--t.held[i].depth > 0) break;
        const LONGLONG held = ticks() - t.held[i].since;
        const uint32_t site = t.held[i].site;
        t.held[i] = t.held[--t.n];
        if (held >= g_min_ticks && InterlockedIncrement(&g_logged) <= kMaxLogged) {
            char who[32];
            label(GetCurrentThreadId(), who, sizeof(who));
            LOG_INFO("[lock] %s held mutex %p for %.1f ms (taken at %08X)", who, self,
                     1000.0 * static_cast<double>(held) / g_freq.QuadPart, site);
        }
        break;
    }
    LeaveCriticalSection(cs);
    return 0;
}

bool jump(uint32_t site, const uint8_t* head, const void* to) {
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (std::memcmp(p, head, 5) != 0) return false;
    uint8_t code[5] = {0xE9};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (site + 5));
    std::memcpy(code + 1, &rel, 4);
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, code, 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

}  // namespace

void lockwatch_install() {
    QueryPerformanceFrequency(&g_freq);
    g_min_ticks = static_cast<LONGLONG>(kLogMs / 1000.0 * g_freq.QuadPart);
    // Unlock first: a lock taken by the original code is then still released by ours (which tolerates a mutex it
    // never saw taken).
    const bool ok = jump(kUnlock, kUnlockHead, reinterpret_cast<const void*>(&hk_unlock)) &&
                    jump(kTryLock, kTryHead, reinterpret_cast<const void*>(&hk_trylock)) &&
                    jump(kLock, kLockHead, reinterpret_cast<const void*>(&hk_lock));
    if (ok) {
        LOG_INFO("[lock] timing the engine's light mutexes (0x567E70): holds and waits of %.0f ms or more are logged",
                 kLogMs);
    } else {
        LOG_ERROR("[lock] the light mutex functions did not match this build (or were partly patched)");
    }
}

#else

void lockwatch_install() {}

#endif
