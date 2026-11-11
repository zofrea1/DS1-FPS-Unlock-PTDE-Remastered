// Watches the primary monitor from outside a game: vblankmon.exe [seconds]
// Each second prints how many vertical blanks the display had and their median spacing. With G-SYNC / VRR following
// a game that presents ~100 times a second the spacing is ~10 ms; at a fixed 240 Hz it stays 4.17 ms. Also prints
// when Windows switches the display's advanced colour (HDR) on or off.
#include <windows.h>

#include <dxgi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

static int hdr_state() {  // -1 unknown, 0 off, 1 on (primary path)
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
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return 2;
    IDXGIAdapter1* a = nullptr;
    IDXGIOutput* o = nullptr;
    if (FAILED(f->EnumAdapters1(0, &a)) || FAILED(a->EnumOutputs(0, &o))) { std::printf("no output\n"); return 2; }
    LARGE_INTEGER fq, t0, t, prev;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    prev = t0;
    std::vector<double> gaps;
    double next_report = 1.0, next_hdr = 0.0;
    int hdr = hdr_state();
    std::printf("0.0 s: HDR %s\n", hdr == 1 ? "on" : hdr == 0 ? "off" : "?");
    std::fflush(stdout);
    for (;;) {
        if (FAILED(o->WaitForVBlank())) { Sleep(1); }
        QueryPerformanceCounter(&t);
        const double now = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        gaps.push_back(1000.0 * double(t.QuadPart - prev.QuadPart) / fq.QuadPart);
        prev = t;
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
            const double med = gaps.empty() ? 0 : gaps[gaps.size() / 2];
            const double p10 = gaps.empty() ? 0 : gaps[gaps.size() / 10];
            const double p90 = gaps.empty() ? 0 : gaps[gaps.size() * 9 / 10];
            std::printf("%.0f s: %zu vblanks, spacing median %.2f ms (10%% %.2f, 90%% %.2f)\n", next_report,
                        gaps.size(), med, p10, p90);
            std::fflush(stdout);
            gaps.clear();
            next_report += 1.0;
            if (next_report > seconds + 0.5) break;
        }
    }
    return 0;
}
