// What the display really does while someone plays: see scanwatch.h.
#include "scanwatch.h"

#include "dxgipresent.h"
#include "layout.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

typedef UINT KmtHandle;
struct OpenFromGdi {
    WCHAR DeviceName[32];
    KmtHandle hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};
struct GetScanLine {
    KmtHandle hAdapter;
    UINT VidPnSourceId;
    BOOLEAN InVerticalBlank;
    UINT ScanLine;
};
using OpenFn = LONG(APIENTRY*)(OpenFromGdi*);
using ScanFn = LONG(APIENTRY*)(GetScanLine*);

LONGLONG g_freq = 0;
std::atomic<LONGLONG> g_last_flip{0};
bool g_monitor = false;

std::mutex g_lock;
constexpr uint32_t kMax = 8192;
uint32_t g_gap_us[kMax];          // between refresh starts
uint32_t g_after_flip_us[kMax];   // refresh start - the last flip before it
uint32_t g_gaps = 0, g_afters = 0;
uint32_t g_polls = 0;
char g_display[64] = "";

LONGLONG now_qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

uint32_t to_us(LONGLONG ticks) {
    if (ticks <= 0) return 0;
    const LONGLONG us = ticks * 1000000 / g_freq;
    return us > 10000000 ? 10000000u : static_cast<uint32_t>(us);
}

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void mark() { dxgipresent_show_mark(4.0); }

void hotkeys() {
    static bool was_u = false, was_p = false, was_h = false;
    const bool combo = down(VK_CONTROL) && down(VK_SHIFT);
    const bool u = combo && down('U'), p = combo && down('P'), h = combo && down('H');
    if (u && !was_u) {
        LOG_INFO("Session: Ctrl+Shift+U, the next 400 draws of the HUD and menu layout follow");
        layout_dump(400);
    }
    if (p && !was_p) {
        static const int kNext[3] = {2, 0, 1};  // timed -> GPU when done -> flip when ready -> timed
        const int now = dxgipresent_pacing();
        const int next = kNext[now >= 0 && now <= 2 ? now : 0];
        dxgipresent_set_pacing(next, nullptr);
        static const char* const kNames[3] = {"0, the GPU flips each frame when done (red mark)",
                                              "1, timed to the game's steps (green mark)",
                                              "2, flipped as soon as ready (yellow mark)"};
        LOG_INFO("Session: Ctrl+Shift+P, presenting mode %s", kNames[next]);
        mark();
    }
    if (h && !was_h) {
        const int next = dxgipresent_hdr_chain() ? 0 : 1;
        dxgipresent_set_hdr_chain(next);
        LOG_INFO("Session: Ctrl+Shift+H, scRGB swap chain %s (blue mark on, grey off)", next ? "on" : "off");
        mark();
    }
    was_u = u;
    was_p = p;
    was_h = h;
}

DWORD WINAPI watch_main(void*) {
    OpenFromGdi o{};
    ScanFn scan = nullptr;
    if (g_monitor) {
        HMODULE g = GetModuleHandleW(L"gdi32.dll");
        auto open = g ? reinterpret_cast<OpenFn>(GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName")) : nullptr;
        scan = g ? reinterpret_cast<ScanFn>(GetProcAddress(g, "D3DKMTGetScanLine")) : nullptr;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        wcscpy_s(o.DeviceName, mi.szDevice);
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, g_display, sizeof(g_display), nullptr, nullptr);
        if (!open || !scan || open(&o) != 0) {
            LOG_ERROR("ScanWatch: the primary display's scan line cannot be read; hotkeys only");
            scan = nullptr;
        } else {
            LOG_INFO("ScanWatch: reading the scan line of %s (the primary display) to see when it refreshes",
                     g_display);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
    }
    bool was_blank = false;
    LONGLONG last_edge = 0, next_keys = 0;
    for (;;) {
        const LONGLONG t = now_qpc();
        if (t >= next_keys) {
            next_keys = t + g_freq / 40;
            hotkeys();
        }
        if (!scan) {
            Sleep(25);
            continue;
        }
        GetScanLine s{};
        s.hAdapter = o.hAdapter;
        s.VidPnSourceId = o.VidPnSourceId;
        if (scan(&s) != 0) {
            Sleep(1);
            continue;
        }
        const LONGLONG now = now_qpc();
        if (was_blank && !s.InVerticalBlank) {
            const LONGLONG flip = g_last_flip.load();
            std::lock_guard<std::mutex> g(g_lock);
            if (last_edge && g_gaps < kMax) g_gap_us[g_gaps++] = to_us(now - last_edge);
            if (flip && g_afters < kMax) g_after_flip_us[g_afters++] = to_us(now - flip);
            last_edge = now;
        }
        was_blank = s.InVerticalBlank != 0;
        ++g_polls;
        YieldProcessor();
    }
}

}  // namespace

void scanwatch_start(bool monitor) {
    static bool started = false;
    if (started) return;
    started = true;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    g_monitor = monitor;
    dxgipresent_set_flip_observer(&scanwatch_note_flip);
    mark();
    HANDLE h = CreateThread(nullptr, 0, &watch_main, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    LOG_INFO("Session hotkeys: Ctrl+Shift+U HUD/menu draw dump, Ctrl+Shift+P presenting mode (DXVK through DXGI), "
             "Ctrl+Shift+H scRGB swap chain");
}

void scanwatch_note_flip(long long qpc) { g_last_flip = qpc; }

void scanwatch_report() {
    if (!g_monitor || !g_freq) return;
    static uint32_t gaps[kMax], afters[kMax];
    uint32_t n, m, polls;
    {
        std::lock_guard<std::mutex> g(g_lock);
        n = g_gaps;
        m = g_afters;
        std::memcpy(gaps, g_gap_us, n * sizeof(uint32_t));
        std::memcpy(afters, g_after_flip_us, m * sizeof(uint32_t));
        polls = g_polls;
        g_gaps = g_afters = g_polls = 0;
    }
    if (n < 8) return;
    double sum = 0;
    for (uint32_t i = 0; i < n; ++i) sum += gaps[i];
    const double mean = sum / n;
    double dev = 0;
    for (uint32_t i = 0; i < n; ++i) dev += (gaps[i] - mean) * (gaps[i] - mean);
    std::sort(gaps, gaps + n);
    char after[200] = "";
    if (m >= 8) {
        std::sort(afters, afters + m);
        std::snprintf(after, sizeof(after), "; refresh start after the last flip: 10%% %.2f, half %.2f, 90%% %.2f, 99%% "
                      "%.2f ms", afters[m / 10] / 1000.0, afters[m / 2] / 1000.0, afters[m * 9 / 10] / 1000.0,
                      afters[m * 99 / 100] / 1000.0);
    }
    LOG_INFO("ScanWatch: %s refreshed %u times, %.2f ms apart on average (%.1f Hz), spread %.3f ms (1%% %.2f, 10%% %.2f, "
             "half %.2f, 90%% %.2f, 99%% %.2f, longest %.2f ms; %u polls)%s", g_display, n, mean / 1000.0,
             mean > 0 ? 1e6 / mean : 0.0, std::sqrt(dev / n) / 1000.0, gaps[n / 100] / 1000.0, gaps[n / 10] / 1000.0,
             gaps[n / 2] / 1000.0, gaps[n * 9 / 10] / 1000.0, gaps[n * 99 / 100] / 1000.0, gaps[n - 1] / 1000.0, polls,
             after);
}
