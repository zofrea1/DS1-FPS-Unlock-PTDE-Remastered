// test_eventwait: src/common/eventwait.h against the original game's countdown.
//
// The original: a wait of w seconds is counted down by (float)(1/30) per frame (the difference rounded to float, as
// the game's test does) and ends on the frame it reaches zero or below. The fix must end each wait on the frame
// nearest to that moment at any frame rate (with even and uneven frame times), on exactly the original's frame at
// 30 FPS, keep long waits from drifting, and tell a new wait at a reused address from an old one.
#include "../../src/common/eventwait.h"

#include <cmath>
#include <cstdio>
#include <random>

namespace {

int g_fail = 0;

void check(bool ok, const char* what, double a, double b) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s: %.6f vs %.6f\n", what, a, b);
    }
}

// The original's length of a wait, in seconds of its game time.
double original_length(float w) {
    float t = w;
    int n = 0;
    do {
        t = static_cast<float>(static_cast<double>(t) - static_cast<double>(static_cast<float>(1.0 / 30.0)));
        ++n;
    } while (t > 0.0f);
    return n / 30.0;
}

// Runs one wait through eventwait::step at frame times from `next_dt`; returns the game time at its end.
template <typename F>
double run(const void* key, float w, double rate, F next_dt, uint64_t* clock_ms, int* frames) {
    float remaining = w;
    double t = 0.0;
    *frames = 0;
    for (;;) {
        const float dt = next_dt();
        t += dt;
        ++*frames;
        *clock_ms += static_cast<uint64_t>(dt * 1000.0);
        if (eventwait::step(key, &remaining, dt, rate, *clock_ms)) return t;
        if (*frames > 20000000) return -1.0;
    }
}

}  // namespace

int main() {
    const float waits[] = {0.0f, 0.1f, 0.2f, 0.3f, 0.5f, 0.7f, 1.0f, 1.5f, 2.0f, 2.8f, 3.0f, 5.0f, 30.0f, 60.0f, 600.0f};
    const double fps[] = {30, 60, 72, 90, 120, 144, 150, 165, 240, 360, 1000};
    uint64_t clock_ms = 1000;
    int key_n = 0;
    static char keys[4096];
    double worst = 0.0;
    for (float w : waits) {
        const double want = original_length(w);
        for (double f : fps) {
            const double dt = 1.0 / f;
            int frames = 0;
            const double got = run(&keys[(key_n++) % 4096], w, 30.0, [&] { return static_cast<float>(dt); }, &clock_ms,
                                   &frames);
            // the frame nearest to the original's end: within half a frame (plus float rounding)
            check(std::fabs(got - want) <= 0.5 * dt + 1e-6, "even frames", got, want);
            if (std::fabs(got - want) > worst) worst = std::fabs(got - want);
            if (f == 30.0) check(std::lround(got * 30.0) == std::lround(want * 30.0), "30 FPS frame count", got, want);
        }
        // uneven frame times (+-25% around 144 FPS): within one frame of the original's end
        std::mt19937 rng(1234);
        std::uniform_real_distribution<double> jit(0.75, 1.25);
        int frames = 0;
        const double got = run(&keys[(key_n++) % 4096], w, 30.0,
                               [&] { return static_cast<float>(jit(rng) / 144.0); }, &clock_ms, &frames);
        check(std::fabs(got - want) <= 1.25 / 144.0 + 1e-6, "uneven frames", got, want);
        std::printf("wait %8.3f s: original %10.4f s (%7d frames at 30) | 240 FPS: %10.4f s\n", w, want,
                    static_cast<int>(std::lround(want * 30.0)),
                    [&] {
                        int fr = 0;
                        return run(&keys[(key_n++) % 4096], w, 30.0, [] { return static_cast<float>(1.0 / 240.0); },
                                   &clock_ms, &fr);
                    }());
    }
    // A new wait at the address of an old one: counted from its own start, not the old one's.
    {
        static char k;
        float remaining = 2.0f;
        for (int i = 0; i < 30; ++i) eventwait::step(&k, &remaining, 1.0f / 144.0f, 30.0, clock_ms += 7);
        int frames = 0;
        const double got = run(&k, 0.5f, 30.0, [] { return static_cast<float>(1.0 / 144.0); }, &clock_ms, &frames);
        check(std::fabs(got - original_length(0.5f)) <= 0.5 / 144.0 + 1e-6, "reused address", got,
              original_length(0.5f));
    }
    // Remastered at 60: its own countdown (float steps of 1/60).
    {
        static char k;
        int frames = 0;
        const double got = run(&k, 0.5f, 60.0, [] { return static_cast<float>(1.0 / 60.0); }, &clock_ms, &frames);
        check(frames == static_cast<int>(eventwait::native_frames(0.5f, 60.0)), "60 FPS native frames", frames,
              eventwait::native_frames(0.5f, 60.0));
    }
    std::printf("worst difference from the original's end at even frame rates: %.4f s\n", worst);
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
