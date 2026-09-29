#include "frame.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// The draw thread's dispatcher (0xBAC4D0) fetches a command with `call 0x578190`
// and switches on the result (0..5), invoking one of six virtual methods on the
// render executor. That call site is a 5-byte relative call.
constexpr uint32_t kDispatchCall = 0x00BAC4ED;        // absolute VA of the call
constexpr uint32_t kGetCmdFn = 0x00578190;            // absolute VA of the callee
constexpr uint32_t kImageBase = 0x00400000;  // this build has a fixed base; addresses above are absolute

using GetCmdFn = int(__fastcall*)(void* self, void* edx);
GetCmdFn g_orig = nullptr;

constexpr int kCommands = 6;
constexpr double kSurveySeconds = 30.0;

struct Survey {
    LARGE_INTEGER freq{};
    LARGE_INTEGER start{};
    LARGE_INTEGER last_report{};
    LARGE_INTEGER last_seen[kCommands]{};
    int count[kCommands]{};
    double min_gap[kCommands];
    double max_gap[kCommands];
    double sum_gap[kCommands]{};
    int gaps[kCommands]{};
    bool active = false;
    bool done = false;
} g_survey;

void survey_reset_window() {
    for (int i = 0; i < kCommands; ++i) {
        g_survey.count[i] = 0;
        g_survey.min_gap[i] = 1e9;
        g_survey.max_gap[i] = 0.0;
        g_survey.sum_gap[i] = 0.0;
        g_survey.gaps[i] = 0;
    }
}

void survey_note(int cmd) {
    if (!g_survey.active || g_survey.done || cmd < 0 || cmd >= kCommands) {
        return;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_survey.last_seen[cmd].QuadPart != 0) {
        const double gap_ms = 1000.0 * static_cast<double>(now.QuadPart - g_survey.last_seen[cmd].QuadPart) /
                              static_cast<double>(g_survey.freq.QuadPart);
        g_survey.sum_gap[cmd] += gap_ms;
        ++g_survey.gaps[cmd];
        if (gap_ms < g_survey.min_gap[cmd]) g_survey.min_gap[cmd] = gap_ms;
        if (gap_ms > g_survey.max_gap[cmd]) g_survey.max_gap[cmd] = gap_ms;
    }
    g_survey.last_seen[cmd] = now;
    ++g_survey.count[cmd];

    const double since_report = static_cast<double>(now.QuadPart - g_survey.last_report.QuadPart) /
                                static_cast<double>(g_survey.freq.QuadPart);
    if (since_report >= 1.0) {
        char line[512];
        int n = 0;
        n += std::snprintf(line + n, sizeof(line) - n, "draw-thread commands in %.2fs:", since_report);
        for (int i = 0; i < kCommands; ++i) {
            const double avg = g_survey.gaps[i] ? g_survey.sum_gap[i] / g_survey.gaps[i] : 0.0;
            n += std::snprintf(line + n, sizeof(line) - n, " c%d=%d(gap avg %.2f min %.2f max %.2f ms)", i,
                               g_survey.count[i], avg, g_survey.gaps[i] ? g_survey.min_gap[i] : 0.0,
                               g_survey.max_gap[i]);
        }
        LOG_INFO("%s", line);
        survey_reset_window();
        g_survey.last_report = now;
        const double total = static_cast<double>(now.QuadPart - g_survey.start.QuadPart) /
                             static_cast<double>(g_survey.freq.QuadPart);
        if (total >= kSurveySeconds) {
            g_survey.done = true;
            LOG_INFO("Survey finished");
        }
    }
}

int __fastcall hook_getcmd(void* self, void* edx) {
    const int cmd = g_orig(self, edx);
    survey_note(cmd);
    return cmd;
}

}  // namespace

bool frame_install(const Settings&) {
    if (reinterpret_cast<uint32_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        LOG_ERROR("The game image is not loaded at its preferred base; absolute hook addresses are invalid");
        return false;
    }
    auto* site = reinterpret_cast<uint8_t*>(kDispatchCall);
    if (site[0] != 0xE8) {
        LOG_ERROR("Draw-thread fetch site opcode is %02X, expected E8", site[0]);
        return false;
    }
    int32_t rel = 0;
    std::memcpy(&rel, site + 1, sizeof(rel));
    const uint32_t target = kDispatchCall + 5 + static_cast<uint32_t>(rel);
    if (target != kGetCmdFn) {
        LOG_ERROR("Draw-thread fetch call goes to %08X, expected %08X", target, kGetCmdFn);
        return false;
    }
    g_orig = reinterpret_cast<GetCmdFn>(target);

    QueryPerformanceFrequency(&g_survey.freq);
    QueryPerformanceCounter(&g_survey.start);
    g_survey.last_report = g_survey.start;
    survey_reset_window();
    g_survey.active = true;

    const int32_t new_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&hook_getcmd) - (kDispatchCall + 5));
    DWORD old = 0;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("Could not unprotect the draw-thread fetch call (Win32=%lu)", GetLastError());
        return false;
    }
    std::memcpy(site + 1, &new_rel, sizeof(new_rel));
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    DWORD ignored = 0;
    VirtualProtect(site + 1, 4, old, &ignored);
    LOG_INFO("Draw-thread command survey installed at %08X (30 s, observation only)", kDispatchCall);
    return true;
}
