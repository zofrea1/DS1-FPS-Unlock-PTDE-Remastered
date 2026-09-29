#include "watch.h"

#include "log.h"
#include "state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace {

constexpr int kSlots = 4;
constexpr int kCandidates = 64;
constexpr uint32_t kArmCalls = 60;
constexpr float kBigStep = 0.05f;
constexpr int kMaxLines = 40000;

struct Candidate {
    const void* chr = nullptr;
    void* phys = nullptr;
    uint32_t calls = 0;
};

Candidate g_cand[kCandidates];
std::atomic<int> g_lock{0};

struct Slot {
    std::atomic<void*> addr{nullptr};
    const void* chr = nullptr;
    const uint8_t* proxy = nullptr;
    volatile float last = 0.0f;
};

Slot g_slot[kSlots];
std::atomic<int> g_armed{0};
std::atomic<bool> g_run{false};
FILE* g_out = nullptr;
std::atomic<int> g_lines{0};
HANDLE g_thread = nullptr;
PVOID g_veh = nullptr;
ULONGLONG g_start_ms = 0;

void spin(std::atomic<int>& l) {
    while (l.exchange(1, std::memory_order_acquire) != 0) {
        YieldProcessor();
    }
}

void unspin(std::atomic<int>& l) {
    l.store(0, std::memory_order_release);
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    const DWORD64 dr6 = ctx->Dr6;
    if ((dr6 & 0xF) == 0) {
        // A watchpoint trap can be delivered with DR6 cleared when it races the
        // register update done from the re-arm thread. If one of our slots is
        // enabled on this thread and nobody is single-stepping (TF clear), it is
        // ours: swallow it. Passing it on gets it reported as a crash.
        if ((ctx->Dr7 & 0x55) != 0 && (ctx->EFlags & 0x100) == 0) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    for (int i = 0; i < kSlots; ++i) {
        if (!(dr6 & (1ull << i))) {
            continue;
        }
        void* addr = g_slot[i].addr.load(std::memory_order_relaxed);
        if (!addr) {
            continue;
        }
        const float now = *static_cast<volatile float*>(addr);
        const float before = g_slot[i].last;
        g_slot[i].last = now;
        if (std::fabs(now - before) < kBigStep || !g_out || g_lines.load() >= kMaxLines) {
            continue;
        }
        g_lines.fetch_add(1);
        const auto image = reinterpret_cast<uintptr_t>(g_image);
        float pvy = 0.0f;
        __try {
            pvy = *reinterpret_cast<const float*>(g_slot[i].proxy + 0x64);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        std::fprintf(g_out, "%llu chr=%p y %.5f -> %.5f (%.5f) proxyvely=%.4f tid=%lu rip=%llX stack:",
                     static_cast<unsigned long long>(GetTickCount64() - g_start_ms), g_slot[i].chr, before, now,
                     now - before, pvy, GetCurrentThreadId(),
                     static_cast<unsigned long long>(ctx->Rip - image));
        const auto* sp = reinterpret_cast<const uintptr_t*>(ctx->Rsp);
        int shown = 0;
        for (int n = 0; n < 160 && shown < 14; ++n) {
            uintptr_t v = 0;
            __try {
                v = sp[n];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (v > image && v < image + 0x3200000) {
                std::fprintf(g_out, " %llX", static_cast<unsigned long long>(v - image));
                ++shown;
            }
        }
        std::fprintf(g_out, "\n");
        std::fflush(g_out);
    }
    ctx->Dr6 = 0;
    ctx->EFlags |= 0x10000;  // resume flag: do not re-trigger on this instruction
    return EXCEPTION_CONTINUE_EXECUTION;
}

void apply_to_all_threads() {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int applied = 0;
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
                continue;
            }
            HANDLE thread = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                                       entry.th32ThreadID);
            if (!thread) {
                continue;
            }
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &ctx)) {
                    DWORD64 dr7 = 0;
                    DWORD64* regs[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
                    for (int i = 0; i < kSlots; ++i) {
                        void* addr = g_slot[i].addr.load(std::memory_order_relaxed);
                        if (!addr) {
                            continue;
                        }
                        *regs[i] = reinterpret_cast<DWORD64>(addr);
                        // enable local i; RW = 01 (write), LEN = 11 (4 bytes)
                        dr7 |= (1ull << (i * 2)) | (1ull << (16 + i * 4)) | (3ull << (18 + i * 4));
                    }
                    ctx.Dr7 = dr7;
                    if (SetThreadContext(thread, &ctx)) {
                        ++applied;
                    }
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    LOG_INFO("Watch: debug registers applied to %d threads", applied);
}

DWORD WINAPI watch_thread(void*) {
    ULONGLONG last_apply_ms = 0;
    while (g_run.load()) {
        Sleep(2000);
        Candidate best[kSlots];
        int found = 0;
        spin(g_lock);
        for (int i = 0; i < kCandidates && found < kSlots; ++i) {
            // Pick the busiest ones (players update every frame at full rate).
            int top = -1;
            for (int j = 0; j < kCandidates; ++j) {
                if (g_cand[j].phys && g_cand[j].calls >= kArmCalls &&
                    (top < 0 || g_cand[j].calls > g_cand[top].calls)) {
                    bool used = false;
                    for (int k = 0; k < found; ++k) {
                        used |= best[k].phys == g_cand[j].phys;
                    }
                    if (!used) {
                        top = j;
                    }
                }
            }
            if (top < 0) {
                break;
            }
            best[found++] = g_cand[top];
        }
        for (auto& c : g_cand) {
            if (c.calls == 0) {
                c.chr = nullptr;  // not updated during the window: gone (new session)
                c.phys = nullptr;
            }
            c.calls = 0;
        }
        unspin(g_lock);
        if (found == 0) {
            continue;
        }
        {
            // Same set of characters (in any order) is nothing to do; and never
            // rewrite debug registers more than once per 10 s.
            bool same = g_armed.load() != 0;
            for (int i = 0; same && i < found; ++i) {
                bool present = false;
                for (int k = 0; k < kSlots; ++k) {
                    present |= g_slot[k].addr.load() != nullptr && g_slot[k].chr == best[i].chr;
                }
                same = present;
            }
            if (same || GetTickCount64() - last_apply_ms < 10000) {
                continue;
            }
        }
        int armed = 0;
        for (int i = 0; i < found; ++i) {
            // phys+0x38 is the character proxy; its phantom's transform
            // translation (see fn 0xA79AC0) is the Havok-side position.
            float* y = nullptr;
            const uint8_t* proxy = nullptr;
            __try {
                proxy = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(best[i].phys) + 0x38);
                const uint8_t* phantom = *reinterpret_cast<uint8_t* const*>(proxy + 0x80);
                const uint8_t* xform = *reinterpret_cast<uint8_t* const*>(phantom + 0x30);
                y = reinterpret_cast<float*>(const_cast<uint8_t*>(xform) + 0x30 + 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                y = nullptr;
            }
            if (!y) {
                continue;
            }
            g_slot[armed].chr = best[i].chr;
            g_slot[armed].proxy = proxy;
            g_slot[armed].last = *y;
            g_slot[armed].addr.store(y);
            LOG_INFO("Watch: slot %d chr=%p proxy=%p phantom y at %p (calls=%u)", armed, best[i].chr, proxy, y,
                     best[i].calls);
            ++armed;
        }
        if (armed == 0) {
            continue;
        }
        for (int i = armed; i < kSlots; ++i) {
            g_slot[i].addr.store(nullptr);
        }
        apply_to_all_threads();
        last_apply_ms = GetTickCount64();
        g_armed.store(1);
    }
    return 0;
}

}  // namespace

bool watch_start(const wchar_t*) {
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) {
        return false;
    }
    wchar_t full[MAX_PATH];
    _snwprintf_s(full, _TRUNCATE, L"%sDSR-FPS-Unlock-watch-%lu.txt", dir,
                 static_cast<unsigned long>(GetCurrentProcessId()));
    g_out = _wfsopen(full, L"w", _SH_DENYNO);
    if (!g_out) {
        return false;
    }
    g_start_ms = GetTickCount64();
    g_veh = AddVectoredExceptionHandler(1, on_exception);
    g_run.store(true);
    g_thread = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
    LOG_INFO("Watch enabled (writes big vertical steps to %%TEMP%%\\DSR-FPS-Unlock-watch-%lu.txt)",
             static_cast<unsigned long>(GetCurrentProcessId()));
    return g_thread != nullptr;
}

void watch_stop() {
    g_run.store(false);
    if (g_out) {
        std::fflush(g_out);
    }
}

void watch_note(const void* chr, void* phys) {
    if (!g_run.load(std::memory_order_relaxed)) {
        return;
    }
    spin(g_lock);
    Candidate* slot = nullptr;
    Candidate* empty = nullptr;
    for (auto& c : g_cand) {
        if (c.chr == chr) {
            slot = &c;
            break;
        }
        if (!c.chr && !empty) {
            empty = &c;
        }
    }
    if (!slot && empty) {
        slot = empty;
        slot->chr = chr;
    }
    if (slot) {
        slot->phys = phys;
        ++slot->calls;
    }
    unspin(g_lock);
}
