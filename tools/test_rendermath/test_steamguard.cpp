// SteamPauseGuard against fake_steam_api.dll (built as steam_api.dll next to this exe): a game loop at ~225 FPS calls
// SteamAPI_RunCallbacks each frame unless the guard's gate is closed. Counts the frames whose call still waited for a
// pause (over 4 ms), before and after the guard learned the rhythm. Pass: none in the last 30 s.
#include <windows.h>

#include <cstdio>

#include "log.h"
#include "steamguard.h"

#pragma comment(lib, "winmm.lib")

int main() {
    timeBeginPeriod(1);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    log_init(self, L"test_steamguard.log");
    HMODULE api = LoadLibraryW(L"steam_api.dll");
    if (!api) { std::printf("no steam_api.dll\n"); return 2; }
    auto run = reinterpret_cast<void(__cdecl*)()>(GetProcAddress(api, "SteamAPI_RunCallbacks"));
    steamguard_start(true, false);
    LARGE_INTEGER f, t0, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int frames = 0, skipped = 0, hits_early = 0, hits_late = 0;
    double worst_late = 0;
    for (;;) {
        QueryPerformanceCounter(&a);
        const double now = double(a.QuadPart - t0.QuadPart) / f.QuadPart;
        if (now > 50.0) break;
        ++frames;
        if (steamguard_closed()) {
            ++skipped;
        } else {
            run();
            QueryPerformanceCounter(&b);
            const double ms = 1000.0 * double(b.QuadPart - a.QuadPart) / f.QuadPart;
            steamguard_note(ms, b.QuadPart);
            if (ms > 4.0) {
                std::printf("  %.2f s: a frame's call waited %.1f ms\n", now, ms);
                if (now < 20.0) ++hits_early; else { ++hits_late; if (ms > worst_late) worst_late = ms; }
            }
        }
        // ~225 FPS
        LARGE_INTEGER c;
        do {
            Sleep(0);
            QueryPerformanceCounter(&c);
        } while (double(c.QuadPart - a.QuadPart) / f.QuadPart < 1.0 / 225.0);
    }
    std::printf("%d frames, %d calls skipped by the guard; frames that waited: %d in the first 20 s, %d after "
                "(worst %.1f ms)\n", frames, skipped, hits_early, hits_late, worst_late);
    std::printf("%s\n", hits_late == 0 ? "PASS" : "FAIL");
    return hits_late == 0 ? 0 : 1;
}
