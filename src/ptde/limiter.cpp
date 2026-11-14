#include "limiter.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// The schedule is origin + index * frequency / fps, so the cap does not drift. g_fps, the schedule and the timer
// belong to the thread that waits (0 while the limiter is off); another thread only posts a new rate in g_requested,
// which the waiting thread takes up at its next wait. g_freq is set once, before any hook can wait.
int g_fps = 0;
volatile LONG g_requested = 0;
volatile LONG g_point = kLimitEarly;
bool g_armed = true;  // render thread: set at each frame boundary, so the frame's first Present waits once
LONGLONG g_freq = 0;
LONGLONG g_origin = 0;
LONGLONG g_index = 0;
LONGLONG g_wait_ticks = 0;
// limiter_take_slot: the waiting thread puts, the render thread takes.
constexpr LONG kSlots = 8;
LONGLONG g_slots[kSlots];
volatile LONG g_slot_put = 0, g_slot_take = 0;

void put_slot(LONGLONG t) {
    const LONG put = g_slot_put;
    if (put - g_slot_take >= kSlots) return;  // nobody takes them (no boundary hook): keep the oldest
    g_slots[put % kSlots] = t;
    InterlockedExchange(&g_slot_put, put + 1);
}
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
    const int requested = static_cast<int>(g_requested);
    if (requested != g_fps) {
        g_fps = requested;
        g_index = 0;  // a fresh schedule from this frame
        if (g_fps > 0) {
            LOG_INFO("FPS cap: now %d (%.3f ms)", g_fps, 1000.0 / static_cast<double>(g_fps));
        }
    }
    if (g_fps <= 0 || g_freq <= 0) {
        return;
    }
    ensure_timer();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_index == 0) {
        g_origin = now.QuadPart;
        g_index = 1;
        put_slot(now.QuadPart);
        return;
    }
    const LONGLONG target = g_origin + (g_index * g_freq) / g_fps;
    // on the schedule, or (more than half a frame late) when it really goes
    put_slot(now.QuadPart - target > g_freq / (2 * g_fps) ? now.QuadPart : target);
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
    if (fps <= 0 || g_freq > 0) {
        return;
    }
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        LOG_ERROR("FPS cap: no performance counter, the cap is off");
        return;
    }
    g_freq = freq.QuadPart;
    g_fps = fps;
    g_requested = fps;
    LOG_INFO("FPS cap: %d (%.3f ms)", g_fps, 1000.0 / static_cast<double>(g_fps));
}

void limiter_set_fps(int fps) {
    InterlockedExchange(&g_requested, fps > 0 ? fps : 0);
}

int limiter_requested_fps() {
    return static_cast<int>(g_requested);
}

void limiter_set_point(LimitPoint point) {
    InterlockedExchange(&g_point, point);
}

LimitPoint limiter_point() {
    return static_cast<LimitPoint>(g_point);
}

bool limiter_is_on() {
    return g_freq > 0 && g_requested > 0;
}

void limiter_wait_early() {
    if (g_point == kLimitEarly) {
        wait_for_slot();
    }
}

void limiter_wait_present() {
    // Every Present that shows a new picture comes here; only the frame's first one waits.
    if (g_point == kLimitPresent && g_armed) {
        g_armed = false;
        wait_for_slot();
    }
}

void limiter_frame_boundary() {
    if (g_point == kLimitBoundary) {
        wait_for_slot();
    }
    g_armed = true;
}

bool limiter_take_slot(long long* out) {
    const LONG take = g_slot_take;
    const LONG put = InterlockedCompareExchange(&g_slot_put, 0, 0);
    if (put == take) return false;
    *out = g_slots[take % kSlots];
    InterlockedExchange(&g_slot_take, take + 1);
    return true;
}

double limiter_take_wait_ms() {
    const LONGLONG ticks = InterlockedExchange64(&g_wait_ticks, 0);
    if (ticks <= 0 || g_freq <= 0) {
        return 0.0;
    }
    return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq);
}
