// Reads the primary monitor's scan line from outside a game: scanmon.exe [seconds]
// Each second prints how many refreshes the display scanned out (vertical blank -> active edges) and the share of time
// spent in vertical blank. At a fixed 240 Hz that is 240 refreshes and a few percent; with G-SYNC / VRR following a
// game at ~100 fps it is ~100 refreshes and over half the time in blank (the display waits for the next frame).
// Also prints when Windows turns the display's advanced colour (HDR) on or off, and each second the spread of the
// times between refreshes (10th, 50th and 90th percentile and the longest, in ms): with VRR following a steady frame
// rate they are all one frame time; a display that refreshes at its own pace or doubles frames shows two clusters.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

typedef UINT D3DKMT_HANDLE;
struct OpenFromGdi {
    WCHAR DeviceName[32];
    D3DKMT_HANDLE hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};
struct GetScanLine {
    D3DKMT_HANDLE hAdapter;
    UINT VidPnSourceId;
    BOOLEAN InVerticalBlank;
    UINT ScanLine;
};
typedef LONG(APIENTRY* OpenFn)(OpenFromGdi*);
typedef LONG(APIENTRY* ScanFn)(GetScanLine*);

static int hdr_state() {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return -1;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
        return -1;
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
        info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        info.header.size = sizeof(info);
        info.header.adapterId = paths[i].targetInfo.adapterId;
        info.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) return info.advancedColorEnabled ? 1 : 0;
    }
    return -1;
}

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 10.0;
    HMODULE g = GetModuleHandleA("gdi32.dll");
    auto open = reinterpret_cast<OpenFn>(GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName"));
    auto scan = reinterpret_cast<ScanFn>(GetProcAddress(g, "D3DKMTGetScanLine"));
    if (!open || !scan) return 2;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    OpenFromGdi o{};
    wcscpy_s(o.DeviceName, mi.szDevice);
    if (open(&o) != 0) { std::printf("open adapter failed\n"); return 2; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    LARGE_INTEGER fq, t0, t;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    int hdr = hdr_state();
    std::printf("0.00 s: HDR %s\n", hdr == 1 ? "on" : hdr == 0 ? "off" : "?");
    double next_report = 1.0, next_hdr = 0.25, last = 0.0, blank_time = 0.0;
    long long refreshes = 0, polls = 0;
    bool was_blank = false;
    double last_edge = 0.0;
    std::vector<double> gaps, all;
    for (;;) {
        GetScanLine s{};
        s.hAdapter = o.hAdapter;
        s.VidPnSourceId = o.VidPnSourceId;
        if (scan(&s) != 0) { Sleep(1); continue; }
        QueryPerformanceCounter(&t);
        const double now = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (was_blank) blank_time += now - last;
        if (was_blank && !s.InVerticalBlank) {
            ++refreshes;
            if (last_edge > 0.0) gaps.push_back((now - last_edge) * 1000.0);
            last_edge = now;
        }
        was_blank = s.InVerticalBlank != 0;
        last = now;
        ++polls;
        if (now >= next_hdr) {
            next_hdr = now + 0.25;
            const int h = hdr_state();
            if (h != hdr) {
                hdr = h;
                std::printf("%.2f s: HDR %s\n", now, hdr == 1 ? "on" : hdr == 0 ? "off" : "?");
            }
        }
        if (now >= next_report) {
            std::sort(gaps.begin(), gaps.end());
            const size_t n = gaps.size();
            std::printf("%.0f s: %lld refreshes, %.0f%% of the time in vertical blank (%lld polls); between refreshes "
                        "p10 %.2f p50 %.2f p90 %.2f max %.2f ms\n", next_report, refreshes, 100.0 * blank_time, polls,
                        n ? gaps[n / 10] : 0.0, n ? gaps[n / 2] : 0.0, n ? gaps[n * 9 / 10] : 0.0, n ? gaps[n - 1] : 0.0);
            all.insert(all.end(), gaps.begin(), gaps.end());
            gaps.clear();
            std::fflush(stdout);
            refreshes = polls = 0;
            blank_time = 0.0;
            next_report += 1.0;
            if (next_report > seconds + 0.5) break;
        }
    }
    std::sort(all.begin(), all.end());
    if (!all.empty()) {
        const size_t n = all.size();
        double sum = 0;
        for (double g : all) sum += g;
        const double mean = sum / n;
        double dev = 0;
        for (double g : all) dev += (g - mean) * (g - mean);
        std::printf("all: %zu refreshes, mean %.3f ms (%.1f Hz), spread (standard deviation) %.3f ms, p1 %.2f p10 %.2f "
                    "p50 %.2f p90 %.2f p99 %.2f max %.2f ms\n", n, mean, 1000.0 / mean, std::sqrt(dev / n), all[n / 100],
                    all[n / 10], all[n / 2], all[n * 9 / 10], all[n * 99 / 100], all[n - 1]);
    }
    return 0;
}
