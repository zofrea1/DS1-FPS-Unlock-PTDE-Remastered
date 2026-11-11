// The profiler's spike watch against a fake game thread: updates of about 1 ms, and every 1.5 s one that spends
// 20 ms in hitch() (called from long_update()). The [spike] lines should name hitch()'s code as self time and the
// call site in long_update() as a place the time was under, with the gaps between them about 1.5 s.
#define FPSU_DIAGNOSTICS 1
#include <windows.h>

#include <cstdio>

#include "profile.cpp"

unsigned long d3dperf_filtered_samplers() { return 0; }
unsigned long d3dperf_seen_samplers() { return 0; }
unsigned long d3dperf_readback_async() { return 0; }
unsigned long d3dperf_readback_waits() { return 0; }
void d3dperf_filter_stats(unsigned long* a, unsigned long* b, unsigned long* c) { *a = *b = *c = 0; }
void d3dperf_constant_stats(unsigned long* a, unsigned long* b, unsigned long* c, unsigned long* d) { *a = *b = *c = *d = 0; }
void surfpool_stats(unsigned long* a, unsigned long* b) { *a = *b = 0; }

volatile double g_sink = 0;

__declspec(noinline) void spin(double ms) {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    do {
        for (int i = 0; i < 1000; ++i) g_sink = g_sink * 1.0000001 + 1.0;
        QueryPerformanceCounter(&b);
    } while (1000.0 * (b.QuadPart - a.QuadPart) / f.QuadPart < ms);
}

__declspec(noinline) void hitch() { spin(20.0); }
__declspec(noinline) void long_update() {
    hitch();
    g_sink += 1.0;
}

int main() {
    timeBeginPeriod(1);
    if (!profile_start()) { std::printf("profile_start failed\n"); return 2; }
    std::printf("hitch() at %p, long_update() at %p\n", static_cast<void*>(&hitch), static_cast<void*>(&long_update));
    LARGE_INTEGER f, t0, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    double next_hitch = 1.5;
    for (;;) {
        QueryPerformanceCounter(&a);
        const double now = static_cast<double>(a.QuadPart - t0.QuadPart) / f.QuadPart;
        if (now > 8.0) break;
        profile_game_frame_begin();
        if (now >= next_hitch) {
            long_update();
            next_hitch += 1.5;
        } else {
            spin(1.0);
        }
        QueryPerformanceCounter(&b);
        profile_game_frame(1000.0 * (b.QuadPart - a.QuadPart) / f.QuadPart);
        Sleep(3);
    }
    Sleep(50);
    return 0;
}
