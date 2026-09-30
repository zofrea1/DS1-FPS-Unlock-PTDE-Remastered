#include "trace.h"

#include "log.h"
#include "snap.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern volatile int g_cmd_totals[6];

namespace {

constexpr uint32_t kWorldChrMan = 0x0137DC70;

wchar_t g_dir[MAX_PATH] = {};
FILE* g_file = nullptr;
int g_capture = 0;
uint32_t g_row = 0;
LARGE_INTEGER g_freq{}, g_t0{};
bool g_down = false;

constexpr int kChrWords = 0x100;   // chr[0 .. 0x400)
constexpr int kMcWords = 0xC0;     // movement controller [0 .. 0x300)
constexpr int kPhysWords = 0xC0;   // physics [0 .. 0x300)

// Reads `words` dwords from `p`, zero-filled if the memory is not readable.
bool read_words(const void* p, uint32_t* out, int words) {
    std::memset(out, 0, words * 4);
    if (!p) return false;
    __try {
        std::memcpy(out, p, words * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* read_ptr(const void* p);
const uint8_t* player();
void snapshot(const char* tag);

const void* read_ptr(const void* p) {
    if (!p) return nullptr;
    __try {
        return *static_cast<const void* const*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// [WorldChrMan] is a vector of (character, extra) pairs; the first character is the player.
const uint8_t* player() {
    const void* wcm = read_ptr(reinterpret_cast<const void*>(kWorldChrMan));
    if (!wcm) return nullptr;
    const void* first = read_ptr(static_cast<const uint8_t*>(wcm) + 4);
    return static_cast<const uint8_t*>(read_ptr(first));
}

bool plausible(uint32_t v) {
    return v >= 0x10000 && v < 0x7FFE0000 && (v & 3) == 0;
}

// One-off dump of the memory reachable from the player object: the player's first 0x400
// bytes, and everything two pointer levels below it. Two of these (start and end of a
// capture) show which fields hold positions and other state that changed in between.
void snapshot(const char* tag) {
    const uint8_t* chr = player();
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%sPTDE-FPS-Unlock-snap-%d-%hs.txt", g_dir, g_capture, tag);
    FILE* f = _wfopen(path, L"w");
    if (!f || !chr) {
        if (f) std::fclose(f);
        return;
    }
    static uint32_t a[0x100], b[0x40];
    read_words(chr, a, 0x100);
    std::fprintf(f, "chr %p\n", chr);
    for (int i = 0; i < 0x100; ++i) {
        std::fprintf(f, "c+%03X %08x\n", i * 4, a[i]);
    }
    for (int i = 0; i < 0x100; ++i) {
        if (!plausible(a[i])) continue;
        if (!read_words(reinterpret_cast<void*>(a[i]), b, 0x40)) continue;
        for (int j = 0; j < 0x40; ++j) {
            std::fprintf(f, "c+%03X>+%03X %08x\n", i * 4, j * 4, b[j]);
        }
        static uint32_t c2[0x20];
        for (int j = 0; j < 0x40; ++j) {
            if (!plausible(b[j]) || b[j] == a[i]) continue;
            if (!read_words(reinterpret_cast<void*>(b[j]), c2, 0x20)) continue;
            for (int k = 0; k < 0x20; ++k) {
                std::fprintf(f, "c+%03X>+%03X>+%03X %08x\n", i * 4, j * 4, k * 4, c2[k]);
            }
        }
    }
    std::fclose(f);
}

void start_capture() {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%sPTDE-FPS-Unlock-trace-%d.csv", g_dir, ++g_capture);
    g_file = _wfopen(path, L"w");
    if (!g_file) {
        LOG_ERROR("Trace: could not open the capture file");
        return;
    }
    std::setvbuf(g_file, nullptr, _IOFBF, 1 << 20);
    std::fprintf(g_file, "row,ms,dt_ms,fps");
    for (int i = 0; i < kChrWords; ++i) std::fprintf(g_file, ",c%03X", i * 4);
    for (int i = 0; i < kMcWords; ++i) std::fprintf(g_file, ",m%03X", i * 4);
    for (int i = 0; i < kPhysWords; ++i) std::fprintf(g_file, ",p%03X", i * 4);
    std::fprintf(g_file, ",sn_dy,sn_factor,sn_lifted,sn_calls");
    for (int i = 0; i < 6; ++i) std::fprintf(g_file, ",cmd%d", i);
    for (int i = 0; i < 0x20; ++i) std::fprintf(g_file, ",g1_%02X", i * 4);
    for (int i = 0; i < 0x10; ++i) std::fprintf(g_file, ",g2_%02X", i * 4);
    std::fprintf(g_file, "\n");
    g_row = 0;
    snapshot("start");
    QueryPerformanceCounter(&g_t0);
    LOG_INFO("Trace: capture %d started", g_capture);
}

void stop_capture() {
    if (g_file) {
        snapshot("end");
        std::fclose(g_file);
        g_file = nullptr;
        LOG_INFO("Trace: capture %d stopped after %u rows", g_capture, g_row);
    }
}

}  // namespace

void trace_set_dir(const wchar_t* dll_path) {
    wcsncpy_s(g_dir, dll_path, _TRUNCATE);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    QueryPerformanceFrequency(&g_freq);
}

void trace_frame(double dt_ms, int target_fps) {
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !g_down) {
        if (g_file) stop_capture(); else start_capture();
    }
    g_down = down;
    if (!g_file) return;

    const uint8_t* chr = player();
    if (!chr) return;
    const uint8_t* mc = static_cast<const uint8_t*>(read_ptr(chr + 0x28));
    const uint8_t* phys = static_cast<const uint8_t*>(read_ptr(mc + 0x1C));

    static uint32_t c[kChrWords], m[kMcWords], p[kPhysWords];
    read_words(chr, c, kChrWords);
    read_words(mc, m, kMcWords);
    read_words(phys, p, kPhysWords);

    if (g_row == 0) {
        float pos[3];
        std::memcpy(pos, p + 4, sizeof(pos));
        LOG_INFO("Trace: chr=%p mc=%p phys=%p pos=(%.2f, %.2f, %.2f)", chr, mc, phys, pos[0], pos[1], pos[2]);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    std::fprintf(g_file, "%u,%.3f,%.3f,%d", g_row++,
                 1000.0 * static_cast<double>(now.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart),
                 dt_ms, target_fps);
    for (int i = 0; i < kChrWords; ++i) std::fprintf(g_file, ",%x", c[i]);
    for (int i = 0; i < kMcWords; ++i) std::fprintf(g_file, ",%x", m[i]);
    for (int i = 0; i < kPhysWords; ++i) std::fprintf(g_file, ",%x", p[i]);
    {
        float dy = 0, factor = 0;
        int lifted = 0;
        unsigned calls = 0;
        snap_debug(phys, &dy, &factor, &lifted, &calls);
        std::fprintf(g_file, ",%.4f,%.4f,%d,%u", dy, factor, lifted, calls);
    }
    for (int i = 0; i < 6; ++i) std::fprintf(g_file, ",%d", g_cmd_totals[i]);
    {
        // Global clock objects: [0x13784A0] and [0x137CDFC] (the countdown the 1/30 timer touches).
        static uint32_t g1[0x20], g2[0x10];
        read_words(read_ptr(reinterpret_cast<const void*>(0x013784A0)), g1, 0x20);
        read_words(read_ptr(reinterpret_cast<const void*>(0x0137CDFC)), g2, 0x10);
        for (int i = 0; i < 0x20; ++i) std::fprintf(g_file, ",%x", g1[i]);
        for (int i = 0; i < 0x10; ++i) std::fprintf(g_file, ",%x", g2[i]);
    }
    std::fprintf(g_file, "\n");
}
