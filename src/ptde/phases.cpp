#include "phases.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>

#include <cstdio>
#include <cstring>

namespace {

// The frame function's own direct calls (call rel32), in order. Its calls through registers are not timed; their
// share is the frame's ticks minus the sites'.
constexpr uint32_t kFrameSites[] = {0xF917E6, 0xF917F9, 0xF91807, 0xF9180D, 0xF9181C, 0xF91856, 0xF91865,
                                    0xF9187A, 0xF91892, 0xF918A6, 0xF918BB, 0xF918CA, 0xF918D8, 0xF918E7,
                                    0xF91901, 0xF91906, 0xF91910, 0xF91915, 0xF9194A};

struct Site {
    uint32_t ret;      // the caller's return address while the callee runs (the stub popped it)
    uint32_t pad;
    uint64_t start;    // counter at the call
    uint64_t ticks;    // counter ticks inside the callee, since the last report
    uint32_t address;  // the call instruction
    uint32_t target;   // the callee
    uint32_t calls;    // since the last report
    uint8_t* stub;
};
constexpr int kMaxSites = 64;
Site g_sites[kMaxSites];
int g_count = 0;
uint8_t* g_code = nullptr;  // stubs (executable)
size_t g_code_used = 0;
constexpr size_t kCodeSize = 64 * 96;

bool g_active = false;
uint64_t g_frame_ticks = 0;
uint32_t g_frames = 0;
uint64_t g_tsc0 = 0;
LARGE_INTEGER g_qpc0{};
double g_tsc_per_ms = 0.0;  // learned from the first report window

void put8(uint8_t*& p, uint8_t v) { *p++ = v; }
void put32(uint8_t*& p, uint32_t v) {
    std::memcpy(p, &v, 4);
    p += 4;
}

// The stub for one site: takes the return address off the stack (so the callee sees its arguments where the caller
// left them), reads the counter, calls the callee, adds the counter difference, returns to the caller. eax and edx
// (return values, register arguments) are kept around the counter reads; the flags are not (no caller expects them
// across a call).
uint8_t* build_stub(Site& s) {
    if (!g_code) {
        g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCodeSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_code) return nullptr;
    }
    if (g_code_used + 96 > kCodeSize) return nullptr;
    uint8_t* stub = g_code + g_code_used;
    uint8_t* p = stub;
    const uint32_t ret = reinterpret_cast<uint32_t>(&s.ret);
    const uint32_t start = reinterpret_cast<uint32_t>(&s.start);
    const uint32_t ticks = reinterpret_cast<uint32_t>(&s.ticks);
    const uint32_t calls = reinterpret_cast<uint32_t>(&s.calls);
    put8(p, 0x8F); put8(p, 0x05); put32(p, ret);                        // pop [ret]
    put8(p, 0x50); put8(p, 0x52);                                       // push eax; push edx
    put8(p, 0x0F); put8(p, 0x31);                                       // rdtsc
    put8(p, 0xA3); put32(p, start);                                     // mov [start], eax
    put8(p, 0x89); put8(p, 0x15); put32(p, start + 4);                  // mov [start+4], edx
    put8(p, 0x5A); put8(p, 0x58);                                       // pop edx; pop eax
    put8(p, 0xE8); put32(p, s.target - (reinterpret_cast<uint32_t>(p) + 4));  // call target
    put8(p, 0x50); put8(p, 0x52);                                       // push eax; push edx
    put8(p, 0x0F); put8(p, 0x31);                                       // rdtsc
    put8(p, 0x2B); put8(p, 0x05); put32(p, start);                      // sub eax, [start]
    put8(p, 0x1B); put8(p, 0x15); put32(p, start + 4);                  // sbb edx, [start+4]
    put8(p, 0x01); put8(p, 0x05); put32(p, ticks);                      // add [ticks], eax
    put8(p, 0x11); put8(p, 0x15); put32(p, ticks + 4);                  // adc [ticks+4], edx
    put8(p, 0xFF); put8(p, 0x05); put32(p, calls);                      // inc dword [calls]
    put8(p, 0x5A); put8(p, 0x58);                                       // pop edx; pop eax
    put8(p, 0xFF); put8(p, 0x25); put32(p, ret);                        // jmp [ret]
    g_code_used += 96;
    return stub;
}

// Times the call at `address`. Game thread only, at a frame start (no other thread runs the sites).
const char* add_site(uint32_t address) {
    for (int i = 0; i < g_count; ++i) {
        if (g_sites[i].address == address) return "already timed";
    }
    if (g_count >= kMaxSites) return "too many sites";
    auto* at = reinterpret_cast<uint8_t*>(address);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE))) {
        return "not code";
    }
    if (at[0] != 0xE8) return "not a call rel32";
    int32_t rel = 0;
    std::memcpy(&rel, at + 1, 4);
    Site& s = g_sites[g_count];
    s = Site{};
    s.address = address;
    s.target = address + 5 + static_cast<uint32_t>(rel);
    s.stub = build_stub(s);
    if (!s.stub) return "no room for the stub";
    DWORD old = 0;
    if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return "could not unprotect";
    const int32_t new_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(s.stub) - (address + 5));
    std::memcpy(at + 1, &new_rel, 4);
    DWORD ignored = 0;
    VirtualProtect(at + 1, 4, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    ++g_count;
    return "timed";
}

void report() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    const uint64_t tsc = __rdtsc();
    const double ms = 1000.0 * static_cast<double>(now.QuadPart - g_qpc0.QuadPart) / static_cast<double>(f.QuadPart);
    if (ms <= 0.0 || g_frames == 0) return;
    g_tsc_per_ms = static_cast<double>(tsc - g_tsc0) / ms;
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "[phases] %.1fs %u frames, ms per frame: frame %.3f |", ms / 1000.0,
                          g_frames, static_cast<double>(g_frame_ticks) / g_tsc_per_ms / g_frames);
    uint64_t sum = 0;
    for (int i = 0; i < g_count && n > 0 && n < static_cast<int>(sizeof(line)) - 40; ++i) {
        Site& s = g_sites[i];
        sum += s.ticks;
        n += std::snprintf(line + n, sizeof(line) - n, " %X %.3f%s", s.target,
                           static_cast<double>(s.ticks) / g_tsc_per_ms / g_frames, s.calls > g_frames ? "+" : "");
        s.ticks = 0;
        s.calls = 0;
    }
    LOG_INFO("%s", line);
    g_frame_ticks = 0;
    g_frames = 0;
    g_tsc0 = tsc;
    g_qpc0 = now;
}

}  // namespace

void phases_game_frame() {
    if (!g_active) return;
    ++g_frames;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    if (now.QuadPart - g_qpc0.QuadPart >= 5 * f.QuadPart) report();
}

void phases_frame_ticks(uint64_t ticks) {
    if (g_active) g_frame_ticks += ticks;
}

bool phases_active() { return g_active; }

const char* phases_command(const char* verb, uint32_t address) {
    if (std::strcmp(verb, "on") == 0) {
        if (!g_active) {
            for (uint32_t a : kFrameSites) add_site(a);
            QueryPerformanceCounter(&g_qpc0);
            g_tsc0 = __rdtsc();
            g_frames = 0;
            g_frame_ticks = 0;
            g_active = true;
            LOG_INFO("[phases] timing %d calls of the frame function", g_count);
        }
        return "on";
    }
    if (std::strcmp(verb, "add") == 0) {
        const char* r = add_site(address);
        LOG_INFO("[phases] call at %08X: %s", address, r);
        return r;
    }
    return "phases on | phases add ADDR";
}
