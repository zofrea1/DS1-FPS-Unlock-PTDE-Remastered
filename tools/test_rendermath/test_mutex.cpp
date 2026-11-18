// FastMutexes (src/ptde/mutex.cpp) on its own, no game image needed:
//  1. the same single- and two-thread sequences (recursion, tries, timeouts, an unlock by another thread) on a
//     mutex of ours and on one holding a real kernel mutex (the original's path): every return value must match;
//  2. a stress run: threads taking ours (infinite and finite timeouts, tries, recursion) around a shared check that
//     no two threads are ever inside at once, and that nothing deadlocks.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);

bool cpu_translated(const char** name) {
    if (name) *name = "test";
    return false;
}

#include "mutex.cpp"

static int g_fail = 0;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            ++g_fail;                         \
            std::printf("FAIL %s: ", #c);     \
            std::printf(__VA_ARGS__);         \
            std::printf("\n");                \
        }                                     \
    } while (0)

static GameMutex make(bool fast) {
    GameMutex m{};
    m.vtable = nullptr;
    m.handle = reinterpret_cast<uintptr_t>(fast ? fast_create(nullptr, FALSE, nullptr) : CreateMutexW(nullptr, FALSE, nullptr));
    return m;
}

// One scripted sequence; returns the results in order.
static std::vector<int> script(bool fast) {
    std::vector<int> r;
    GameMutex m = make(fast);
    r.push_back(lock(&m, nullptr, INFINITE));      // 0
    r.push_back(lock(&m, nullptr, INFINITE));      // 0 (recursive)
    r.push_back(try_lock(&m));                     // 0 (recursive)
    std::thread([&] {
        r.push_back(try_lock(&m));                 // -2: held by the main thread
        r.push_back(lock(&m, nullptr, 30));        // -3 after 30 ms
        r.push_back(unlock(&m));                   // -4: not the owner
    }).join();
    r.push_back(unlock(&m));                       // 0 (depth 2)
    r.push_back(unlock(&m));                       // 0 (depth 1)
    r.push_back(unlock(&m));                       // 0 (free)
    r.push_back(unlock(&m));                       // -4: nobody holds it
    std::thread([&] {
        r.push_back(lock(&m, nullptr, 30));        // 0: free
        r.push_back(unlock(&m));                   // 0
    }).join();
    fast_close(reinterpret_cast<HANDLE>(m.handle));
    return r;
}

int main() {
    timeBeginPeriod(1);
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    g_wait_on_address = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(kb, "WaitOnAddress"));
    g_wake_single = reinterpret_cast<WakeByAddressFn>(GetProcAddress(kb, "WakeByAddressSingle"));
    if (!g_wait_on_address || !g_wake_single) {
        std::printf("no WaitOnAddress\n");
        return 2;
    }
    const std::vector<int> a = script(false), b = script(true);
    CHECK(a == b, "kernel and fast sequences differ");
    std::printf("sequence (kernel):");
    for (int v : a) std::printf(" %d", v);
    std::printf("\nsequence (fast):  ");
    for (int v : b) std::printf(" %d", v);
    std::printf("\n");

    // Stress: 8 threads, each 300k rounds on one fast mutex.
    GameMutex m = make(true);
    std::atomic<int> inside{0};
    long long counter = 0;
    std::atomic<long long> expect{0};
    std::atomic<int> timeouts{0}, tries_failed{0};
    const auto t0 = GetTickCount64();
    std::vector<std::thread> th;
    for (int t = 0; t < 8; ++t) {
        th.emplace_back([&, t] {
            std::mt19937 rng(1234 + t);
            for (int i = 0; i < 300000; ++i) {
                const int kind = rng() % 16;
                int r;
                if (kind == 0) r = try_lock(&m);
                else if (kind == 1) r = lock(&m, nullptr, 1 + rng() % 3);
                else r = lock(&m, nullptr, INFINITE);
                if (r != 0) {
                    if (r == -2) ++tries_failed;
                    else if (r == -3) ++timeouts;
                    else CHECK(false, "lock returned %d", r);
                    continue;
                }
                const int depth = (rng() % 8 == 0) ? 2 : 1;
                if (depth == 2) CHECK(lock(&m, nullptr, INFINITE) == 0, "recursive lock");
                CHECK(inside.fetch_add(1) == 0, "two threads inside");
                ++counter;
                ++expect;
                if (rng() % 64 == 0) Sleep(0);
                inside.fetch_sub(1);
                for (int d = 0; d < depth; ++d) CHECK(unlock(&m) == 0, "unlock");
            }
        });
    }
    for (auto& x : th) x.join();
    CHECK(counter == expect.load(), "counter %lld vs %lld", counter, expect.load());
    std::printf("stress: %lld rounds inside, %d timeouts, %d failed tries, %llu ms\n", counter, timeouts.load(),
                tries_failed.load(), GetTickCount64() - t0);
    fast_close(reinterpret_cast<HANDLE>(m.handle));

    // The cost of one uncontended lock and unlock, ours against the kernel's.
    for (int fast = 0; fast < 2; ++fast) {
        GameMutex k = make(fast != 0);
        LARGE_INTEGER f, s, e;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&s);
        for (int i = 0; i < 200000; ++i) {
            lock(&k, nullptr, INFINITE);
            unlock(&k);
        }
        QueryPerformanceCounter(&e);
        std::printf("%s: %.1f ns a lock and unlock\n", fast ? "fast  " : "kernel", (e.QuadPart - s.QuadPart) * 1e9 / f.QuadPart / 200000);
        fast_close(reinterpret_cast<HANDLE>(k.handle));
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
