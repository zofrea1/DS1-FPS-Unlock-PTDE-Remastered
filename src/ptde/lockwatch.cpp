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
constexpr LONG kMaxLogged = 1500;
constexpr int kHeldMax = 24;

struct Held {
    void* m;
    LONGLONG since;
    uint32_t site;
    int depth;
    ULONGLONG cpu;  // the thread's CPU time (100 ns) when taken, for the watched sites only
};

// The resource loader's worker (0x66C4F0 takes 0x135F628 and 0x135F648, then 0x60B470 the file load event queue's
// lock): its holds also log the holder's own CPU time, which tells work from waiting.
constexpr uint32_t kCpuSites[] = {0x0066C56E, 0x0066C58E, 0x0060B4AC};

ULONGLONG thread_cpu() {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return ((static_cast<ULONGLONG>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
           ((static_cast<ULONGLONG>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
}

bool cpu_site(uint32_t site) {
    for (uint32_t s : kCpuSites) {
        if (s == site) return true;
    }
    return false;
}
struct PerThread {
    Held held[kHeldMax];
    int n;
};
thread_local PerThread t_held;

LARGE_INTEGER g_freq{};
volatile LONG g_logged = 0;
LONGLONG g_min_ticks = 0;

// The loader's current outermost hold of one of its locks (for the sampler thread).
volatile LONG g_hold_tid = 0;
volatile LONG g_hold_seq = 0;
volatile LONGLONG g_hold_since = 0;
volatile uint32_t g_hold_site = 0;

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
    const bool watched = cpu_site(site);
    if (watched) {
        bool outer = true;
        for (int i = 0; i < t.n; ++i) {
            if (t.held[i].cpu) outer = false;
        }
        if (outer) {
            g_hold_since = now;
            g_hold_site = site;
            InterlockedIncrement(&g_hold_seq);
            InterlockedExchange(&g_hold_tid, static_cast<LONG>(GetCurrentThreadId()));
        }
    }
    if (t.n < kHeldMax) t.held[t.n++] = Held{m, now, site, 1, watched ? thread_cpu() : 0};
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
        const ULONGLONG cpu0 = t.held[i].cpu;
        t.held[i] = t.held[--t.n];
        if (cpu0) {
            bool any = false;
            for (int j = 0; j < t.n; ++j) {
                if (t.held[j].cpu) any = true;
            }
            if (!any) InterlockedCompareExchange(&g_hold_tid, 0, static_cast<LONG>(GetCurrentThreadId()));
        }
        if (held >= g_min_ticks && InterlockedIncrement(&g_logged) <= kMaxLogged) {
            char who[32];
            label(GetCurrentThreadId(), who, sizeof(who));
            char cpu[48] = "";
            if (cpu0) std::snprintf(cpu, sizeof(cpu), ", its own cpu %.1f ms", (thread_cpu() - cpu0) / 10000.0);
            LOG_INFO("[lock] %s held mutex %p for %.1f ms (taken at %08X%s)", who, self,
                     1000.0 * static_cast<double>(held) / g_freq.QuadPart, site, cpu);
        }
        break;
    }
    LeaveCriticalSection(cs);
    return 0;
}

// While a thread holds one of the loader's locks (a watched site), its waits are timed: what the loader waits for
// with the game thread queued behind it.
uint32_t loader_site_held() {
    const PerThread& t = t_held;
    for (int i = 0; i < t.n; ++i) {
        if (t.held[i].cpu) return t.held[i].site;
    }
    return 0;
}

int game_stack(const uint32_t* sp, uint32_t* out, int max) {
    int k = 0;
    __try {
        for (int i = 0; i < 300 && k < max; ++i) {
            const uint32_t v = sp[i];
            if (v < 0x00401000 || v > 0x01100000) continue;
            const auto* c = reinterpret_cast<const uint8_t*>(v);
            if (c[-5] == 0xE8 || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) ||
                (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10)) {
                out[k++] = v;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return k;
}

void log_wait(const char* what, uint32_t held_site, double ms, const void* frame) {
    if (InterlockedIncrement(&g_logged) > kMaxLogged) return;
    char who[32];
    label(GetCurrentThreadId(), who, sizeof(who));
    uint32_t st[10];
    const int n = game_stack(static_cast<const uint32_t*>(frame), st, 10);
    char line[400];
    int o = std::snprintf(line, sizeof(line), "[lock] %s waited %.1f ms in %s while holding the loader lock taken at %08X; stack",
                          who, ms, what, held_site);
    for (int i = 0; i < n && o > 0 && o < static_cast<int>(sizeof(line)) - 12; ++i) {
        o += std::snprintf(line + o, sizeof(line) - o, " %08X", st[i]);
    }
    LOG_INFO("%s", line);
}

constexpr double kWaitLogMs = 8.0;
using WaitOneFn = DWORD(WINAPI*)(HANDLE, DWORD);
using WaitManyFn = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD);
using SleepFn = void(WINAPI*)(DWORD);
WaitOneFn g_wait_one = nullptr;
WaitManyFn g_wait_many = nullptr;
SleepFn g_sleep = nullptr;
using EnterCsFn = void(WINAPI*)(LPCRITICAL_SECTION);
EnterCsFn g_enter_cs = nullptr;

void WINAPI hk_enter_cs(LPCRITICAL_SECTION cs) {
    const uint32_t held = loader_site_held();
    if (!held || TryEnterCriticalSection(cs)) {
        if (!held) g_enter_cs(cs);
        return;
    }
    const LONGLONG a = ticks();
    g_enter_cs(cs);
    const double ms = 1000.0 * static_cast<double>(ticks() - a) / g_freq.QuadPart;
    if (ms >= kWaitLogMs) log_wait("EnterCriticalSection", held, ms, _AddressOfReturnAddress());
}

DWORD WINAPI hk_wait_one(HANDLE h, DWORD timeout) {
    const uint32_t held = loader_site_held();
    if (!held) return g_wait_one(h, timeout);
    const LONGLONG a = ticks();
    const DWORD r = g_wait_one(h, timeout);
    const double ms = 1000.0 * static_cast<double>(ticks() - a) / g_freq.QuadPart;
    if (ms >= kWaitLogMs) log_wait("WaitForSingleObject", held, ms, _AddressOfReturnAddress());
    return r;
}

DWORD WINAPI hk_wait_many(DWORD n, const HANDLE* hs, BOOL all, DWORD timeout) {
    const uint32_t held = loader_site_held();
    if (!held) return g_wait_many(n, hs, all, timeout);
    const LONGLONG a = ticks();
    const DWORD r = g_wait_many(n, hs, all, timeout);
    const double ms = 1000.0 * static_cast<double>(ticks() - a) / g_freq.QuadPart;
    if (ms >= kWaitLogMs) log_wait("WaitForMultipleObjects", held, ms, _AddressOfReturnAddress());
    return r;
}

void WINAPI hk_sleep(DWORD ms_in) {
    const uint32_t held = loader_site_held();
    if (!held) return g_sleep(ms_in);
    const LONGLONG a = ticks();
    g_sleep(ms_in);
    const double ms = 1000.0 * static_cast<double>(ticks() - a) / g_freq.QuadPart;
    if (ms >= kWaitLogMs) log_wait("Sleep", held, ms, _AddressOfReturnAddress());
}

void** import_slot(const char* dll, const char* fn) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<void**>(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), fn) == 0) return &slots[i];
        }
    }
    return nullptr;
}

template <typename Fn>
bool wrap(const char* fn, Fn hook, Fn* original) {
    void** slot = import_slot("KERNEL32.dll", fn);
    if (!slot) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = reinterpret_cast<Fn>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(hook));
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Sampler: a hold of the loader's locks that has run 25 ms gets the holder's stack (thread suspended only while its
// registers and stack are copied; names are looked up after it runs again), every 40 ms while it lasts.
constexpr int kStackWords = 3072;
constexpr LONG kMaxSamples = 400;
uint32_t g_copy[kStackWords];

bool code_return(uint32_t v) {
    __try {
        const auto* c = reinterpret_cast<const uint8_t*>(v);
        return c[-5] == 0xE8 || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) ||
               (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10) || (c[-7] == 0xFF && (c[-6] & 0x38) == 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void where(uint32_t v, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(v)), &mod) || !mod) {
        std::snprintf(out, n, "%08X", v);
        return;
    }
    if (mod == GetModuleHandleW(nullptr)) {
        std::snprintf(out, n, "%08X", v);
        return;
    }
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* b = path;
    for (DWORD i = 0; i < len; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') b = path + i + 1;
    }
    std::snprintf(out, n, "%ls+%X", b, v - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mod)));
}

bool in_module(uint32_t v) {
    HMODULE mod = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(v)), &mod) &&
           mod != nullptr;
}

DWORD WINAPI sampler(void*) {
    LONG samples = 0;
    LONG last_seq = 0;
    LONGLONG last_at = 0;
    DWORD cached_tid = 0;
    HANDLE th = nullptr;
    while (samples < kMaxSamples) {
        Sleep(5);
        const LONG tid = g_hold_tid;
        if (!tid) continue;
        const LONG seq = g_hold_seq;
        const LONGLONG since = g_hold_since;
        const LONGLONG now = ticks();
        const double held_ms = 1000.0 * static_cast<double>(now - since) / g_freq.QuadPart;
        if (held_ms < 25.0) continue;
        if (seq == last_seq && 1000.0 * static_cast<double>(now - last_at) / g_freq.QuadPart < 40.0) continue;
        if (static_cast<DWORD>(tid) != cached_tid) {
            if (th) CloseHandle(th);
            th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                            static_cast<DWORD>(tid));
            cached_tid = static_cast<DWORD>(tid);
        }
        if (!th) continue;
        if (SuspendThread(th) == static_cast<DWORD>(-1)) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        int words = 0;
        const bool still = g_hold_tid == tid && g_hold_seq == seq;
        if (still && GetThreadContext(th, &ctx)) {
            __try {
                const auto* sp = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(ctx.Esp));
                for (; words < kStackWords; ++words) g_copy[words] = sp[words];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ResumeThread(th);
        if (!still || !words) continue;
        last_seq = seq;
        last_at = now;
        ++samples;
        char line[1400];
        char w[64];
        where(ctx.Eip, w, sizeof(w));
        int o = std::snprintf(line, sizeof(line), "[lock] loader thread %ld %.0f ms into a hold (taken at %08X): eip %s; stack",
                              tid, held_ms, g_hold_site, w);
        int shown = 0;
        for (int i = 0; i < words && shown < 22 && o < static_cast<int>(sizeof(line)) - 48; ++i) {
            const uint32_t v = g_copy[i];
            if (v < 0x00401000 || !in_module(v) || !code_return(v)) continue;
            where(v, w, sizeof(w));
            o += std::snprintf(line + o, sizeof(line) - o, " %s", w);
            ++shown;
        }
        LOG_INFO("%s", line);
    }
    if (th) CloseHandle(th);
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
        const int waits = wrap("WaitForSingleObject", &hk_wait_one, &g_wait_one) +
                          wrap("WaitForMultipleObjects", &hk_wait_many, &g_wait_many) + wrap("Sleep", &hk_sleep, &g_sleep) +
                          wrap("EnterCriticalSection", &hk_enter_cs, &g_enter_cs);
        if (HANDLE t = CreateThread(nullptr, 0, sampler, nullptr, 0, nullptr)) CloseHandle(t);
        LOG_INFO("[lock] timing the engine's light mutexes (0x567E70): holds and waits of %.0f ms or more are logged; "
                 "the loader's waits while it holds its locks too (%d of 4 imports)",
                 kLogMs, waits);
    } else {
        LOG_ERROR("[lock] the light mutex functions did not match this build (or were partly patched)");
    }
}

#else

void lockwatch_install() {}

#endif
