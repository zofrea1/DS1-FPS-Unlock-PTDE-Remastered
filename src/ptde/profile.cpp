#include "profile.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "user32.lib")

volatile DWORD g_render_thread_id = 0;

namespace {

constexpr int kSeconds = 15;
constexpr int kMaxSamples = 40000;
constexpr int kBuckets = 4096;

struct Bucket {
    HMODULE module = nullptr;
    uint32_t rva = 0;  // bucket start (rva >> 8 << 8)
    int count = 0;
};

uint32_t g_samples[kMaxSamples];  // raw instruction pointers, collected while nothing else runs
Bucket g_buckets[kBuckets];

void module_name(HMODULE module, char* out, size_t cap) {
    wchar_t path[MAX_PATH] = {};
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) {
        std::snprintf(out, cap, "(unknown)");
        return;
    }
    const wchar_t* slash = wcsrchr(path, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, out, static_cast<int>(cap), nullptr, nullptr);
}

void run_profile() {
    const DWORD tid = g_render_thread_id;
    if (!tid) {
        LOG_ERROR("Profile: the render thread has not been seen yet");
        return;
    }
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) {
        LOG_ERROR("Profile: could not open the render thread (Win32=%lu)", GetLastError());
        return;
    }
    LOG_INFO("Profile: sampling the render thread (%lu) for %d s", tid, kSeconds);
    int n = 0;
    const DWORD end = GetTickCount() + kSeconds * 1000;
    while (GetTickCount() < end && n < kMaxSamples) {
        // Nothing may allocate or log between Suspend and Resume: the thread could hold a lock.
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            if (GetThreadContext(thread, &ctx)) {
                g_samples[n++] = ctx.Eip;
            }
            ResumeThread(thread);
        }
        Sleep(1);
    }
    CloseHandle(thread);

    std::memset(g_buckets, 0, sizeof(g_buckets));
    int used = 0;
    for (int i = 0; i < n; ++i) {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(g_samples[i])), &module);
        const uint32_t rva = module ? (g_samples[i] - reinterpret_cast<uint32_t>(module)) & ~0xFFu : g_samples[i] & ~0xFFu;
        int found = -1;
        for (int b = 0; b < used; ++b) {
            if (g_buckets[b].module == module && g_buckets[b].rva == rva) {
                found = b;
                break;
            }
        }
        if (found < 0 && used < kBuckets) {
            found = used++;
            g_buckets[found].module = module;
            g_buckets[found].rva = rva;
        }
        if (found >= 0) {
            ++g_buckets[found].count;
        }
    }
    if (n == 0) {
        LOG_ERROR("Profile: no samples were taken");
        return;
    }

    // Per-module totals.
    HMODULE seen[64] = {};
    int totals[64] = {};
    int modules = 0;
    for (int b = 0; b < used; ++b) {
        int m = -1;
        for (int i = 0; i < modules; ++i) {
            if (seen[i] == g_buckets[b].module) {
                m = i;
                break;
            }
        }
        if (m < 0 && modules < 64) {
            m = modules++;
            seen[m] = g_buckets[b].module;
        }
        if (m >= 0) {
            totals[m] += g_buckets[b].count;
        }
    }
    LOG_INFO("Profile: %d samples. Time by module:", n);
    bool listed[64] = {};
    for (int rank = 0; rank < 10; ++rank) {
        int best = -1;
        for (int i = 0; i < modules; ++i) {
            if (!listed[i] && (best < 0 || totals[i] > totals[best])) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        listed[best] = true;
        char name[128];
        module_name(seen[best], name, sizeof(name));
        LOG_INFO("  %-32s %5.1f%%  (%d samples)", name, 100.0 * totals[best] / n, totals[best]);
    }

    LOG_INFO("Profile: hottest 256-byte code blocks (module + RVA):");
    bool taken[kBuckets] = {};
    for (int rank = 0; rank < 25; ++rank) {
        int best = -1;
        for (int b = 0; b < used; ++b) {
            if (!taken[b] && (best < 0 || g_buckets[b].count > g_buckets[best].count)) {
                best = b;
            }
        }
        if (best < 0) {
            break;
        }
        taken[best] = true;
        char name[128];
        module_name(g_buckets[best].module, name, sizeof(name));
        LOG_INFO("  %-28s +%08X  %5.1f%%", name, g_buckets[best].rva, 100.0 * g_buckets[best].count / n);
    }
}

DWORD WINAPI profile_thread(void*) {
    bool was_down = false;
    for (;;) {
        Sleep(50);
        const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down && !was_down) {
            run_profile();
        }
        was_down = down;
    }
    return 0;
}

}  // namespace

bool profile_start() {
    HANDLE thread = CreateThread(nullptr, 0, profile_thread, nullptr, 0, nullptr);
    if (!thread) {
        return false;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(thread);
    LOG_INFO("Profiler ready: press F10 in a busy scene to sample the render thread for %d s", kSeconds);
    return true;
}
