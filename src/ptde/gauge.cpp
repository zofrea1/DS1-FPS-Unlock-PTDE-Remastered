#include "gauge.h"

#include "log.h"
#include "watch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kVtables[3] = {0x011C090C, 0x011BE714, 0x011BE324};  // PCGauge, SmoothGauge, ObjGauge
constexpr const char* kNames[3] = {"pc", "smooth", "obj"};
constexpr int kMaxObjects = 400;
constexpr int kMaxFrames = 3000;

wchar_t g_dir[MAX_PATH] = {};
int g_capture = 0;
size_t armed_at = 0;
bool g_down = false;
bool g_active = false;
LARGE_INTEGER g_freq{}, g_t0{};

struct Object {
    uint32_t address;
    int kind;
    uint32_t words;   // 0x180 for the player gauge, 0x40 for the others
    size_t first;     // first column
};
size_t g_columns = 0;
std::vector<Object> g_objects;
std::vector<uint32_t> g_data;  // frames x (objects * kWords)
std::vector<double> g_ms, g_dt;

bool read_words(uint32_t address, uint32_t* out, uint32_t words) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), words * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(out, 0, words * 4);
        return false;
    }
}

void scan_region(uint32_t begin, uint32_t end) {
    __try {
        for (uint32_t a = begin; a + 4 <= end && static_cast<int>(g_objects.size()) < kMaxObjects; a += 4) {
            const uint32_t v = *reinterpret_cast<const uint32_t*>(a);
            for (int k = 0; k < 3; ++k) {
                if (v == kVtables[k]) {
                    g_objects.push_back({a, k, k == 0 ? 0x180u : 0x40u, 0});
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool start() {
    armed_at = 0;
    g_objects.clear();
    g_data.clear();
    g_ms.clear();
    g_dt.clear();
    MEMORY_BASIC_INFORMATION info{};
    uint32_t address = 0x10000;
    while (address < 0x7FFE0000 && VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) {
        const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.BaseAddress));
        if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && (info.Protect & 0xFF) == PAGE_READWRITE &&
            (info.Protect & PAGE_GUARD) == 0) {
            scan_region(base, base + static_cast<uint32_t>(info.RegionSize));
        }
        address = base + static_cast<uint32_t>(info.RegionSize);
        if (address <= base) break;
    }
    if (g_objects.empty()) {
        LOG_ERROR("Gauge: no gauge objects found");
        return false;
    }
    g_columns = 0;
    for (Object& o : g_objects) {
        o.first = g_columns;
        g_columns += o.words;
    }
    QueryPerformanceCounter(&g_t0);
    int counts[3] = {};
    for (const Object& o : g_objects) ++counts[o.kind];
    LOG_INFO("Gauge: recording %zu objects (pc %d, smooth %d, obj %d)", g_objects.size(), counts[0], counts[1],
             counts[2]);
    return true;
}

void stop() {
    const size_t frames = g_ms.size();
    const size_t columns = g_columns;
    if (frames < 2) {
        LOG_ERROR("Gauge: too few frames recorded");
        return;
    }
    std::vector<size_t> keep;
    for (size_t c = 0; c < columns; ++c) {
        for (size_t f = 1; f < frames; ++f) {
            if (g_data[f * columns + c] != g_data[c]) {
                keep.push_back(c);
                break;
            }
        }
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%sPTDE-FPS-Unlock-gauge-%d.csv", g_dir, ++g_capture);
    FILE* file = nullptr;
    _wfopen_s(&file, path, L"w");
    if (!file) {
        LOG_ERROR("Gauge: could not open the output file");
        return;
    }
    std::fprintf(file, "row,ms,dt_ms");
    for (size_t c : keep) {
        size_t oi = 0;
        while (oi + 1 < g_objects.size() && g_objects[oi + 1].first <= c) ++oi;
        const Object& o = g_objects[oi];
        std::fprintf(file, ",%s%zu_%08X+%03X", kNames[o.kind], oi, o.address, static_cast<unsigned>((c - o.first) * 4));
    }
    std::fputc(10, file);
    for (size_t f = 0; f < frames; ++f) {
        std::fprintf(file, "%zu,%.3f,%.3f", f, g_ms[f], g_dt[f]);
        for (size_t c : keep) std::fprintf(file, ",%x", g_data[f * columns + c]);
        std::fputc(10, file);
    }
    std::fclose(file);
    LOG_INFO("Gauge: wrote %zu frames, %zu of %zu words changed", frames, keep.size(), columns);
    g_data.clear();
    g_data.shrink_to_fit();
}

}  // namespace

void gauge_set_dir(const wchar_t* dll_path) {
    wcsncpy_s(g_dir, dll_path, _TRUNCATE);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    QueryPerformanceFrequency(&g_freq);
}

void gauge_frame(double dt_ms) {
    const bool down = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
    if (down && !g_down) {
        if (g_active) {
            g_active = false;
            stop();
        } else {
            g_active = start();
        }
    }
    g_down = down;
    if (!g_active) return;
    if (g_ms.size() >= static_cast<size_t>(kMaxFrames)) {
        g_active = false;
        LOG_INFO("Gauge: frame limit reached");
        stop();
        return;
    }
    const size_t columns = g_columns;
    const size_t base = g_data.size();
    g_data.resize(base + columns);
    for (size_t i = 0; i < g_objects.size(); ++i) {
        read_words(g_objects[i].address, &g_data[base + g_objects[i].first], g_objects[i].words);
    }
    // Once a smooth-gauge value moves by a visible amount, put a write watchpoint on it and report the
    // writers a little later: that is the code that animates the bar.
    if (g_data.size() >= 2 * columns) {
        const size_t previous = base - columns;
        if (armed_at == 0) {
            size_t oi = 0;
            for (size_t c = 0; c < columns; ++c) {
                while (oi + 1 < g_objects.size() && g_objects[oi + 1].first <= c) ++oi;
                float a = 0, b = 0;
                std::memcpy(&a, &g_data[previous + c], 4);
                std::memcpy(&b, &g_data[base + c], 4);
                if (a == a && b == b && std::fabs(b - a) > 0.05f && std::fabs(b) < 2.0f && std::fabs(a) < 2.0f &&
                    std::fabs(b - a) < 1.0f) {
                    watch_arm_address(g_objects[oi].address + static_cast<uint32_t>((c - g_objects[oi].first) * 4));
                    armed_at = g_ms.size();
                    break;
                }
            }
        } else if (g_ms.size() == armed_at + 120) {
            watch_report_now();
        }
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    g_ms.push_back(1000.0 * static_cast<double>(now.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart));
    g_dt.push_back(dt_ms);
}
