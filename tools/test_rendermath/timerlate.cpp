// The FPS cap's wait (limiter.cpp sleep_until) on its own, outside the game: timerlate.exe [fps] [seconds] [mode]
// mode: timer (high-resolution waitable timer + timeout, as the cap does), sleep (Sleep(1) loop), spin (no sleeping).
// Prints every frame that ends more than 2 ms after its slot, with the time since the previous late one, so a
// periodic delay from Windows or a driver shows as a steady gap (the game showed one every 5.04 s).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "winmm.lib")

static LONGLONG g_freq;
static HANDLE g_timer;

static LONGLONG now_ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void sleep_until(LONGLONG target, int mode) {
    LONGLONG now = now_ticks();
    const LONGLONG spin = g_freq / 2500;
    while (mode != 2 && now + spin < target) {
        const LONGLONG remain = target - now - spin;
        if (mode == 0) {
            LARGE_INTEGER due{};
            due.QuadPart = -(remain * 10000000LL / g_freq);
            if (due.QuadPart < 0 && SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) {
                DWORD ms = static_cast<DWORD>(remain * 1000 / g_freq) + 2;
                if (ms < 2) ms = 2;
                WaitForSingleObject(g_timer, ms);
            }
        } else {
            Sleep(1);
        }
        now = now_ticks();
    }
    while (now < target) {
        YieldProcessor();
        now = now_ticks();
    }
}

int main(int argc, char** argv) {
    const int fps = argc > 1 ? std::atoi(argv[1]) : 225;
    const double seconds = argc > 2 ? std::atof(argv[2]) : 30.0;
    const int mode = argc > 3 ? (std::strcmp(argv[3], "sleep") == 0 ? 1 : std::strcmp(argv[3], "spin") == 0 ? 2 : 0) : 0;
    if (argc > 4) timeBeginPeriod(1);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     SYNCHRONIZE | TIMER_MODIFY_STATE);
    std::printf("%d fps, %.0f s, %s%s, high-resolution timer %s\n", fps, seconds,
                mode == 0 ? "timer" : mode == 1 ? "sleep" : "spin", argc > 4 ? ", timeBeginPeriod(1)" : "",
                g_timer ? "yes" : "no");
    const LONGLONG t0 = now_ticks();
    LONGLONG origin = t0, last_late = t0;
    long long index = 1, late = 0, frames = 0;
    double worst = 0;
    for (;;) {
        const LONGLONG target = origin + index * g_freq / fps;
        sleep_until(target, mode);
        const LONGLONG now = now_ticks();
        const double over = 1000.0 * (now - target) / g_freq;
        ++frames;
        if (over > 2.0) {
            ++late;
            if (over > worst) worst = over;
            std::printf("  %.3f s: %.1f ms late (%.3f s after the last)\n", double(now - t0) / g_freq, over,
                        double(now - last_late) / g_freq);
            last_late = now;
        }
        ++index;
        if (origin + index * g_freq / fps <= now) {
            origin = now;
            index = 1;
        }
        if (now - t0 > seconds * g_freq) break;
    }
    std::printf("%lld frames, %lld late (worst %.1f ms)\n", frames, late, worst);
    return 0;
}
