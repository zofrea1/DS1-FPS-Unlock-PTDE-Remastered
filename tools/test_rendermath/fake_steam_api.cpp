// A stand-in steam_api.dll for test_steamguard, modelled on the Windows Steam client seen in the game's logs:
//  - every call goes through one lock and Steam answers one at a time;
//  - about every 5.00-5.05 s Steam pauses 15 ms (its Wi-Fi check); a call made meanwhile waits for the end of the
//    pause, and every other caller waits for the lock that call holds;
//  - half way between pauses Steam is sometimes a little slow (5 ms), as the canary of rc6 saw.
// Exports SteamUtils (4th virtual: GetServerRealTime), SteamMatchmaking (38 virtuals; RequestLobbyList is slot 4,
// AddRequestLobbyListDistanceFilter slot 9) and SteamAPI_RunCallbacks. Counts the calls that reached it.
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

LONGLONG g_freq = 0, g_t0 = 0;
CRITICAL_SECTION g_lock;
volatile LONG g_distance_calls = 0, g_request_calls = 0, g_last_distance = -1;

double now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0) / g_freq;
}

// The pause containing time t, if any.
bool pause_at(double t, double* end) {
    double s = 1.3;
    unsigned seed = 12345;
    while (s <= t + 0.1) {
        if (t >= s && t < s + 0.015) {
            *end = s + 0.015;
            return true;
        }
        const double half = s + 2.51;  // the smaller slowdown, every other period
        if ((seed & 0x100) && t >= half && t < half + 0.005) {
            *end = half + 0.005;
            return true;
        }
        seed = seed * 1103515245u + 12345u;
        s += 5.0 + 0.05 * ((seed >> 8) % 1000) / 1000.0;
    }
    return false;
}

void answer() {
    EnterCriticalSection(&g_lock);
    double end = 0;
    if (pause_at(now_s(), &end)) {
        while (now_s() < end) Sleep(0);
    }
    LeaveCriticalSection(&g_lock);
}

struct Utils {
    virtual uint32_t SecondsSinceAppActive() { answer(); return 0; }
    virtual uint32_t SecondsSinceComputerActive() { answer(); return 0; }
    virtual int ConnectedUniverse() { answer(); return 1; }
    virtual uint32_t ServerRealTime() { answer(); return 1; }
} g_utils;

void __fastcall mm_other(void*, void*) { answer(); }
uint64_t __fastcall mm_request(void*, void*) {
    answer();
    InterlockedIncrement(&g_request_calls);
    return 1;
}
void __fastcall mm_distance(void*, void*, int v) {
    answer();
    InterlockedIncrement(&g_distance_calls);
    InterlockedExchange(&g_last_distance, v);
}
void* g_mm_vt[38];
struct Mm {
    void** vt;
} g_mm{g_mm_vt};

// ISteamNetworking005: records what reached Steam, in order (a send's first 4 bytes, or -1 for a channel close).
volatile LONG g_net_log[100000];
volatile LONG g_net_n = 0;
void net_record(LONG v) {
    const LONG i = InterlockedIncrement(&g_net_n) - 1;
    if (i < 100000) g_net_log[i] = v;
}
bool __fastcall net_send(void*, void*, uint64_t, const void* data, uint32_t size, int, int) {
    answer();
    LONG v = 0;
    if (size >= 4) std::memcpy(&v, data, 4);
    net_record(v);
    return true;
}
bool __fastcall net_peer(void*, void*, uint64_t) { answer(); return true; }
bool __fastcall net_close_channel(void*, void*, uint64_t, int) {
    answer();
    net_record(-1);
    return true;
}
void __fastcall net_other(void*, void*) { answer(); }
void* g_net_vt[22];
struct Net {
    void** vt;
} g_net{g_net_vt};

}  // namespace

extern "C" __declspec(dllexport) void* __cdecl SteamUtils() { return &g_utils; }
extern "C" __declspec(dllexport) void* __cdecl SteamMatchmaking() { return &g_mm; }
extern "C" __declspec(dllexport) void __cdecl SteamAPI_RunCallbacks() { answer(); }
extern "C" __declspec(dllexport) void* __cdecl SteamNetworking() { return &g_net; }
extern "C" __declspec(dllexport) long __cdecl FakeNetLog(const long** log) {
    *log = const_cast<const long*>(g_net_log);
    return g_net_n;
}
extern "C" __declspec(dllexport) void __cdecl FakeSteamCounts(long* distance, long* request, long* last_distance) {
    *distance = g_distance_calls;
    *request = g_request_calls;
    *last_distance = g_last_distance;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        g_freq = f.QuadPart;
        g_t0 = t.QuadPart;
        InitializeCriticalSection(&g_lock);
        for (void*& p : g_mm_vt) p = reinterpret_cast<void*>(&mm_other);
        g_mm_vt[4] = reinterpret_cast<void*>(&mm_request);
        g_mm_vt[9] = reinterpret_cast<void*>(&mm_distance);
        for (void*& p : g_net_vt) p = reinterpret_cast<void*>(&net_other);
        g_net_vt[0] = reinterpret_cast<void*>(&net_send);
        g_net_vt[3] = g_net_vt[4] = reinterpret_cast<void*>(&net_peer);
        g_net_vt[5] = reinterpret_cast<void*>(&net_close_channel);
    }
    return TRUE;
}
