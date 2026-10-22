#include "limiter.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// 0 while the limiter is off. The schedule is origin + index * frequency / fps, so the cap does not drift.
int g_fps = 0;
bool g_early = true;
LONGLONG g_freq = 0;
LONGLONG g_origin = 0;
LONGLONG g_index = 0;
LONGLONG g_wait_ticks = 0;
HANDLE g_timer = nullptr;
bool g_timer_ready = false;

// Created on the first wait, not from DllMain. The loader lock is held there.
void ensure_timer() {
    if (g_timer_ready) {
        return;
    }
    g_timer_ready = true;
    constexpr DWORD kAccess = SYNCHRONIZE | TIMER_MODIFY_STATE;
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, kAccess);
    if (!g_timer) {
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, kAccess);
    }
    if (!g_timer) {
        LOG_INFO("FPS cap is sleeping without a waitable timer");
    }
}

void sleep_until(LONGLONG target) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    // The timer wakes close to the target. The last fraction of a millisecond is spun so Sleep's quantum cannot
    // push a frame past the cap.
    const LONGLONG spin = g_freq / 2500;
    while (now.QuadPart + spin < target) {
        const LONGLONG remain = target - now.QuadPart - spin;
        bool waited = false;
        if (g_timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -(remain * 10000000LL / g_freq);
            if (due.QuadPart < 0 && SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) {
                DWORD ms = static_cast<DWORD>(remain * 1000 / g_freq) + 2;
                if (ms < 2) {
                    ms = 2;
                } else if (ms > 1000) {
                    ms = 1000;
                }
                WaitForSingleObject(g_timer, ms);
                waited = true;
            }
        }
        if (!waited) {
            const DWORD ms = static_cast<DWORD>(remain * 1000 / g_freq);
            if (ms > 1) {
                Sleep(ms > 15 ? ms - 1 : 1);
            } else {
                break;
            }
        }
        QueryPerformanceCounter(&now);
    }
    while (now.QuadPart < target) {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    }
}

void wait_for_slot() {
    if (g_fps <= 0 || g_freq <= 0) {
        return;
    }
    ensure_timer();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_index == 0) {
        g_origin = now.QuadPart;
        g_index = 1;
        return;
    }
    const LONGLONG target = g_origin + (g_index * g_freq) / g_fps;
    if (now.QuadPart < target) {
        const LONGLONG before = now.QuadPart;
        sleep_until(target);
        QueryPerformanceCounter(&now);
        InterlockedExchangeAdd64(&g_wait_ticks, now.QuadPart - before);
    }
    ++g_index;
    const LONGLONG upcoming = g_origin + (g_index * g_freq) / g_fps;
    // A hitch leaves the old schedule in the past. The following frame starts a fresh interval from now, so the
    // limiter does not skip waits trying to repay the missed time.
    if (upcoming <= now.QuadPart) {
        g_origin = now.QuadPart;
        g_index = 1;
    }
}

}  // namespace

void limiter_start(int fps) {
    if (g_timer) {
        CancelWaitableTimer(g_timer);
        CloseHandle(g_timer);
        g_timer = nullptr;
    }
    g_timer_ready = false;
    g_fps = 0;
    g_freq = 0;
    g_origin = 0;
    g_index = 0;
    g_wait_ticks = 0;
    if (fps <= 0) {
        return;
    }
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        LOG_ERROR("FPS cap: no performance counter, the cap is off");
        return;
    }
    g_freq = freq.QuadPart;
    g_fps = fps;
    LOG_INFO("FPS cap: %d (%.3f ms), %s", g_fps, 1000.0 / static_cast<double>(g_fps),
             g_early ? "before input is read" : "before Present");
}

void limiter_set_early(bool early) {
    g_early = early;
}

bool limiter_is_on() {
    return g_fps > 0;
}

void limiter_wait_early() {
    if (g_early) {
        wait_for_slot();
    }
}

void limiter_wait_present() {
    if (!g_early) {
        wait_for_slot();
    }
}

double limiter_take_wait_ms() {
    const LONGLONG ticks = InterlockedExchange64(&g_wait_ticks, 0);
    if (ticks <= 0 || g_freq <= 0) {
        return 0.0;
    }
    return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq);
}
