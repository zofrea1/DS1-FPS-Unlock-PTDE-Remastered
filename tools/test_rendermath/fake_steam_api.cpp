// A stand-in steam_api.dll for test_steamguard: Steam's answers pause 15 ms every 5.00-5.05 s (as the Windows client's
// Wi-Fi check does); a call made during a pause waits until it ends. Exports SteamUtils (an object whose 4th virtual
// is GetServerRealTime) and SteamAPI_RunCallbacks.
#include <windows.h>

#include <cstdint>

namespace {

LONGLONG g_freq = 0, g_t0 = 0;

double now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0) / g_freq;
}

// The pause containing time t, if any: [start, start + 15 ms).
bool pause_at(double t, double* end) {
    double s = 1.3;
    unsigned seed = 12345;
    while (s <= t + 0.1) {
        if (t >= s && t < s + 0.015) {
            *end = s + 0.015;
            return true;
        }
        seed = seed * 1103515245u + 12345u;
        s += 5.0 + 0.05 * ((seed >> 8) % 1000) / 1000.0;
    }
    return false;
}

void answer() {
    double end = 0;
    if (pause_at(now_s(), &end)) {
        while (now_s() < end) Sleep(0);
    }
}

struct Utils {
    virtual uint32_t SecondsSinceAppActive() { answer(); return 0; }
    virtual uint32_t SecondsSinceComputerActive() { answer(); return 0; }
    virtual int ConnectedUniverse() { answer(); return 1; }
    virtual uint32_t ServerRealTime() { answer(); return 1; }
} g_utils;

}  // namespace

extern "C" __declspec(dllexport) void* __cdecl SteamUtils() { return &g_utils; }
extern "C" __declspec(dllexport) void __cdecl SteamAPI_RunCallbacks() { answer(); }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        g_freq = f.QuadPart;
        g_t0 = t.QuadPart;
    }
    return TRUE;
}
