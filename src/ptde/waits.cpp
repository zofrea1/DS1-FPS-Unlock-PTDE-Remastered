#include "waits.h"

#include "intro.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// IAT slots of KERNEL32 imports in this build (checked against GetProcAddress).
constexpr uint32_t kIatSleep = 0x010CC208;
constexpr uint32_t kIatWfso = 0x010CC234;
constexpr uint32_t kIatWfmo = 0x010CC15C;

using SleepFn = VOID(WINAPI*)(DWORD);
using WfsoFn = DWORD(WINAPI*)(HANDLE, DWORD);
using WfmoFn = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD);
SleepFn g_sleep = nullptr;
WfsoFn g_wfso = nullptr;
WfmoFn g_wfmo = nullptr;

// While the intro is being skipped, waits of 20..300 ms are cut to 1 ms.
constexpr DWORD kAccelMin = 20;
constexpr DWORD kAccelMax = 300;

LARGE_INTEGER g_freq{};

struct Entry {
    volatile LONG ra;  // return address (32-bit image)
    LONG kind;         // 0 Sleep, 1 WaitForSingleObject, 2 WaitForMultipleObjects
    volatile LONG count;
    volatile LONG req_ms;      // sum of requested timeouts (INFINITE excluded)
    volatile LONG actual_ms;   // sum of measured wait durations
    volatile LONG max_req;
    volatile LONG infinite;    // calls with an INFINITE timeout
};
constexpr int kEntries = 128;
Entry g_table[kEntries];

Entry* slot_for(LONG ra, LONG kind) {
    const unsigned start = static_cast<unsigned>(ra >> 2) & (kEntries - 1);
    for (unsigned n = 0; n < kEntries; ++n) {
        Entry& e = g_table[(start + n) & (kEntries - 1)];
        if (e.ra == ra && e.kind == kind) {
            return &e;
        }
        if (e.ra == 0) {
            if (InterlockedCompareExchange(&e.ra, ra, 0) == 0) {
                e.kind = kind;
                return &e;
            }
            if (e.ra == ra && e.kind == kind) {
                return &e;
            }
        }
    }
    return nullptr;
}

bool recording() {
    return g_intro_state == 1;
}

DWORD effective(DWORD ms) {
    if (g_intro_skipping && ms >= kAccelMin && ms <= kAccelMax) {
        return 1;
    }
    return ms;
}

LONGLONG stamp() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void note(void* ra, LONG kind, DWORD requested, LONGLONG t0) {
    Entry* e = slot_for(reinterpret_cast<LONG>(ra), kind);
    if (!e) {
        return;
    }
    const LONG actual = static_cast<LONG>(1000 * (stamp() - t0) / g_freq.QuadPart);
    InterlockedIncrement(&e->count);
    if (requested == INFINITE) {
        InterlockedIncrement(&e->infinite);
    } else {
        InterlockedExchangeAdd(&e->req_ms, static_cast<LONG>(requested));
        if (static_cast<LONG>(requested) > e->max_req) {
            e->max_req = static_cast<LONG>(requested);
        }
    }
    InterlockedExchangeAdd(&e->actual_ms, actual);
}

VOID WINAPI hook_sleep(DWORD ms) {
    if (!recording()) {
        g_sleep(ms);
        return;
    }
    void* ra = _ReturnAddress();
    const LONGLONG t0 = stamp();
    g_sleep(effective(ms));
    note(ra, 0, ms, t0);
}

DWORD WINAPI hook_wfso(HANDLE h, DWORD ms) {
    if (!recording()) {
        return g_wfso(h, ms);
    }
    void* ra = _ReturnAddress();
    const LONGLONG t0 = stamp();
    const DWORD r = g_wfso(h, effective(ms));
    note(ra, 1, ms, t0);
    return r;
}

DWORD WINAPI hook_wfmo(DWORD n, const HANDLE* handles, BOOL all, DWORD ms) {
    if (!recording()) {
        return g_wfmo(n, handles, all, ms);
    }
    void* ra = _ReturnAddress();
    const LONGLONG t0 = stamp();
    const DWORD r = g_wfmo(n, handles, all, effective(ms));
    note(ra, 2, ms, t0);
    return r;
}

template <typename T>
bool hook_iat(uint32_t slot_va, T detour, T* original, const char* name) {
    auto* slot = reinterpret_cast<void**>(slot_va);
    *original = reinterpret_cast<T>(*slot);
    void* expected = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name));
    if (*slot != expected) {
        LOG_INFO("IAT %s at %08X holds %p (kernel32 export is %p); chaining to whatever is there", name, slot_va,
                 *slot, expected);
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG_ERROR("Could not unprotect the %s import (Win32=%lu)", name, GetLastError());
        return false;
    }
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(detour));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

}  // namespace

bool waits_install() {
    QueryPerformanceFrequency(&g_freq);
    const bool a = hook_iat(kIatSleep, &hook_sleep, &g_sleep, "Sleep");
    const bool b = hook_iat(kIatWfso, &hook_wfso, &g_wfso, "WaitForSingleObject");
    const bool c = hook_iat(kIatWfmo, &hook_wfmo, &g_wfmo, "WaitForMultipleObjects");
    LOG_INFO("Wait hooks installed: Sleep=%d WaitForSingleObject=%d WaitForMultipleObjects=%d", a, b, c);
    return a && b && c;
}

void waits_report() {
    static const char* const kNames[3] = {"Sleep", "WaitForSingleObject", "WaitForMultipleObjects"};
    // Busiest first by measured wait time.
    bool used[kEntries] = {};
    LOG_INFO("Waits recorded during the intro run (call site RVA = return address - 0x400000):");
    for (int rank = 0; rank < 10; ++rank) {
        int best = -1;
        for (int i = 0; i < kEntries; ++i) {
            if (g_table[i].ra != 0 && !used[i] && (best < 0 || g_table[i].actual_ms > g_table[best].actual_ms)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        used[best] = true;
        const Entry& e = g_table[best];
        LOG_INFO("  %-22s from %08lX: calls=%ld, requested avg %.1f ms (max %ld, %ld infinite), measured avg %.1f ms, "
                 "total %ld ms",
                 kNames[e.kind], static_cast<unsigned long>(static_cast<ULONG>(e.ra)), e.count,
                 e.count > e.infinite ? static_cast<double>(e.req_ms) / (e.count - e.infinite) : 0.0, e.max_req,
                 e.infinite, e.count ? static_cast<double>(e.actual_ms) / e.count : 0.0, e.actual_ms);
    }
}
