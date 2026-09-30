#include "deep.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kWorldChrMan = 0x0137DC70;
constexpr int kMaxFrames = 3000;

wchar_t g_dir[MAX_PATH] = {};
int g_capture = 0;
bool g_down = false;
bool g_active = false;
LARGE_INTEGER g_freq{}, g_t0{};

struct Span {
    uint32_t address;
    uint32_t words;
    uint32_t first_column;
};

std::vector<Span> g_spans;
std::vector<std::string> g_labels;      // one per column
std::vector<uint32_t> g_data;           // frames x columns
std::vector<double> g_ms, g_dt;
uint32_t g_columns = 0;

bool read_words(uint32_t address, uint32_t* out, uint32_t words) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), words * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(out, 0, words * 4);
        return false;
    }
}

uint32_t read_ptr(uint32_t address) {
    uint32_t v = 0;
    read_words(address, &v, 1);
    return v;
}

bool plausible(uint32_t v) {
    return v >= 0x10000 && v < 0x7FFE0000 && (v & 3) == 0;
}

uint32_t player() {
    const uint32_t wcm = read_ptr(kWorldChrMan);
    if (!wcm) return 0;
    const uint32_t first = read_ptr(wcm + 4);
    return first ? read_ptr(first) : 0;
}

void add_span(uint32_t address, uint32_t words, const char* path) {
    Span s{address, words, g_columns};
    g_spans.push_back(s);
    for (uint32_t i = 0; i < words; ++i) {
        char label[96];
        std::snprintf(label, sizeof(label), "%s+%03X", path, i * 4);
        g_labels.push_back(label);
    }
    g_columns += words;
}

bool start() {
    const uint32_t chr = player();
    if (!chr) {
        LOG_ERROR("Deep: player not found");
        return false;
    }
    g_spans.clear();
    g_labels.clear();
    g_data.clear();
    g_ms.clear();
    g_dt.clear();
    g_columns = 0;
    std::unordered_set<uint32_t> seen;
    static uint32_t level0[0x100], level1[0x40];
    read_words(chr, level0, 0x100);
    add_span(chr, 0x100, "c");
    seen.insert(chr);
    uint32_t objects = 0;
    for (uint32_t i = 0; i < 0x100 && objects < 80; ++i) {
        const uint32_t p1 = level0[i];
        if (!plausible(p1) || seen.count(p1)) continue;
        if (!read_words(p1, level1, 0x40)) continue;
        seen.insert(p1);
        ++objects;
        char path[64];
        std::snprintf(path, sizeof(path), "c+%03X>", i * 4);
        add_span(p1, 0x40, path);
        for (uint32_t j = 0; j < 0x40 && g_spans.size() < 700; ++j) {
            const uint32_t p2 = level1[j];
            if (!plausible(p2) || seen.count(p2)) continue;
            uint32_t probe[0x10];
            if (!read_words(p2, probe, 0x10)) continue;
            seen.insert(p2);
            char path2[96];
            std::snprintf(path2, sizeof(path2), "c+%03X>+%03X>", i * 4, j * 4);
            add_span(p2, 0x10, path2);
        }
    }
    QueryPerformanceCounter(&g_t0);
    LOG_INFO("Deep: recording %u words in %zu objects", g_columns, g_spans.size());
    return true;
}

void stop() {
    const size_t frames = g_ms.size();
    if (frames < 2) {
        LOG_ERROR("Deep: too few frames recorded");
        return;
    }
    std::vector<uint32_t> keep;
    for (uint32_t c = 0; c < g_columns; ++c) {
        const uint32_t first = g_data[c];
        for (size_t f = 1; f < frames; ++f) {
            if (g_data[f * g_columns + c] != first) {
                keep.push_back(c);
                break;
            }
        }
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%sPTDE-FPS-Unlock-deep-%d.csv", g_dir, ++g_capture);
    FILE* file = nullptr;
    _wfopen_s(&file, path, L"w");
    if (!file) {
        LOG_ERROR("Deep: could not open the output file");
        return;
    }
    std::setvbuf(file, nullptr, _IOFBF, 1 << 20);
    std::fprintf(file, "row,ms,dt_ms");
    for (uint32_t c : keep) std::fprintf(file, ",%s", g_labels[c].c_str());
    std::fputc(10, file);
    for (size_t f = 0; f < frames; ++f) {
        std::fprintf(file, "%zu,%.3f,%.3f", f, g_ms[f], g_dt[f]);
        for (uint32_t c : keep) std::fprintf(file, ",%x", g_data[f * g_columns + c]);
        std::fputc(10, file);
    }
    std::fclose(file);
    LOG_INFO("Deep: wrote %zu frames, %zu of %u words changed", frames, keep.size(), g_columns);
    g_data.clear();
    g_data.shrink_to_fit();
}

}  // namespace

void deep_set_dir(const wchar_t* dll_path) {
    wcsncpy_s(g_dir, dll_path, _TRUNCATE);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    QueryPerformanceFrequency(&g_freq);
}

void deep_frame(double dt_ms, int) {
    const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
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
        LOG_INFO("Deep: frame limit reached");
        stop();
        return;
    }
    const size_t base = g_data.size();
    g_data.resize(base + g_columns);
    for (const Span& s : g_spans) {
        read_words(s.address, &g_data[base + s.first_column], s.words);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    g_ms.push_back(1000.0 * static_cast<double>(now.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart));
    g_dt.push_back(dt_ms);
}
