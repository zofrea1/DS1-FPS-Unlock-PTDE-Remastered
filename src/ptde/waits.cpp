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
constexpr uint32_t kIatQpc = 0x010CC250;
constexpr uint32_t kIatTimeGetTime = 0x010CC334;
constexpr uint32_t kIatGetTickCount = 0x010CC134;

using SleepFn = VOID(WINAPI*)(DWORD);
using WfsoFn = DWORD(WINAPI*)(HANDLE, DWORD);
using WfmoFn = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD);
using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
using TimeFn = DWORD(WINAPI*)();
SleepFn g_sleep = nullptr;
WfsoFn g_wfso = nullptr;
WfmoFn g_wfmo = nullptr;
QpcFn g_qpc = nullptr;
TimeFn g_time = nullptr;
TimeFn g_tick = nullptr;

LARGE_INTEGER g_freq{};

struct Entry {
    volatile LONG ra;  // return address (32-bit image)
    LONG kind;         // 0 Sleep, 1 WaitForSingleObject, 2 WaitForMultipleObjects
    volatile LONG count;
    volatile LONG req_ms;      // sum of requested timeouts (INFINITE excluded)
    volatile LONG actual_ms;   // sum of measured wait durations
    volatile LONG max_req;
    volatile LONG infinite;    // calls with an INFINITE timeout
    volatile LONG min_depth;   // shallowest stack seen (QueryPerformanceCounter / timeGetTime)
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
                e.min_depth = 99;
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

// The intro sleeps in loops until a real-time deadline. Shortening the sleeps alone only makes
// those loops spin faster, so this is paired with the timer warp below (which moves the
// deadline closer on every spin): waits of 20..300 ms become 1 ms while the intro is skipped.
constexpr DWORD kAccelMin = 20;
constexpr DWORD kAccelMax = 300;
DWORD effective(DWORD ms) {
    if (g_intro_skipping && ms >= kAccelMin && ms <= kAccelMax) {
        return 1;
    }
    return ms;
}

// Timer warp: while the intro is skipped, QueryPerformanceCounter callers with a shallow stack
// (the intro's own polling loops, as opposed to deep engine code) see time jump forward by
// 1/50 s per call. The added offset is kept afterwards so time stays monotonic.
volatile LONGLONG g_qpc_warp = 0;      // QueryPerformanceCounter ticks added
volatile LONG g_ms_warp = 0;           // milliseconds added to timeGetTime / GetTickCount
volatile LONG g_warped_qpc = 0;
volatile LONG g_warped_ms = 0;
// Every game-side caller is warped, regardless of stack depth: a depth test decided nothing
// last time and the number of qualifying calls was never logged.

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

void note_time(void* ra, LONG kind, USHORT depth) {
    Entry* e = slot_for(reinterpret_cast<LONG>(ra), kind);
    if (!e) {
        return;
    }
    InterlockedIncrement(&e->count);
    if (static_cast<LONG>(depth) < e->min_depth) {
        e->min_depth = static_cast<LONG>(depth);
    }
}

BOOL WINAPI hook_qpc(LARGE_INTEGER* counter) {
    const BOOL ok = g_qpc(counter);
    if (g_intro_state == 1) {
        void* ra = _ReturnAddress();
        void* frames[8];
        const USHORT depth = CaptureStackBackTrace(1, 8, frames, nullptr);
        note_time(ra, 3, depth);
    }
    return ok;  // recording only: warping is done by the process-wide hook below
}

DWORD WINAPI hook_time() {
    if (g_intro_state == 1) {
        void* frames[8];
        const USHORT depth = CaptureStackBackTrace(1, 8, frames, nullptr);
        note_time(_ReturnAddress(), 4, depth);
    }
    if (g_intro_skipping) {
        InterlockedExchangeAdd(&g_ms_warp, 20);
        InterlockedIncrement(&g_warped_ms);
    }
    return g_time() + static_cast<DWORD>(g_ms_warp);
}

DWORD WINAPI hook_tick() {
    if (g_intro_state == 1) {
        void* frames[8];
        const USHORT depth = CaptureStackBackTrace(1, 8, frames, nullptr);
        note_time(_ReturnAddress(), 5, depth);
    }
    if (g_intro_skipping) {
        InterlockedExchangeAdd(&g_ms_warp, 20);
        InterlockedIncrement(&g_warped_ms);
    }
    return g_tick() + static_cast<DWORD>(g_ms_warp);
}

// ---- process-wide QueryPerformanceCounter hot patch --------------------------------------
constexpr LONGLONG kMaxWarpSeconds = 30;
QpcFn g_qpc_real = nullptr;

BOOL WINAPI hook_qpc_global(LARGE_INTEGER* counter) {
    const BOOL ok = g_qpc_real(counter);
    if (g_intro_skipping) {
        if (InterlockedCompareExchange64(&g_qpc_warp, 0, 0) < kMaxWarpSeconds * g_freq.QuadPart) {
            InterlockedExchangeAdd64(&g_qpc_warp, g_freq.QuadPart / 50);
        }
        InterlockedIncrement(&g_warped_qpc);
    }
    if (ok && counter) {
        counter->QuadPart += InterlockedCompareExchange64(&g_qpc_warp, 0, 0);
    }
    return ok;
}

template <typename T>
bool hook_iat(uint32_t slot_va, T detour, T* original, const wchar_t* module, const char* name) {
    auto* slot = reinterpret_cast<void**>(slot_va);
    *original = reinterpret_cast<T>(*slot);
    void* expected = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(module), name));
    if (*slot != expected) {
        LOG_INFO("IAT %s at %08X holds %p (export is %p); chaining to whatever is there", name, slot_va,
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
    const bool a = hook_iat(kIatSleep, &hook_sleep, &g_sleep, L"kernel32.dll", "Sleep");
    const bool b = hook_iat(kIatWfso, &hook_wfso, &g_wfso, L"kernel32.dll", "WaitForSingleObject");
    const bool c = hook_iat(kIatWfmo, &hook_wfmo, &g_wfmo, L"kernel32.dll", "WaitForMultipleObjects");
    const bool d = hook_iat(kIatQpc, &hook_qpc, &g_qpc, L"kernel32.dll", "QueryPerformanceCounter");
    const bool e = hook_iat(kIatTimeGetTime, &hook_time, &g_time, L"winmm.dll", "timeGetTime");
    const bool f = hook_iat(kIatGetTickCount, &hook_tick, &g_tick, L"kernel32.dll", "GetTickCount");
    LOG_INFO("Wait/time hooks installed: Sleep=%d WaitForSingleObject=%d WaitForMultipleObjects=%d QPC=%d timeGetTime=%d "
             "GetTickCount=%d", a, b, c, d, e, f);
    return a && b && c && d && e && f;
}

void waits_report() {
    static const char* const kNames[6] = {"Sleep", "WaitForSingleObject", "WaitForMultipleObjects", "QueryPerformanceCounter", "timeGetTime", "GetTickCount"};
    // Busiest first by measured wait time.
    bool used[kEntries] = {};
    LOG_INFO("Timer warp during the intro: %ld QueryPerformanceCounter calls warped (+%.1f s), %ld timeGetTime/GetTickCount calls warped (+%ld ms)",
             g_warped_qpc, static_cast<double>(g_qpc_warp) / static_cast<double>(g_freq.QuadPart), g_warped_ms, g_ms_warp);
    LOG_INFO("Waits recorded during the intro run (call site RVA = return address - 0x400000):");
    for (int rank = 0; rank < 16; ++rank) {
        int best = -1;
        for (int i = 0; i < kEntries; ++i) {
            if (g_table[i].ra != 0 && !used[i] &&
                (best < 0 || (g_table[i].kind >= 3 ? g_table[i].count / 4 : g_table[i].actual_ms) >
                                 (g_table[best].kind >= 3 ? g_table[best].count / 4 : g_table[best].actual_ms))) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        used[best] = true;
        const Entry& e = g_table[best];
        if (e.kind >= 3) {
            LOG_INFO("  %-24s from %08lX: calls=%ld, shallowest stack depth %ld", kNames[e.kind],
                     static_cast<unsigned long>(static_cast<ULONG>(e.ra)), e.count, e.min_depth);
            continue;
        }
        LOG_INFO("  %-22s from %08lX: calls=%ld, requested avg %.1f ms (max %ld, %ld infinite), measured avg %.1f ms, "
                 "total %ld ms",
                 kNames[e.kind], static_cast<unsigned long>(static_cast<ULONG>(e.ra)), e.count,
                 e.count > e.infinite ? static_cast<double>(e.req_ms) / (e.count - e.infinite) : 0.0, e.max_req,
                 e.infinite, e.count ? static_cast<double>(e.actual_ms) / e.count : 0.0, e.actual_ms);
    }
}

bool intro_timer_install() {
    QueryPerformanceFrequency(&g_freq);
    auto* target = reinterpret_cast<uint8_t*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryPerformanceCounter"));
    if (!target) {
        LOG_ERROR("Could not find kernel32!QueryPerformanceCounter");
        return false;
    }
    // mov edi,edi / push ebp / mov ebp,esp, with a run of int3 padding in front: the Windows
    // hot-patch layout. A jump into the padding plus a two-byte short jump over the first
    // instruction can be applied while other threads are running the function.
    static const uint8_t kPrologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    static const uint8_t kPadding[5] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0 || std::memcmp(target - 5, kPadding, 5) != 0) {
        LOG_ERROR("kernel32!QueryPerformanceCounter is not in the hot-patch layout (%02X %02X %02X %02X %02X). "
                  "Not hooking it.", target[0], target[1], target[2], target[3], target[4]);
        return false;
    }
    g_qpc_real = reinterpret_cast<QpcFn>(target + 2);  // the mov edi,edi is a no-op, so start after it
    DWORD old = 0;
    if (!VirtualProtect(target - 5, 8, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("Could not unprotect kernel32!QueryPerformanceCounter (Win32=%lu)", GetLastError());
        return false;
    }
    uint8_t jump[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&hook_qpc_global) -
                                              reinterpret_cast<uint32_t>(target));
    std::memcpy(jump + 1, &rel, sizeof(rel));
    std::memcpy(target - 5, jump, sizeof(jump));  // the long jump, into the padding
    FlushInstructionCache(GetCurrentProcess(), target - 5, 5);
    const uint16_t short_jump = 0xF9EB;           // EB F9: jmp -7, back into the padding
    InterlockedExchange16(reinterpret_cast<SHORT*>(target), static_cast<SHORT>(short_jump));
    FlushInstructionCache(GetCurrentProcess(), target, 2);
    DWORD ignored = 0;
    VirtualProtect(target - 5, 8, old, &ignored);
    LOG_INFO("Process-wide QueryPerformanceCounter hook installed at %p (intro warp capped at %lld s)", target,
             kMaxWarpSeconds);
    return true;
}
