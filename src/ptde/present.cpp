#include "present.h"

#include "d3d.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kImageBase = 0x00400000;
constexpr uint32_t kImageSize = 0x011C2000;

// Present wrappers (all thiscall, callee-cleaned):
//   0x5847C0  Present with no arguments
//   0x5847D0  Present with (src, dest, dirty), ret 0xC
//   0x5847A0  Present with two arguments, ret 8
constexpr uint32_t kPresent0 = 0x005847C0;
constexpr uint32_t kPresent2 = 0x005847A0;
constexpr uint32_t kPresent3 = 0x005847D0;

enum Site { kP3Early, kP3Frame, kP0Direct, kP0Repeat, kP2, kSites };

struct SiteInfo {
    uint32_t call;
    uint32_t target;
    const char* name;
} const kSiteInfo[kSites] = {
    {0x00BAC4B9, kPresent3, "p3-early"},
    {0x00BACE2F, kPresent3, "p3-frame"},
    {0x00BACE5B, kPresent0, "p0-direct"},
    {0x00BACEB1, kPresent0, "p0-repeat"},
    {0x00BACEF5, kPresent2, "p2"},
};

// `jle 0xbacec8` after the catch-up loop's exit test; making it unconditional skips
// the loop that re-presents the frame.
constexpr uint32_t kRepeatJump = 0x00BACE90;

using P0Fn = int(__fastcall*)(void*, void*);
using P2Fn = int(__fastcall*)(void*, void*, int, int);
using P3Fn = int(__fastcall*)(void*, void*, int, int, int);
uint32_t g_orig[kSites]{};

LARGE_INTEGER g_freq{};
LONGLONG g_start = 0;
LONGLONG g_window = 0;
LONGLONG g_last_present = 0;
int g_survey_seconds = 0;
bool g_done = false;

struct Stat {
    int count = 0;
    double ms = 0.0;
    double max_ms = 0.0;
} g_stat[kSites];

// Gap histogram between consecutive present calls (any site).
constexpr int kBuckets = 8;
const double kEdges[kBuckets - 1] = {1.0, 3.5, 4.8, 7.5, 9.5, 15.0, 18.5};
const char* const kBucketNames[kBuckets] = {"<1", "1-3.5", "3.5-4.8", "4.8-7.5", "7.5-9.5", "9.5-15", "15-18.5", ">18.5"};
int g_hist[kBuckets]{};
int g_total_presents = 0;
int g_same_content = 0;
unsigned long long g_prev_hash = 0;

double ms_between(LONGLONG a, LONGLONG b) {
    return 1000.0 * static_cast<double>(b - a) / static_cast<double>(g_freq.QuadPart);
}

void report(LONGLONG now) {
    if (g_survey_seconds <= 0 || g_done || ms_between(g_window, now) < 1000.0) {
        return;
    }
    char line[700];
    int n = std::snprintf(line, sizeof(line), "presents: total=%d |", g_total_presents);
    for (int i = 0; i < kSites; ++i) {
        if (g_stat[i].count) {
            n += std::snprintf(line + n, sizeof(line) - n, " %s n=%d avg %.3f max %.3f ms;", kSiteInfo[i].name,
                               g_stat[i].count, g_stat[i].ms / g_stat[i].count, g_stat[i].max_ms);
        }
    }
    n += std::snprintf(line + n, sizeof(line) - n, " | same-content presents=%d |", g_same_content);
    n += std::snprintf(line + n, sizeof(line) - n, " gaps ms:");
    for (int i = 0; i < kBuckets; ++i) {
        if (g_hist[i]) {
            n += std::snprintf(line + n, sizeof(line) - n, " %s=%d", kBucketNames[i], g_hist[i]);
        }
    }
    LOG_INFO("%s", line);
    std::memset(g_stat, 0, sizeof(g_stat));
    std::memset(g_hist, 0, sizeof(g_hist));
    g_total_presents = 0;
    g_same_content = 0;
    g_window = now;
    if (ms_between(g_start, now) / 1000.0 >= g_survey_seconds) {
        g_done = true;
    }
}

void note(Site site, LONGLONG t0) {
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&end);
    const double blocked = ms_between(t0, end.QuadPart);
    Stat& s = g_stat[site];
    ++s.count;
    s.ms += blocked;
    if (blocked > s.max_ms) s.max_ms = blocked;
    ++g_total_presents;
    {
        const unsigned long long h = d3d_take_frame_hash();
        if (h == g_prev_hash) ++g_same_content;
        g_prev_hash = h;
    }
    if (g_last_present != 0) {
        const double gap = ms_between(g_last_present, t0);
        int b = 0;
        while (b < kBuckets - 1 && gap >= kEdges[b]) ++b;
        ++g_hist[b];
    }
    g_last_present = t0;
    report(end.QuadPart);
}

LONGLONG stamp() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

#define HOOK_P3(NAME, SITE)                                                             \
    int __fastcall NAME(void* c, void* e, int a, int b, int d) {                        \
        const LONGLONG t0 = stamp();                                                    \
        const int r = reinterpret_cast<P3Fn>(g_orig[SITE])(c, e, a, b, d);              \
        note(SITE, t0);                                                                 \
        return r;                                                                       \
    }
#define HOOK_P0(NAME, SITE)                                                             \
    int __fastcall NAME(void* c, void* e) {                                             \
        const LONGLONG t0 = stamp();                                                    \
        const int r = reinterpret_cast<P0Fn>(g_orig[SITE])(c, e);                       \
        note(SITE, t0);                                                                 \
        return r;                                                                       \
    }

HOOK_P3(hook_p3_early, kP3Early)
HOOK_P3(hook_p3_frame, kP3Frame)
HOOK_P0(hook_p0_direct, kP0Direct)
HOOK_P0(hook_p0_repeat, kP0Repeat)

int __fastcall hook_p2(void* c, void* e, int a, int b) {
    const LONGLONG t0 = stamp();
    const int r = reinterpret_cast<P2Fn>(g_orig[kP2])(c, e, a, b);
    note(kP2, t0);
    return r;
}

bool make_writable(void* address, size_t size) {
    DWORD old = 0;
    return VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old) != 0;
}

bool patch_call_site(Site index, void* detour) {
    auto* site = reinterpret_cast<uint8_t*>(kSiteInfo[index].call);
    if (site[0] != 0xE8) {
        LOG_ERROR("Present site %s at %08X has opcode %02X, expected E8", kSiteInfo[index].name,
                  kSiteInfo[index].call, site[0]);
        return false;
    }
    int32_t rel = 0;
    std::memcpy(&rel, site + 1, sizeof(rel));
    const uint32_t target = kSiteInfo[index].call + 5 + static_cast<uint32_t>(rel);
    if (target != kSiteInfo[index].target) {
        LOG_ERROR("Present site %s goes to %08X, expected %08X. Not hooking it.", kSiteInfo[index].name, target,
                  kSiteInfo[index].target);
        return false;
    }
    g_orig[index] = target;
    const int32_t new_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(detour) - (kSiteInfo[index].call + 5));
    if (!make_writable(site + 1, 4)) {
        LOG_ERROR("Could not unprotect present site %s (Win32=%lu)", kSiteInfo[index].name, GetLastError());
        return false;
    }
    std::memcpy(site + 1, &new_rel, sizeof(new_rel));
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

}  // namespace

bool present_install(const Settings& settings) {
    if (reinterpret_cast<uint32_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        return false;
    }
    QueryPerformanceFrequency(&g_freq);
    g_start = g_window = stamp();
    g_survey_seconds = settings.survey_seconds;

    if (settings.skip_present_repeat) {
        auto* jump = reinterpret_cast<uint8_t*>(kRepeatJump);
        if (jump[0] == 0x7E && jump[1] == 0x36) {
            if (make_writable(jump, 2)) {
                jump[0] = 0xEB;  // jle -> jmp
                FlushInstructionCache(GetCurrentProcess(), jump, 2);
                LOG_INFO("Present catch-up loop disabled at %08X", kRepeatJump);
            } else {
                LOG_ERROR("Could not unprotect the catch-up loop jump (Win32=%lu)", GetLastError());
            }
        } else if (jump[0] == 0xEB) {
            LOG_INFO("Present catch-up loop was already disabled");
        } else {
            LOG_ERROR("Catch-up loop jump at %08X is %02X %02X, not the expected 7E 36. Not patching.", kRepeatJump,
                      jump[0], jump[1]);
        }
    }

    if (settings.survey_seconds > 0) {
        int hooked = 0;
        hooked += patch_call_site(kP3Early, reinterpret_cast<void*>(&hook_p3_early)) ? 1 : 0;
        hooked += patch_call_site(kP3Frame, reinterpret_cast<void*>(&hook_p3_frame)) ? 1 : 0;
        hooked += patch_call_site(kP0Direct, reinterpret_cast<void*>(&hook_p0_direct)) ? 1 : 0;
        hooked += patch_call_site(kP0Repeat, reinterpret_cast<void*>(&hook_p0_repeat)) ? 1 : 0;
        hooked += patch_call_site(kP2, reinterpret_cast<void*>(&hook_p2)) ? 1 : 0;
        LOG_INFO("Present instrumentation installed on %d of %d call sites", hooked, static_cast<int>(kSites));
    }
    return true;
}
