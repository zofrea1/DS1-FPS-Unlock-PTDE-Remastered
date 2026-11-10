#include "frame.h"

#include "fpsu_buildcfg.h"
#include "camera.h"
#include "cutscenecam.h"
#include "deep.h"
#include "diag.h"
#include "fixes.h"
#include "footik.h"
#include "gauge.h"
#include "ghost.h"
#include "limiter.h"
#include "log.h"
#include "present.h"
#include "profile.h"
#include "script.h"
#include "sfx.h"
#include "slidecontact.h"
#include "slidesteer.h"
#include "snap.h"
#include "bullettrace.h"
#include "sfxwatch.h"
#include "soulcounter.h"
#include "flick.h"
#include "roll.h"
#include "switchback.h"
#include "speffect.h"
#include "hitrecord.h"
#include "trace.h"
#include "turn.h"
#include "ui.h"
#include "watch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "winmm.lib")

volatile int g_cmd_totals[6] = {};

namespace {

// Fixed-base 32-bit image (checked at install). All addresses are absolute.
constexpr uint32_t kImageBase = 0x00400000;
constexpr uint32_t kImageSize = 0x011C2000;

// The draw thread's dispatcher (0xBAC4D0) fetches a command with `call 0x578190` and
// switches on it (0..5). During gameplay commands 2, 3 and 4 each fire once per
// frame; command 5 fires instead in loading screens. Command 2 is used as the frame
// boundary (and 5, so loading screens keep a sane step).
constexpr uint32_t kDispatchCall = 0x00BAC4ED;
constexpr uint32_t kGetCmdFn = 0x00578190;

// WinMain's frame loop calls this once per frame, and the call is the whole body of the loop.
// The function reads the pad, keyboard and mouse before it steps the world.
constexpr uint32_t kGameFrameCall = 0x01025AA7;
constexpr uint32_t kGameFrameFn = 0x00F91770;

// The engine's 1/30 second step, one pooled float in .rdata loaded directly by 37
// pieces of code.
constexpr uint32_t kTimestepVa = 0x011E7E90;

// `mov dword ptr [esi+0x248], 2` in the swap-interval setter (0xFFB640). DSfix
// rewrites the immediate to 5; without it the render thread waits two vertical
// blanks per frame, which is the 30 FPS lock.
constexpr uint32_t kVblankStoreVa = 0x00FFB688;
constexpr uint32_t kVblankImmVa = 0x00FFB68E;

using GetCmdFn = int(__fastcall*)(void* self, void* edx);
GetCmdFn g_orig = nullptr;
using GameFrameFn = char (*)();
GameFrameFn g_game_frame = nullptr;


constexpr int kCommands = 6;
constexpr float kMinStep = 1.0f / 1000.0f;
// Longest measured frame written as it is. It leaves headroom over the lowest MaxFPS (10), so the
// game keeps real time down to 8 FPS; a longer frame (a hitch, loading) becomes one step of this length.
constexpr float kMaxStep = 1.0f / 8.0f;

struct State {
    Settings cfg;
    float* step = nullptr;
    LARGE_INTEGER freq{};
    LONGLONG period = 0;      // ticks per target frame
    LONGLONG last = 0;        // tick of the previous frame boundary
    LONGLONG start = 0;
    LONGLONG report = 0;
    bool driver = false;
    int rate = 0;
    double last_dt_ms = 0.0;

    // per-window survey
    int count[kCommands]{};
    int frames = 0;
    double dt_min = 1e9;
    double dt_max = 0.0;
    double dt_sum = 0.0;
    float written = 0.0f;
    bool survey_done = false;

    // Render-thread busy time: command 2 (start of the frame's render work) to
    // command 4 (end of it), and how long the limiter waited. Together they show
    // how much of the frame budget the CPU actually uses.
    LONGLONG t_cmd2 = 0;
    int spans = 0;
    double span_sum = 0.0;
    double span_max = 0.0;
    double wait_sum = 0.0;

} g;

double ms_between(LONGLONG a, LONGLONG b) {
    return 1000.0 * static_cast<double>(b - a) / static_cast<double>(g.freq.QuadPart);
}

// CPU use of the render thread over the last survey window: close to 100% means the
// frame is CPU-bound, far below it means the thread is waiting on the GPU or driver.
// (Enumerating every thread here caused a visible hitch once a second, so only the
// calling thread is measured.)
ULONGLONG g_draw_cpu_prev = 0;
bool g_draw_cpu_primed = false;

void draw_cpu_summary(double window_s, char* out, size_t cap) {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user)) {
        out[0] = 0;
        return;
    }
    const ULONGLONG cpu = ((static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                          ((static_cast<ULONGLONG>(user.dwHighDateTime) << 32) | user.dwLowDateTime);
    const double pct = 100.0 * static_cast<double>(cpu - g_draw_cpu_prev) / 1e7 / window_s;
    g_draw_cpu_prev = cpu;
    if (!g_draw_cpu_primed) {
        g_draw_cpu_primed = true;
        out[0] = 0;
        return;
    }
    std::snprintf(out, cap, " | render thread cpu %.0f%%", pct);
}

void survey_report(LONGLONG now) {
    if (g.cfg.survey_seconds <= 0 || g.survey_done) {
        return;
    }
    const double window = ms_between(g.report, now) / 1000.0;
    if (window < 1.0) {
        return;
    }
    char line[700];
    int n = std::snprintf(line, sizeof(line), "%.2fs: frames=%d (%.1f fps)", window, g.frames, g.frames / window);
    if (g.frames > 0) {
        n += std::snprintf(line + n, sizeof(line) - n, " dt avg %.3f min %.3f max %.3f ms, step written %.6f s",
                           g.dt_sum / g.frames, g.dt_min, g.dt_max, g.written);
    }
    if (g.spans > 0) {
        n += std::snprintf(line + n, sizeof(line) - n, " | render busy avg %.3f max %.3f ms, limiter wait avg %.3f ms",
                           g.span_sum / g.spans, g.span_max, g.frames ? g.wait_sum / g.frames : 0.0);
    }
    n += std::snprintf(line + n, sizeof(line) - n, " | cmds");
    for (int i = 0; i < kCommands; ++i) {
        n += std::snprintf(line + n, sizeof(line) - n, " c%d=%d", i, g.count[i]);
    }
    draw_cpu_summary(window, line + n, sizeof(line) - n);
    LOG_INFO("%s", line);
    std::memset(g.count, 0, sizeof(g.count));
    g.frames = 0;
    g.dt_min = 1e9;
    g.dt_max = 0.0;
    g.dt_sum = 0.0;
    g.spans = 0;
    g.span_sum = 0.0;
    g.span_max = 0.0;
    g.wait_sum = 0.0;
    g.report = now;
    if (ms_between(g.start, now) / 1000.0 >= g.cfg.survey_seconds) {
        g.survey_done = true;
        LOG_INFO("Survey finished");
    }
}

// Runs on the draw thread at every frame boundary.
void frame_boundary() {
    // The cap waits on the game thread or just before Present (here only when neither could be hooked). Its wait
    // falls inside the gap since the previous boundary, so the step below is the capped frame time.
    limiter_frame_boundary();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const LONGLONG t = now.QuadPart;
    g.wait_sum += limiter_take_wait_ms();
    if (g.last != 0) {
        const double dt_ms = ms_between(g.last, t);
        g.last_dt_ms = dt_ms;
        ++g.frames;
        g.dt_sum += dt_ms;
        if (dt_ms < g.dt_min) g.dt_min = dt_ms;
        if (dt_ms > g.dt_max) g.dt_max = dt_ms;
        if (g.driver) {
            float dt = static_cast<float>(dt_ms / 1000.0);
            if (dt < kMinStep) dt = kMinStep;
            if (dt > kMaxStep) dt = kMaxStep;
            *g.step = dt;
            g.written = dt;
        }
        const double step = g.driver ? static_cast<double>(g.written) : 1.0 / 30.0;
        fixes_update(step);
        sfxwatch_tick();
        footik_update(step);
        slidesteer_update(step);
        ghost_frame(step);
        script_sample();
    }
    watch_poll();
#if FPSU_DIAGNOSTICS  // Trace's hotkeys; a release build has no Trace (see fpsu_buildcfg.h)
    if (g.cfg.trace) {
        static const int kRates[4] = {30, 60, 120, 240};
        static const int kKeys[4] = {VK_F4, VK_F5, VK_F6, VK_F7};
        for (int i = 0; i < 4; ++i) {
            if ((GetAsyncKeyState(kKeys[i]) & 0x8000) != 0 && g.rate != kRates[i]) {
                g.rate = kRates[i];
                g.period = g.freq.QuadPart / g.rate;
                if (g.driver) {
                    limiter_set_fps(g.rate);  // taken up by the waiting thread at its next wait
                }
                LOG_INFO("Trace: frame rate set to %d FPS", g.rate);
            }
        }
        trace_frame(g.last ? ms_between(g.last, t) : 0.0, g.rate);
        fixes_poll_hotkeys();
        deep_frame(g.last ? ms_between(g.last, t) : 0.0, g.rate);
        gauge_frame(g.last ? ms_between(g.last, t) : 0.0);
    }
#endif
    g.last = t;
    survey_report(t);
}

int __fastcall hook_getcmd(void* self, void* edx) {
    if (!g_render_thread_id) {
        g_render_thread_id = GetCurrentThreadId();
    }
    // Forward first: if DSfix's FPS unlock is on it writes its own step in here, and
    // ours (written afterwards) is the one the game sees.
    const int cmd = g_orig(self, edx);
    if (cmd >= 0 && cmd < kCommands) {
        ++g.count[cmd];
        ++g_cmd_totals[cmd];
    }
    if (cmd == 2 || cmd == 5) {
        frame_boundary();
        if (profile_enabled()) {
            profile_frame_boundary();
        }
        if (cmd == 2) {
            LARGE_INTEGER stamp{};
            QueryPerformanceCounter(&stamp);
            g.t_cmd2 = stamp.QuadPart;
        }
    } else if (cmd == 4 && g.t_cmd2 != 0) {
        LARGE_INTEGER stamp{};
        QueryPerformanceCounter(&stamp);
        const double span = ms_between(g.t_cmd2, stamp.QuadPart);
        ++g.spans;
        if (profile_enabled()) {
            profile_render_span(span);
        }
        g.span_sum += span;
        if (span > g.span_max) g.span_max = span;
    }
    return cmd;
}

bool make_writable(void* address, size_t size);

char hook_game_frame() {
    limiter_wait_early();
    if (!profile_enabled()) {
        return g_game_frame();
    }
    LARGE_INTEGER a{}, b{};
    QueryPerformanceCounter(&a);
    const char result = g_game_frame();
    QueryPerformanceCounter(&b);
    profile_game_frame(1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(g.freq.QuadPart));
    return result;
}

// The cap's early point. Refuses an unexpected build instead of jumping into the wrong function.
bool install_game_frame_hook() {
    auto* site = reinterpret_cast<uint8_t*>(kGameFrameCall);
    if (site[0] != 0xE8) {
        LOG_ERROR("Game-frame call opcode is %02X, expected E8. The early point is not available.", site[0]);
        return false;
    }
    int32_t rel = 0;
    std::memcpy(&rel, site + 1, sizeof(rel));
    const uint32_t target = kGameFrameCall + 5 + static_cast<uint32_t>(rel);
    if (target != kGameFrameFn) {
        LOG_ERROR("Game-frame call goes to %08X, expected %08X. The early point is not available.", target,
                  kGameFrameFn);
        return false;
    }
    g_game_frame = reinterpret_cast<GameFrameFn>(target);
    const int32_t new_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&hook_game_frame) - (kGameFrameCall + 5));
    if (!make_writable(site + 1, 4)) {
        LOG_ERROR("Could not unprotect the game-frame call (Win32=%lu). The early point is not available.",
                  GetLastError());
        return false;
    }
    std::memcpy(site + 1, &new_rel, sizeof(new_rel));
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    LOG_INFO("FPS cap waits at %08X, before input is read", kGameFrameCall);
    return true;
}

bool make_writable(void* address, size_t size) {
    DWORD old = 0;
    return VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old) != 0;
}

bool apply_vblank_patch() {
    static constexpr uint8_t kPrefix[6] = {0xC7, 0x86, 0x48, 0x02, 0x00, 0x00};
    auto* store = reinterpret_cast<uint8_t*>(kVblankStoreVa);
    if (std::memcmp(store, kPrefix, sizeof(kPrefix)) != 0) {
        LOG_ERROR("Swap-interval store at %08X does not match this build. Not patching.", kVblankStoreVa);
        return false;
    }
    uint32_t imm = 0;
    std::memcpy(&imm, store + 6, sizeof(imm));
    if (imm == 5) {
        LOG_INFO("Swap-interval bookkeeping is already 5 (DSfix patched it first)");
        return true;
    }
    if (imm != 2) {
        LOG_ERROR("Swap-interval immediate is %u, expected 2. Not patching.", imm);
        return false;
    }
    if (!make_writable(store + 6, 4)) {
        LOG_ERROR("Could not unprotect the swap-interval store (Win32=%lu)", GetLastError());
        return false;
    }
    const uint32_t five = 5;
    std::memcpy(store + 6, &five, sizeof(five));
    FlushInstructionCache(GetCurrentProcess(), store, 10);
    LOG_INFO("Swap-interval bookkeeping changed from 2 to 5 at %08X", kVblankImmVa);
    return true;
}

}  // namespace

bool frame_install(const Settings& settings) {
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
    if (target == kGetCmdFn) {
        LOG_INFO("Draw-thread fetch call is unhooked (goes to the game's own getter)");
    } else if (target < kImageBase || target >= kImageBase + kImageSize) {
        LOG_INFO("Draw-thread fetch call is already detoured to %08X; chaining in front of it", target);
    } else {
        LOG_ERROR("Draw-thread fetch call goes to %08X inside the game, expected %08X. Not hooking.", target,
                  kGetCmdFn);
        return false;
    }
    g_orig = reinterpret_cast<GetCmdFn>(target);

    g.cfg = settings;
    QueryPerformanceFrequency(&g.freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    g.start = now.QuadPart;
    g.report = now.QuadPart;
    g.rate = settings.target_fps;
    g.period = g.freq.QuadPart / g.rate;
    g.step = reinterpret_cast<float*>(kTimestepVa);

    if (settings.driver) {
        if (!make_writable(g.step, sizeof(float))) {
            LOG_ERROR("Could not unprotect the timestep constant (Win32=%lu)", GetLastError());
        } else {
            timeBeginPeriod(1);
            g.driver = true;
        }
        if (settings.vblank_patch) {
            apply_vblank_patch();
        }
        if (g.driver) {
            limiter_start(settings.target_fps);
            LimitPoint point = settings.fps_limit_early ? kLimitEarly : kLimitPresent;
            if (point == kLimitEarly && !install_game_frame_hook()) {
                // Never no cap: 1.8.0's point needs only the frame hook below.
                point = kLimitBoundary;
                LOG_ERROR("FPS cap waits when the render thread takes a frame instead (as in 1.8.0)");
            }
            limiter_set_point(point);  // present_install() falls back the same way when it cannot hook Present
        }
    }

    const int32_t new_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&hook_getcmd) - (kDispatchCall + 5));
    if (!make_writable(site + 1, 4)) {
        LOG_ERROR("Could not unprotect the draw-thread fetch call (Win32=%lu)", GetLastError());
        return false;
    }
    std::memcpy(site + 1, &new_rel, sizeof(new_rel));
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    LOG_INFO("Frame hook installed at %08X: driver=%s target=%d FPS vblank_patch=%s survey=%ds",
             kDispatchCall, g.driver ? "on" : "off", settings.target_fps, settings.vblank_patch ? "on" : "off",
             settings.survey_seconds);
    diag_watchdog_start([] { return static_cast<uint64_t>(g_cmd_totals[2]); },
                        [] { return static_cast<float>(g.last_dt_ms); }, script_describe);
    watch_set_spec(settings.watch);
    if (settings.driver) {
        FixFlags flags;
        flags.slide = settings.fix_slide;
        flags.damping = settings.fix_damping;
        flags.timers = settings.fix_timers;
        flags.smoothing = settings.fix_smoothing;
        flags.graze = settings.fix_graze;
        flags.ui = settings.fix_ui;
        flags.velocity = settings.fix_velocity;
        fixes_install(flags);
        ui_install(settings);
        if (settings.fix_step_down) {
            snap_install();
        }
        if (settings.fix_camera) {
            camera_install();
        }
        if (settings.fix_lock_on_turn) {
            turn_install();
        }
        if (settings.fix_foot_ik) {
            footik_install();
        }
        if (settings.fix_ghosts) {
            ghost_install();
        }
        // SfxWatch shares the effect registration hook; a Watch of player fields keeps the debug registers.
        if (settings.sfx_watch > 0 && settings.watch[0] == 0) sfxwatch_set_target(settings.sfx_watch);
        if (settings.fix_sfx_spawn_rate || sfxwatch_wanted()) {
            sfx_install(settings.fix_sfx_spawn_rate);
        }
        if (settings.fix_sfx_actions) {
            sfx_actions_install();
        }
        if (settings.fix_slide_contact) {
            slidecontact_install();
        }
        if (settings.fix_slide_steer) {
            slidesteer_install();
        }
        if (settings.fix_cutscene_camera) {
            cutscenecam_install();
        }
        if (settings.fix_soul_counter) {
            soulcounter_install();
        }
        if (settings.fix_stick_flick) {
            flick_install();
        }
        if (settings.fix_speffect_timing) {
            speffect_install();
        }
        if (settings.fix_bullet_hit_timing) {
            hitrecord_install();
        }
        if (settings.fix_roll_direction) {
            roll_install();
        }
        if (settings.fix_switchback) {
            switchback_install();
        }
    }
    script_install(settings.driver && settings.fix_script_memory);
    if (settings.bullet_trace) {
        bullettrace_install();
    }
    present_install(settings);
    if (settings.profile) {
        profile_start();
    }
    return true;
}
