#include "streamwatch.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#if FPSU_DIAGNOSTICS

namespace {

// The game thread's synchronous jobs: 0x577640 (thiscall, one argument, ret 4) puts a request on a worker's list,
// wakes the worker and waits for the request's own signal with no timeout. While a map streams in, the game thread
// stops there for tens of milliseconds. Each call is timed; the slow ones are logged with the request's class (RTTI
// of its vtable) and the game's return addresses on the stack, and every 10 s a summary by class.
constexpr uint32_t kSubmitWait = 0x00577640;
const uint8_t kPrologue[8] = {0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57};  // push ebx/ebp/esi; mov esi,[esp+10h]; push edi
constexpr double kSlowMs = 2.0;
constexpr int kLogged = 80;

using SubmitFn = void(__fastcall*)(void*, void*, void*);
SubmitFn g_orig = nullptr;
LARGE_INTEGER g_freq{};
std::mutex g_lock;
struct Stat {
    int calls = 0;
    int slow = 0;
    double total = 0.0;
    double worst = 0.0;
};
std::map<uint32_t, Stat> g_stats;  // by the request's vtable
LONGLONG g_next = 0;
int g_logged = 0;

void class_name(uint32_t vt, char* out, size_t n) {
    out[0] = 0;
    __try {
        const uint32_t col = *reinterpret_cast<const uint32_t*>(vt - 4);
        const uint32_t td = *reinterpret_cast<const uint32_t*>(col + 12);
        const char* name = reinterpret_cast<const char*>(td + 8);
        size_t i = 0;
        const char* p = name[0] == '.' && name[1] == '?' && name[2] == 'A' ? name + 4 : name;
        for (; p[i] && i + 1 < n && i < 120; ++i) out[i] = p[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, n, "?");
    }
}

// Return addresses into the game's code on the stack above `sp` (a call right before each).
int game_stack(const uint32_t* sp, uint32_t* out, int max) {
    int k = 0;
    __try {
        for (int i = 0; i < 400 && k < max; ++i) {
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

uint32_t vtable_of(const void* p) {
    __try {
        return p ? *static_cast<const uint32_t*>(p) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void __fastcall hk_submit(void* self, void* edx, void* req) {
    const uint32_t vt = vtable_of(req);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    g_orig(self, edx, req);
    QueryPerformanceCounter(&b);
    const double ms = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(g_freq.QuadPart);
    char line[640] = "";
    char summary[2048] = "";
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Stat& s = g_stats[vt];
        ++s.calls;
        s.total += ms;
        if (ms > s.worst) s.worst = ms;
        if (ms >= kSlowMs) {
            ++s.slow;
            if (g_logged < kLogged) {
                ++g_logged;
                char cls[128];
                class_name(vt, cls, sizeof(cls));
                uint32_t st[10];
                const int n = game_stack(static_cast<const uint32_t*>(_AddressOfReturnAddress()), st, 10);
                int o = std::snprintf(line, sizeof(line), "[stream] game thread waited %.1f ms for job %s (vt %08X, thread %lu); stack",
                                      ms, cls, vt, GetCurrentThreadId());
                for (int i = 0; i < n && o > 0 && o < static_cast<int>(sizeof(line)) - 12; ++i) {
                    o += std::snprintf(line + o, sizeof(line) - o, " %08X", st[i]);
                }
            }
        }
        if (!g_next) g_next = b.QuadPart + g_freq.QuadPart * 10;
        if (b.QuadPart >= g_next) {
            g_next = b.QuadPart + g_freq.QuadPart * 10;
            int o = std::snprintf(summary, sizeof(summary), "[stream] last 10 s by job:");
            for (auto& kv : g_stats) {
                if (!kv.second.calls) continue;
                char cls[128];
                class_name(kv.first, cls, sizeof(cls));
                o += std::snprintf(summary + o, sizeof(summary) - o, " %s %d calls %.1f ms (%d slow, worst %.1f);", cls,
                                   kv.second.calls, kv.second.total, kv.second.slow, kv.second.worst);
                if (o >= static_cast<int>(sizeof(summary)) - 1) break;
                kv.second = Stat{};
            }
        }
    }
    if (line[0]) LOG_INFO("%s", line);
    if (summary[0]) LOG_INFO("%s", summary);
}

}  // namespace

void streamwatch_install() {
    auto* site = reinterpret_cast<uint8_t*>(kSubmitWait);
    if (g_orig || std::memcmp(site, kPrologue, sizeof(kPrologue)) != 0) {
        if (!g_orig) LOG_ERROR("[stream] the job submit at %08X does not match this build", kSubmitWait);
        return;
    }
    QueryPerformanceFrequency(&g_freq);
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return;
    std::memcpy(tramp, kPrologue, sizeof(kPrologue));
    tramp[8] = 0xE9;
    const int32_t back = static_cast<int32_t>((kSubmitWait + 8) - (reinterpret_cast<uint32_t>(tramp) + 13));
    std::memcpy(tramp + 9, &back, 4);
    g_orig = reinterpret_cast<SubmitFn>(tramp);
    DWORD old = 0;
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) return;
    uint8_t jmp[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t to = static_cast<int32_t>(reinterpret_cast<uint32_t>(&hk_submit) - (kSubmitWait + 5));
    std::memcpy(jmp + 1, &to, 4);
    std::memcpy(site, jmp, 8);
    VirtualProtect(site, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    LOG_INFO("[stream] watching the game thread's synchronous jobs (0x577640): waits of %.0f ms or more are logged",
             kSlowMs);
}

#else

void streamwatch_install() {}

#endif  // FPSU_DIAGNOSTICS
