// SteamPauseGuard against fake_steam_api.dll (built as steam_api.dll next to this exe). A game loop at ~225 FPS does
// what the game does every frame: SteamAPI_RunCallbacks (through the guard, as the FPS Unlock's import hook does) and
// the matchmaking task's AddRequestLobbyListDistanceFilter(3) (through the vtable copy the guard patches), and every
// 7 s a lobby search (RequestLobbyList, which must still see the filter). Counts the frames whose Steam calls waited
// (over 4 ms) and the gaps between delivered callbacks. Pass: at most one frame waited in 50 s (a pause has to start
// in the 1.5 ms after a quick answer to catch one), the filter reached Steam once per search with its value, and no
// gap between callbacks over 200 ms.
#include <windows.h>

#include <cstdio>
#include <cstring>

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
    auto get_mm = reinterpret_cast<void*(__cdecl*)()>(GetProcAddress(api, "SteamMatchmaking"));
    auto counts = reinterpret_cast<void(__cdecl*)(long*, long*, long*)>(GetProcAddress(api, "FakeSteamCounts"));
    // The vtable copy the co-op build puts on Steam's matchmaking object, with the guard's distance filter.
    void* mm = get_mm();
    void** vt = *static_cast<void***>(mm);
    static void* copy[38];
    std::memcpy(copy, vt, sizeof(copy));
    using RequestFn = uint64_t(__fastcall*)(void*, void*);
    const RequestFn request_orig = reinterpret_cast<RequestFn>(vt[4]);
    const bool no_guard = GetEnvironmentVariableA("NO_GUARD", nullptr, 0) > 0;  // the same loop unguarded, for contrast
    if (!no_guard) steamguard_take_distance_filter(copy);
    *static_cast<void***>(mm) = copy;
    using DistanceFn = void(__fastcall*)(void*, void*, int);

    if (!no_guard) steamguard_start(true, false);
    LARGE_INTEGER f, t0, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int frames = 0, waited = 0, searches = 0;
    double worst = 0, last_cb = 0, worst_gap = 0;
    int64_t last_call = 0;
    double next_search = 7.0;
    for (;;) {
        QueryPerformanceCounter(&a);
        const double now = double(a.QuadPart - t0.QuadPart) / f.QuadPart;
        if (now > 50.0) break;
        ++frames;
        if (steamguard_frame_call(&last_call)) {
            run();
            if (now > 3.0 && last_cb > 0 && now - last_cb > worst_gap) worst_gap = now - last_cb;
            last_cb = now;
        }
        reinterpret_cast<DistanceFn>(copy[9])(mm, nullptr, 3);
        if (now >= next_search) {
            next_search += 7.0;
            ++searches;
            steamguard_before_lobby_request(mm);
            request_orig(mm, nullptr);
        }
        QueryPerformanceCounter(&b);
        const double ms = 1000.0 * double(b.QuadPart - a.QuadPart) / f.QuadPart;
        if (ms > 4.0 && now > 2.0) {
            ++waited;
            worst = ms > worst ? ms : worst;
            std::printf("  %.2f s: the frame's Steam calls waited %.1f ms\n", now, ms);
        }
        LARGE_INTEGER c;
        do {
            Sleep(0);
            QueryPerformanceCounter(&c);
        } while (double(c.QuadPart - a.QuadPart) / f.QuadPart < 1.0 / 225.0);
    }
    long distance = 0, requests = 0, last_distance = 0;
    counts(&distance, &requests, &last_distance);
    std::printf("%d frames: %d waited (worst %.1f ms); longest gap between callbacks %.0f ms; %d searches, the "
                "distance filter reached Steam %ld times (last value %ld) for %ld requests\n", frames, waited, worst,
                worst_gap * 1000.0, searches, distance, last_distance, requests);
    const bool pass = waited <= 1 && distance == requests && requests == searches && last_distance == 3 &&
                      worst_gap < 0.2;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
