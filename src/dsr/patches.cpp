#include "patches.h"

#include "diag.h"
#include "log.h"
#include "profile.h"
#include "state.h"
#include "trace.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#pragma comment(lib, "user32.lib")

namespace {

using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
using SimFn = void (*)(void*, float);
using FxFn = void (*)(void*, float);
using RemoFn = void (*)(void*, float, void*);
using MenuFn = uint8_t (*)(void*, int);
using InputFn = void (*)(void*, void*, void*, void*, float);
using LookupFn = void* (*)(void*, int);
using MenuStepFn = void (*)(void*, float, void*);

QpcFn g_qpc = nullptr;
SimFn g_sim = nullptr;
FxFn g_fx = nullptr;
RemoFn g_remo = nullptr;
MenuFn g_menu = nullptr;
MenuStepFn g_common_menu = nullptr;
MenuStepFn g_ingame_menu = nullptr;
MenuStepFn g_title_menu = nullptr;
InputFn g_press = nullptr;
InputFn g_press_alt = nullptr;
InputFn g_repeat = nullptr;
InputFn g_repeat_alt = nullptr;
LookupFn g_press_lookup = nullptr;
LookupFn g_press_alt_lookup = nullptr;
LookupFn g_repeat_lookup = nullptr;
LookupFn g_repeat_alt_lookup = nullptr;

void** g_qpc_iat = nullptr;
void** g_sim_slot = nullptr;
void** g_fx_slot = nullptr;
void** g_remo_slot = nullptr;
void** g_press_slot = nullptr;
void** g_press_alt_slot = nullptr;
void** g_repeat_slot = nullptr;
void** g_repeat_alt_slot = nullptr;
void** g_common_menu_slot = nullptr;
void** g_ingame_menu_slot = nullptr;
void** g_title_menu_slot = nullptr;

std::atomic<int64_t> g_pacer_anchor{0};
std::atomic<int> g_sim_logged{0};
std::atomic<int> g_fx_logged{0};
std::atomic<int> g_remo_logged{0};
std::atomic<int> g_menu_step_logged{0};
std::atomic<int> g_pacer_logged{0};
std::atomic<int> g_repeat_logged{0};
std::atomic<int> g_havok_state{0};
std::atomic<int> g_menu_state{0};
ULONGLONG g_install_ms = 0;
void* g_havok_relay = nullptr;
void* g_menu_trampoline = nullptr;

constexpr int kMaxActions = 256;
constexpr int kMaxSources = 128;
constexpr int kMaxConsumers = 4096;

struct PulseSource {
    void* detector = nullptr;
    uint8_t kind = 0;
    uint32_t bits[kMaxActions / 32]{};
};

struct QueryConsumer {
    void* menu = nullptr;
    int64_t generation = 0;
};

PulseSource g_sources[kMaxSources];
QueryConsumer g_consumers[kMaxConsumers];
std::atomic<int64_t> g_action_generation[kMaxActions];
std::atomic<int64_t> g_action_ms[kMaxActions];
std::atomic<int64_t> g_pulse_generation{0};
std::atomic<int64_t> g_pulse_batch_ms{0};
std::atomic<int> g_pulse_lock{0};
std::atomic<int> g_consumer_lock{0};

void spin_lock(std::atomic<int>& lock) {
    while (lock.exchange(1, std::memory_order_acquire) != 0) {
        Sleep(0);
    }
}

void spin_unlock(std::atomic<int>& lock) {
    lock.store(0, std::memory_order_release);
}

bool executable(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protect = info.Protect & 0xFF;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

bool patch_pointer(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return false;
    }
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), value);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

// The step handed to the game for the frame in flight (see advance_frame_dt): the measured
// frame time, or 1/TargetFPS when VariableFrameTime is off.
float current_step() {
    return g_frame_dt.load(std::memory_order_relaxed);
}

float scaled_frame(float frame_time) {
    return frame_time * 60.0f * current_step();
}

// The retail scheduler hands every displayed frame a 1/60 step, including the
// dormant 140 Hz flipper. Callers that already pass 1/TargetFPS are left alone.
float correct_repeat_time(float frame_time) {
    if (g_scheduler_active.load(std::memory_order_acquire) == 0) {
        return frame_time;
    }
    const float step = current_step();
    const float native_dt = std::fabs(frame_time - (1.0f / 60.0f));
    const float target_dt = std::fabs(frame_time - step);
    const float corrected = native_dt < target_dt ? frame_time * 60.0f * step : frame_time;
    if (g_repeat_logged.exchange(1) == 0) {
        LOG_INFO("Input repeat step: incoming=%.9f corrected=%.9f", frame_time, corrected);
    }
    return corrected;
}

bool pacer_call_matches(uint32_t return_rva, void** qpc_iat) {
    uint8_t* ret = image_rva(return_rva);
    const uint8_t* call = ret - 6;
    if (call[0] != 0xFF || call[1] != 0x15) {
        LOG_ERROR("Pacer call before RVA 0x%X is %02X %02X", return_rva, call[0], call[1]);
        return false;
    }
    int32_t displacement = 0;
    std::memcpy(&displacement, call + 2, sizeof(displacement));
    auto** actual = reinterpret_cast<void**>(ret + displacement);
    if (actual != qpc_iat) {
        LOG_ERROR("Pacer call before RVA 0x%X uses a different QueryPerformanceCounter import",
                  return_rva);
        return false;
    }
    return true;
}

bool validate_static_sites() {
    g_qpc_iat = reinterpret_cast<void**>(image_rva(kBuild.qpc_iat));
    if (!*g_qpc_iat || !executable(*g_qpc_iat) ||
        !pacer_call_matches(kBuild.pacer_first_return, g_qpc_iat) ||
        !pacer_call_matches(kBuild.pacer_loop_return, g_qpc_iat)) {
        LOG_ERROR("Frame pacer import does not match this build");
        return false;
    }

    struct Slot {
        const char* name;
        uint32_t slot;
        uint32_t fn;
    };
    const Slot slots[] = {
        {"simulation", kBuild.sim_vtable, kBuild.sim_fn},
        {"fx", kBuild.fx_vtable, kBuild.fx_fn},
        {"press", kBuild.press_vtable, kBuild.press_fn},
        {"press-alt", kBuild.press_alt_vtable, kBuild.press_alt_fn},
        {"repeat", kBuild.repeat_vtable, kBuild.repeat_fn},
        {"repeat-alt", kBuild.repeat_alt_vtable, kBuild.repeat_alt_fn},
        {"common-menu", kBuild.common_menu_vtable, kBuild.common_menu_fn},
        {"ingame-menu", kBuild.ingame_menu_vtable, kBuild.ingame_menu_fn},
        {"title-menu", kBuild.title_menu_vtable, kBuild.title_menu_fn},
    };
    for (const Slot& slot : slots) {
        void* value = *reinterpret_cast<void**>(image_rva(slot.slot));
        void* expected = image_rva(slot.fn);
        if (value != expected) {
            LOG_ERROR("%s vtable is %p, expected %p", slot.name, value, expected);
            return false;
        }
    }
    return true;
}

// The wait at 0xCE3450 does (qpc - anchor) * [object+0x60] / frequency.
// The gameplay object is constructed with that field set to 60. Scaling the
// counter by TargetFPS/60 makes the same compare wake up at TargetFPS.
BOOL WINAPI hook_qpc(LARGE_INTEGER* counter) {
    const BOOL ok = g_qpc(counter);
    if (!ok || !counter || g_scheduler_active.load(std::memory_order_acquire) == 0) {
        return ok;
    }
    const void* caller = _ReturnAddress();
    if (caller != image_rva(kBuild.pacer_first_return) &&
        caller != image_rva(kBuild.pacer_loop_return)) {
        return ok;
    }

    const int64_t real = counter->QuadPart;
    int64_t anchor = g_pacer_anchor.load(std::memory_order_relaxed);
    if (anchor == 0) {
        int64_t expected = 0;
        if (g_pacer_anchor.compare_exchange_strong(expected, real)) {
            anchor = real;
        } else {
            anchor = g_pacer_anchor.load(std::memory_order_relaxed);
        }
    }
    const int64_t target = g_target_fps.load(std::memory_order_relaxed);
    counter->QuadPart = anchor + (real - anchor) * target / 60;
    if (g_pacer_logged.exchange(1) == 0) {
        LOG_INFO("Frame pacer active at %u FPS", static_cast<unsigned>(target));
    }
    return ok;
}

void* alloc_near(uint8_t* site) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t gran = info.dwAllocationGranularity;
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_image);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_image + dos->e_lfanew);
    uintptr_t cursor = (reinterpret_cast<uintptr_t>(g_image) + nt->OptionalHeader.SizeOfImage + gran - 1) &
                       ~(gran - 1);
    const uintptr_t limit = reinterpret_cast<uintptr_t>(site) + 0x70000000ull;
    for (; cursor < limit; cursor += gran) {
        void* page = VirtualAlloc(reinterpret_cast<void*>(cursor), 0x1000, MEM_RESERVE | MEM_COMMIT,
                                  PAGE_READWRITE);
        if (page) {
            return page;
        }
    }
    return nullptr;
}

// Constants the game reads every frame, recomputed from the frame time by apply_frame_step().
float* g_havok_cell = nullptr;     // Havok step scale (RW page, read by the relay)
float* g_decay_cave = nullptr;     // airborne momentum damp per frame
float* g_grav_cave = nullptr;      // slide gravity per frame
float* g_friction_cave = nullptr;  // slide friction per frame

constexpr float kMinStep = 1.0f / 2000.0f;
constexpr float kMaxStep = 1.0f / 20.0f;

int64_t g_dt_last_qpc = 0;
int64_t g_dt_freq = 0;
std::atomic<int> g_dt_logged{0};

// Computes the step for this frame. With VariableFrameTime on it is the wall-clock time
// since the previous simulation step, clamped so a hitch (loading, alt-tab) becomes one slow
// frame instead of a physics explosion; off, it is the fixed 1/TargetFPS of the original
// design. Called once per frame from the simulation hook.
float advance_frame_dt() {
    const float nominal = 1.0f / static_cast<float>(g_target_fps.load(std::memory_order_relaxed));
    float dt = nominal;
    if (g_variable_dt.load(std::memory_order_relaxed) != 0) {
        if (g_dt_freq == 0) {
            LARGE_INTEGER freq{};
            QueryPerformanceFrequency(&freq);
            g_dt_freq = freq.QuadPart;
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (g_dt_last_qpc != 0 && g_dt_freq != 0) {
            const float sample = static_cast<float>(static_cast<double>(now.QuadPart - g_dt_last_qpc) /
                                                     static_cast<double>(g_dt_freq));
            dt = sample < kMinStep ? kMinStep : (sample > kMaxStep ? kMaxStep : sample);
        }
        g_dt_last_qpc = now.QuadPart;
    }
    g_frame_dt.store(dt, std::memory_order_relaxed);
    if (g_dt_logged.fetch_add(1) == 120) {
        LOG_INFO("Frame step after 120 frames: %.3f ms (%s)", dt * 1000.0f,
                 g_variable_dt.load(std::memory_order_relaxed) != 0 ? "measured" : "fixed");
    }
    return dt;
}

// Pushes the frame time into every cave the game reads.
void apply_frame_step(float dt) {
    const double scale = static_cast<double>(dt) * 60.0;
    if (g_havok_cell) {
        *g_havok_cell = static_cast<float>(scale);
    }
    if (g_decay_cave) {
        *g_decay_cave = static_cast<float>(std::exp(std::log(0.975) * scale));
    }
    if (g_grav_cave) {
        *g_grav_cave = static_cast<float>(0.5 * scale);
    }
    if (g_friction_cave) {
        *g_friction_cave = static_cast<float>(std::exp(std::log(0.65) * scale));
    }
}

// Havok is stepped from a separate call with a fixed 1/60 in xmm1. The call
// target is an Arxan stub, so the scale is applied in a nearby relay and the
// stub is entered with the adjusted register. .text writes during the first
// seconds of startup are rejected, so this runs from the simulation thread.
bool patch_havok() {
    auto* site = image_rva(kBuild.havok_call);
    if (site[0] != 0xE8) {
        LOG_ERROR("Havok call opcode is %02X", site[0]);
        return false;
    }
    int32_t original_disp = 0;
    std::memcpy(&original_disp, site + 1, sizeof(original_disp));
    uint8_t* target = site + 5 + original_disp;
    if (target != image_rva(kBuild.havok_fn)) {
        LOG_ERROR("Havok call target is %p", target);
        return false;
    }
    g_havok_relay = alloc_near(site);
    if (!g_havok_relay) {
        LOG_ERROR("Could not allocate the Havok relay (Win32=%lu)", GetLastError());
        return false;
    }

    // The scale lives in its own read/write page so it can follow the frame time.
    auto* cell = static_cast<float*>(alloc_near(site));
    if (!cell) {
        VirtualFree(g_havok_relay, 0, MEM_RELEASE);
        g_havok_relay = nullptr;
        LOG_ERROR("Could not allocate the Havok scale cell (Win32=%lu)", GetLastError());
        return false;
    }
    const float scale = current_step() * 60.0f;
    *cell = scale;
    uint8_t code[13] = {
        0xF3, 0x0F, 0x59, 0x0D, 0x00, 0x00, 0x00, 0x00,  // mulss xmm1, [rip+disp] (the scale cell)
        0xE9, 0x00, 0x00, 0x00, 0x00,                    // jmp stub
    };
    const intptr_t cell_disp = reinterpret_cast<uint8_t*>(cell) - (static_cast<uint8_t*>(g_havok_relay) + 8);
    const intptr_t jump = target - (static_cast<uint8_t*>(g_havok_relay) + 13);
    if (jump < INT32_MIN || jump > INT32_MAX || cell_disp < INT32_MIN || cell_disp > INT32_MAX) {
        VirtualFree(cell, 0, MEM_RELEASE);
        VirtualFree(g_havok_relay, 0, MEM_RELEASE);
        g_havok_relay = nullptr;
        LOG_ERROR("Havok stub or scale cell is out of range of the relay");
        return false;
    }
    const auto cell_rel = static_cast<int32_t>(cell_disp);
    const auto rel = static_cast<int32_t>(jump);
    std::memcpy(code + 4, &cell_rel, sizeof(cell_rel));
    std::memcpy(code + 9, &rel, sizeof(rel));
    std::memcpy(g_havok_relay, code, sizeof(code));
    DWORD protect = 0;
    if (!VirtualProtect(g_havok_relay, 0x1000, PAGE_EXECUTE_READ, &protect)) {
        VirtualFree(g_havok_relay, 0, MEM_RELEASE);
        g_havok_relay = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_havok_relay, sizeof(code));

    const intptr_t disp = static_cast<uint8_t*>(g_havok_relay) - (site + 5);
    if (disp < INT32_MIN || disp > INT32_MAX) {
        VirtualFree(g_havok_relay, 0, MEM_RELEASE);
        g_havok_relay = nullptr;
        return false;
    }
    const auto rel_disp = static_cast<int32_t>(disp);
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(g_havok_relay, 0, MEM_RELEASE);
        g_havok_relay = nullptr;
        return false;
    }
    std::memcpy(site + 1, &rel_disp, sizeof(rel_disp));
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    DWORD ignored = 0;
    VirtualProtect(site, 5, old, &ignored);
    g_havok_cell = cell;
    LOG_INFO("Havok step scaled by %.6f (follows the frame time)", scale);
    return true;
}

uint8_t hook_menu(void* menu, int action) {
    const uint8_t active = g_menu(menu, action);
    if (active && g_input_log.load(std::memory_order_relaxed) != 0 && ((action >= 0x50 && action <= 0x54) || action == 0x70)) {
        static std::atomic<int> raw_lines{0};
        if (raw_lines.fetch_add(1) < 6000) {
            LOG_INFO("[input] raw action=0x%02X menu=%p", action, menu);
        }
    }
    if (!active || !menu || action < 0 || action >= kMaxActions ||
        g_scheduler_active.load(std::memory_order_acquire) == 0) {
        return active;
    }
    const int64_t generation = g_action_generation[action].load(std::memory_order_relaxed);
    const uint64_t pulse_ms = static_cast<uint64_t>(g_action_ms[action].load(std::memory_order_relaxed));
    const uint64_t now = GetTickCount64();
    if (generation == 0 || now - pulse_ms > 250) {
        return active;
    }

    const size_t hash =
        ((reinterpret_cast<uintptr_t>(menu) >> 4) * 0x9E3779B1u) & (kMaxConsumers - 1);
    QueryConsumer* consumer = nullptr;
    spin_lock(g_consumer_lock);
    for (size_t probe = 0; probe < kMaxConsumers; ++probe) {
        QueryConsumer* candidate = &g_consumers[(hash + probe) & (kMaxConsumers - 1)];
        if (!candidate->menu || candidate->menu == menu) {
            consumer = candidate;
            if (!candidate->menu) {
                candidate->menu = menu;
            }
            break;
        }
    }
    if (!consumer) {
        consumer = &g_consumers[hash];
        consumer->menu = menu;
        consumer->generation = 0;
    }
    const bool suppress = consumer->generation == generation;
    if (!suppress) {
        consumer->generation = generation;
    }
    spin_unlock(g_consumer_lock);
    if (g_input_log.load(std::memory_order_relaxed) != 0 && ((action >= 0x50 && action <= 0x54) || action == 0x70)) {
        LOG_INFO("[input] query action=0x%02X %s", action, suppress ? "suppressed" : "delivered");
    }
    if (suppress) {
        g_menu_suppressed.fetch_add(1, std::memory_order_relaxed);
    }
    return suppress ? 0 : active;
}

bool patch_menu() {
    if (g_menu_filter.load(std::memory_order_relaxed) == 0) {
        LOG_INFO("Menu input filter disabled by MenuInputFilter=false");
        return true;
    }
    auto* target = image_rva(kBuild.menu_query);
    static const uint8_t kPrologue[15] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7,
                                           0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF};
    if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
        LOG_ERROR("Menu query prologue changed");
        return false;
    }
    g_menu_trampoline = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_menu_trampoline) {
        return false;
    }
    auto* trampoline = static_cast<uint8_t*>(g_menu_trampoline);
    std::memcpy(trampoline, target, sizeof(kPrologue));
    uint8_t back[14] = {0xFF, 0x25, 0, 0, 0, 0};
    void* resume = target + sizeof(kPrologue);
    std::memcpy(back + 6, &resume, sizeof(resume));
    std::memcpy(trampoline + sizeof(kPrologue), back, sizeof(back));
    DWORD protect = 0;
    if (!VirtualProtect(trampoline, 0x1000, PAGE_EXECUTE_READ, &protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_menu_trampoline = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, sizeof(kPrologue) + sizeof(back));
    g_menu = reinterpret_cast<MenuFn>(trampoline);

    uint8_t detour[15] = {0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90};
    void* hook = reinterpret_cast<void*>(hook_menu);
    std::memcpy(detour + 6, &hook, sizeof(hook));
    DWORD old = 0;
    if (!VirtualProtect(target, sizeof(detour), PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_menu_trampoline = nullptr;
        g_menu = reinterpret_cast<MenuFn>(target);
        return false;
    }
    std::memcpy(target, detour, sizeof(detour));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(detour));
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(detour), old, &ignored);
    LOG_INFO("Menu input filter active");
    return true;
}

// Grazing a wall, or just sprinting at a high frame rate, used to slam the
// character into a slow run. The check is length(position delta this frame) * 60 < 1,
// and 60 is the retail rate, so a 240 FPS step is four times more likely to fail it.
// The same test at any rate is length * (1 / frame_time) < 1. The 0.8 slowdown and
// 1.2 recovery are per frame, so they are raised to frame_time * 60.
constexpr uint32_t kSpeedMul = 0x379B9B;
constexpr uint32_t kSpeedDecay = 0x379BB9;
constexpr uint32_t kSpeedRecover = 0x379BEC;

float* g_speed_cave = nullptr;
int64_t g_speed_last_qpc = 0;
int64_t g_speed_qpc_freq = 0;
std::atomic<int> g_speed_state{0};
std::atomic<int> g_speed_logged{0};

bool write_disp32(uint8_t* disp, uint8_t* next_ip, const void* target) {
    const intptr_t rel = static_cast<const uint8_t*>(target) - next_ip;
    if (rel < INT32_MIN || rel > INT32_MAX) {
        return false;
    }
    const auto value = static_cast<int32_t>(rel);
    DWORD old = 0;
    if (!VirtualProtect(disp, 4, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(disp, &value, sizeof(value));
    DWORD ignored = 0;
    VirtualProtect(disp, 4, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), disp, 4);
    return true;
}

bool rip_mulss_is(const uint8_t* insn, float expected) {
    if (insn[0] != 0xF3 || insn[1] != 0x0F || insn[2] != 0x59 || insn[3] != 0x05) {
        return false;
    }
    int32_t disp = 0;
    std::memcpy(&disp, insn + 4, sizeof(disp));
    const auto* constant = reinterpret_cast<const float*>(insn + 8 + disp);
    return std::fabs(*constant - expected) < 0.0001f;
}

// HUD gauges (health, stamina, boss and enemy bars). FrpgMenuDlgObjGauge::update moves the
// displayed value toward the real one once per frame, by half the gap while the gap is over 2 and
// by 1.0 otherwise. It has no time step, so at 240 FPS the Estus fill ran four times too fast.
// The 1.0 (movss xmm1,[rip+d]) and 0.5 (mulss xmm1,[rip+d]) loads are retargeted to a pair of
// floats rewritten every frame: 1.0 * n and 1 - 0.5^n, with n = frame time * 60.
constexpr uint32_t kGaugeMin = 0x6664B7;
constexpr uint32_t kGaugeK = 0x6664D6;
float* g_gauge_cave = nullptr;
std::atomic<int> g_gauge_state{0};

bool rip_xmm1_load_is(const uint8_t* insn, uint8_t op, float expected) {
    if (insn[0] != 0xF3 || insn[1] != 0x0F || insn[2] != op || insn[3] != 0x0D) {
        return false;
    }
    int32_t disp = 0;
    std::memcpy(&disp, insn + 4, sizeof(disp));
    const auto* constant = reinterpret_cast<const float*>(insn + 8 + disp);
    return std::fabs(*constant - expected) < 0.0001f;
}

void write_gauge_factors(float dt) {
    if (!g_gauge_cave) {
        return;
    }
    float n = dt * 60.0f;
    if (n < 0.02f) {
        n = 0.02f;
    }
    if (n > 10.0f) {
        n = 10.0f;
    }
    g_gauge_cave[0] = n;
    g_gauge_cave[1] = 1.0f - std::pow(0.5f, n);
}

bool patch_gauge() {
    auto* min_step = image_rva(kGaugeMin);
    auto* factor = image_rva(kGaugeK);
    if (!rip_xmm1_load_is(min_step, 0x10, 1.0f) || !rip_xmm1_load_is(factor, 0x59, 0.5f)) {
        LOG_ERROR("HUD gauge update does not match this build");
        return false;
    }
    void* page = alloc_near(min_step);
    if (!page) {
        LOG_ERROR("Could not allocate the HUD gauge constants (Win32=%lu)", GetLastError());
        return false;
    }
    auto* cave = static_cast<float*>(page);
    cave[0] = 1.0f;
    cave[1] = 0.5f;
    if (!write_disp32(min_step + 4, min_step + 8, cave) || !write_disp32(factor + 4, factor + 8, cave + 1)) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not retarget the HUD gauge constants");
        return false;
    }
    g_gauge_cave = cave;
    LOG_INFO("HUD gauge fill follows the frame time (Estus, stamina and boss bars)");
    return true;
}

// Loading screen swirl. FrpgMenuDlgNowLoading::update (0x6FEC50) adds one to two counters
// ([rdi+0x208], [rdi+0x20C]) every time it is drawn; both wrap at 100 and turn the bonfire swirl,
// so it spun at the frame rate. The three instructions that do it (18 bytes) become a call to a
// stub that adds the number of 1/30 s steps that really elapsed instead.
constexpr uint32_t kSwirlSite = 0x6FED0A;

std::atomic<uint32_t> g_swirl_total{0};

uint32_t swirl_steps() {
    static int64_t freq = 0;
    static int64_t last = 0;
    static double carry = 0.0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (freq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    if (last == 0) {
        last = now.QuadPart;
        return 1;
    }
    double elapsed = static_cast<double>(now.QuadPart - last) / static_cast<double>(freq);
    last = now.QuadPart;
    if (elapsed > 0.25) {
        elapsed = 1.0 / 30.0;
    }
    carry += elapsed * 30.0;
    const uint32_t whole = static_cast<uint32_t>(carry);
    carry -= whole;
    g_swirl_total.fetch_add(whole, std::memory_order_relaxed);
    return whole;
}

std::atomic<int> g_swirl_state{0};

bool patch_swirl() {
    static const uint8_t kOriginal[18] = {0xFF, 0x87, 0x08, 0x02, 0x00, 0x00, 0x8B, 0x8F, 0x08,
                                          0x02, 0x00, 0x00, 0xFF, 0x87, 0x0C, 0x02, 0x00, 0x00};
    uint8_t* site = image_rva(kSwirlSite);
    if (std::memcmp(site, kOriginal, sizeof(kOriginal)) != 0) {
        LOG_ERROR("Loading screen swirl does not match this build");
        return false;
    }
    auto* page = static_cast<uint8_t*>(alloc_near(site));
    if (!page) {
        LOG_ERROR("Could not allocate the loading swirl stub (Win32=%lu)", GetLastError());
        return false;
    }
    size_t n = 0;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            page[n++] = b;
        }
    };
    emit({0x48, 0x83, 0xEC, 0x28});  // sub rsp, 28h
    emit({0x48, 0xB8});              // mov rax, swirl_steps
    const uint64_t func = reinterpret_cast<uint64_t>(&swirl_steps);
    std::memcpy(page + n, &func, 8);
    n += 8;
    emit({0xFF, 0xD0});                                  // call rax
    emit({0x48, 0x83, 0xC4, 0x28});                      // add rsp, 28h
    emit({0x01, 0x87, 0x08, 0x02, 0x00, 0x00});          // add [rdi+208h], eax
    emit({0x01, 0x87, 0x0C, 0x02, 0x00, 0x00});          // add [rdi+20Ch], eax
    emit({0x8B, 0x8F, 0x08, 0x02, 0x00, 0x00});          // mov ecx, [rdi+208h]
    emit({0xC3});                                        // ret
    const intptr_t rel = reinterpret_cast<intptr_t>(page) - reinterpret_cast<intptr_t>(site + 5);
    if (rel < static_cast<intptr_t>(INT32_MIN) || rel > static_cast<intptr_t>(INT32_MAX)) {
        LOG_ERROR("Loading swirl stub is out of range");
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old)) {
        LOG_ERROR("Could not make the loading swirl stub executable (Win32=%lu)", GetLastError());
        return false;
    }
    if (!VirtualProtect(site, sizeof(kOriginal), PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("Could not unprotect the loading swirl (Win32=%lu)", GetLastError());
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, n);
    site[0] = 0xE8;
    const int32_t value = static_cast<int32_t>(rel);
    std::memcpy(site + 1, &value, 4);
    std::memset(site + 5, 0x90, sizeof(kOriginal) - 5);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kOriginal));
    VirtualProtect(site, sizeof(kOriginal), old, &old);
    LOG_INFO("Loading screen swirl follows real time");
    return true;
}

void write_speed_factors(float dt) {
    const float target = static_cast<float>(g_target_fps.load(std::memory_order_relaxed));
    const bool high = target >= 90.0f;
    const float leniency = high ? 1.2f : 1.0f;
    const float recover_base = high ? 1.5f : 1.2f;
    if (dt < 1.0f / 480.0f) {
        dt = 1.0f / 480.0f;
    }
    if (dt > 0.05f) {
        dt = 1.0f / target;
    }
    g_speed_cave[0] = (1.0f / dt) * leniency;
    g_speed_cave[1] = std::exp(std::log(0.8f) * dt * 60.0f);
    g_speed_cave[2] = std::exp(std::log(recover_base) * dt * 60.0f);
}

float sample_frame_dt(bool* measured) {
    if (g_speed_qpc_freq == 0) {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        g_speed_qpc_freq = freq.QuadPart;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const float nominal = 1.0f / static_cast<float>(g_target_fps.load(std::memory_order_relaxed));
    float dt = nominal;
    *measured = false;
    if (g_speed_last_qpc != 0 && g_speed_qpc_freq != 0) {
        const float sample = static_cast<float>(static_cast<double>(now.QuadPart - g_speed_last_qpc) /
                                                 static_cast<double>(g_speed_qpc_freq));
        if (sample >= 1.0f / 480.0f && sample <= 0.05f) {
            dt = sample;
            *measured = true;
        }
    }
    g_speed_last_qpc = now.QuadPart;
    return dt;
}

void update_speed_factors() {
    if (!g_speed_cave) {
        return;
    }
    bool measured = false;
    const float dt = sample_frame_dt(&measured);
    write_speed_factors(dt);
    if (measured && g_speed_logged.exchange(1) == 0) {
        LOG_INFO("Sprint graze using frame time %.3f ms, scale %.3f, decay %.4f, recover %.4f", dt * 1000.0f,
                 g_speed_cave[0], g_speed_cave[1], g_speed_cave[2]);
    }
}

bool patch_speed() {
    auto* mul = image_rva(kSpeedMul);
    auto* decay = image_rva(kSpeedDecay);
    auto* recover = image_rva(kSpeedRecover);
    if (!rip_mulss_is(mul, 60.0f) || !rip_mulss_is(decay, 0.8f) || !rip_mulss_is(recover, 1.2f)) {
        LOG_ERROR("Sprint graze check does not match this build");
        return false;
    }
    void* page = alloc_near(mul);
    if (!page) {
        LOG_ERROR("Could not allocate sprint constants (Win32=%lu)", GetLastError());
        return false;
    }
    g_speed_cave = static_cast<float*>(page);
    write_speed_factors(1.0f / static_cast<float>(g_target_fps.load(std::memory_order_relaxed)));
    if (!write_disp32(mul + 4, mul + 8, g_speed_cave) ||
        !write_disp32(decay + 4, decay + 8, g_speed_cave + 1) ||
        !write_disp32(recover + 4, recover + 8, g_speed_cave + 2)) {
        g_speed_cave = nullptr;
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not retarget the sprint graze multiplies");
        return false;
    }
    LOG_INFO("Sprint graze check retargeted (vanilla multiplier 60, decay 0.8, recover 1.2)");
    return true;
}

// Airborne momentum: fn 0x2bbe40 damps body velocity by 0.975 per FRAME,
// loaded from [body+0xf0] at 0x2bc24b. Retail only ran at sixty frames a
// second, so 0.975 per frame was 0.975 per 1/60 s of wall time. At TargetFPS
// the same per-frame factor decays that much faster per wall second - four
// times faster at 240 - which drains walk-off momentum and the back half of
// a jump before it can play out. Point the load at a cave float holding
// 0.975 ^ (60 / TargetFPS): exactly 0.975 at 60 FPS, and the factor raised
// to TargetFPS equals 0.975 raised to 60 per second, so the wall-time decay
// matches retail at any target.
constexpr uint32_t kDecaySite = 0x2BC24B;

std::atomic<int> g_decay_state{0};

bool patch_decay() {
    auto* site = image_rva(kDecaySite);
    // movss xmm3, dword ptr [rbx + 0xf0] (f3 0f 10 9b f0 00 00 00), preceded
    // by xorps xmm1, xmm1 and followed by movaps xmm2, xmm5.
    static constexpr uint8_t kExpected[] = {0xF3, 0x0F, 0x10, 0x9B, 0xF0, 0x00, 0x00, 0x00};
    if (site[-3] != 0x0F || site[-2] != 0x57 || site[-1] != 0xC9 ||
        std::memcmp(site, kExpected, sizeof(kExpected)) != 0 || site[8] != 0x0F || site[9] != 0x28 ||
        site[10] != 0xD5) {
        LOG_ERROR("Velocity damp load does not match this build");
        return false;
    }
    void* page = alloc_near(site);
    if (!page) {
        LOG_ERROR("Could not allocate the damp constant (Win32=%lu)", GetLastError());
        return false;
    }
    auto* factor = static_cast<float*>(page);
    *factor = static_cast<float>(std::exp(std::log(0.975) * 60.0 * static_cast<double>(current_step())));

    // Same length both ways: [rbx + disp32] becomes [rip + disp32] by
    // changing only the ModRM byte (mod=00, reg=xmm3, rm=101), then the
    // displacement is retargeted at the cave.
    DWORD old = 0;
    if (!VirtualProtect(site + 3, 1, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not unprotect the velocity damp load (Win32=%lu)", GetLastError());
        return false;
    }
    site[3] = 0x1D;
    DWORD ignored = 0;
    VirtualProtect(site + 3, 1, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site + 3, 1);
    if (!write_disp32(site + 4, site + 8, factor)) {
        site[3] = 0x9B;
        FlushInstructionCache(GetCurrentProcess(), site + 3, 1);
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not retarget the velocity damp load");
        return false;
    }
    g_decay_cave = factor;
    LOG_INFO("Velocity damp retargeted (per-frame factor %.6f instead of 0.975, follows the frame time)", *factor);
    return true;
}

// Ground slide: the same controller step (fn 0x2bbe40) keeps a slide velocity in
// [body+0x120] that is fed into the velocity handed to the physics proxy.
//  * While the body touches a slope, 0x2BBF5F adds a tangential gravity term of
//    0.5 (u/s) per FRAME (it is projected off the contact normal, so it is zero
//    on flat floor and non-zero on slopes, lips and curves).
//  * While it is not touching, 0x2BC094 multiplies the slide velocity by 0.65
//    per FRAME.
// Neither uses dt, so at TargetFPS they run TargetFPS/60 times too often: slopes
// and lips build downward speed several times faster (the character "snaps" off
// them), and leftover slide speed dies several times faster once airborne.
// Point both loads at cave floats holding the same amounts per 1/60 s:
// 0.5 * 60/TargetFPS and 0.65 ^ (60/TargetFPS).
constexpr uint32_t kSlideGravSite = 0x2BBF5F;
constexpr uint32_t kSlideFrictionSite = 0x2BC094;

std::atomic<int> g_slide_state{0};

// `movss xmmN, dword ptr [rip + disp32]` (F3 0F 10 modrm, mod=00 rm=101) whose
// current value is `expected`; retargets the load at a cave float.
bool retarget_movss_const(uint32_t rva, float expected, float replacement, const char* what, float** cave_out) {
    auto* site = image_rva(rva);
    if (site[0] != 0xF3 || site[1] != 0x0F || site[2] != 0x10 || (site[3] & 0xC7) != 0x05) {
        LOG_ERROR("%s load at 0x%08X does not match this build", what, rva);
        return false;
    }
    int32_t disp = 0;
    std::memcpy(&disp, site + 4, sizeof(disp));
    const auto* constant = reinterpret_cast<const float*>(site + 8 + disp);
    if (std::fabs(*constant - expected) > 0.0001f) {
        LOG_ERROR("%s constant at 0x%08X is %.6f, expected %.6f", what, rva, *constant, expected);
        return false;
    }
    void* page = alloc_near(site);
    if (!page) {
        LOG_ERROR("Could not allocate the %s constant (Win32=%lu)", what, GetLastError());
        return false;
    }
    *static_cast<float*>(page) = replacement;
    if (!write_disp32(site + 4, site + 8, page)) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not retarget the %s load", what);
        return false;
    }
    if (cave_out) {
        *cave_out = static_cast<float*>(page);
    }
    return true;
}

bool patch_slide() {
    const double scale = 60.0 * static_cast<double>(current_step());
    const float grav = static_cast<float>(0.5 * scale);
    const float friction = static_cast<float>(std::exp(std::log(0.65) * scale));
    float* grav_cave = nullptr;
    float* friction_cave = nullptr;
    if (!retarget_movss_const(kSlideGravSite, 0.5f, grav, "slide gravity", &grav_cave) ||
        !retarget_movss_const(kSlideFrictionSite, 0.65f, friction, "slide friction", &friction_cave)) {
        return false;
    }
    g_grav_cave = grav_cave;
    g_friction_cave = friction_cave;
    LOG_INFO("Ground slide retargeted (gravity %.5f per frame instead of 0.5, friction %.5f instead of 0.65)", grav,
             friction);
    return true;
}

struct CaveEmitter {
    uint8_t* buf;
    size_t len = 0;

    void put(const void* p, size_t n) {
        std::memcpy(buf + len, p, n);
        len += n;
    }
    void b(uint8_t v) {
        put(&v, 1);
    }
    void d(uint32_t v) {
        put(&v, 4);
    }
    void q(uint64_t v) {
        put(&v, 8);
    }
    size_t imm32() {
        const size_t at = len;
        d(0);
        return at;
    }
    size_t imm64() {
        const size_t at = len;
        q(0);
        return at;
    }
    void fix32(size_t at, const void* target) {
        const auto rel = static_cast<int32_t>(static_cast<const uint8_t*>(target) - (buf + at + 4));
        std::memcpy(buf + at, &rel, sizeof(rel));
    }
    void fix64(size_t at, uint64_t value) {
        std::memcpy(buf + at, &value, sizeof(value));
    }
};

// Ground snap ("step down"). Every frame the physics body is lifted by the step
// height [phys+0xE4] (0.31), moved, then snapped back down onto ground within
// reach (fn 0x2BCB00, applied by setPosition at 0x2BC7E9). Reach below the lifted
// position is [phys+0xE4] + [phys+0x228]; [phys+0x228] (0.4) is the most the body
// can be pulled down in ONE FRAME.
//
// That 0.4 plays two roles that only coincide at 60 FPS:
//  * a geometric step-down height (walking off a 0.2 tread must be caught in one
//    frame at any frame rate), and
//  * a rate limit: a surface that drops away faster than 0.4 per frame (24 u/s)
//    lets the character leave the ground. At TargetFPS the same surface drops
//    1/N as much per frame, so the character stays glued to a curved lip and
//    rides it down at up to 0.4/dt - the drop boost at every walk-off.
//
// Scaling the field by 60/TargetFPS fixed the boost but broke role one: steps
// were no longer caught in a frame and the player fell for a few frames. Retail's
// actual rule is "the body may drop at most 0.4 over one 1/60 s frame". Emulate
// exactly that: keep a short history of each frame's net descent and give the next
// frame a reach of (0.4 * tolerance - descent over the last k-1 frames), capped at
// the retail 0.4, where k = frames per 1/60 s (4 at 240 FPS, 1 at 60/61). A fresh
// step-down has no recent descent, so it gets the full retail reach; a lip that has
// already used up its budget over the last 1/60 s is released, at retail's rate.
// The sliding window is slightly stricter than retail's frame-grid-aligned one, so
// a small tolerance keeps borderline slopes attached the way they are at 61 FPS.
//
// The field is written once by a setter that does not run every frame, so it is
// managed from the snap-apply cave, which runs every frame with real dt: the
// original ("base") value is remembered per physics object and the scaled or full
// value is written back for the next frame's ground check.
constexpr uint32_t kSnapApplySite = 0x2BC7E9;
constexpr uint32_t kReachOffset = 0x228;

std::atomic<int> g_step_down_state{0};

struct GroundState {
    std::atomic<const void*> phys{nullptr};
    float base = 0.0f;
    float written = -1.0f;
    float drop[8] = {};  // net descent per recent frame (>= 0), newest at (head - 1)
    unsigned head = 0;
    ULONGLONG last_ms = 0;
};
constexpr size_t kGroundSlots = 512;
GroundState g_ground[kGroundSlots];

GroundState* ground_slot(const void* phys, ULONGLONG now) {
    const size_t start = ((reinterpret_cast<uintptr_t>(phys) >> 4) * 2654435761u) & (kGroundSlots - 1);
    for (size_t n = 0; n < 64; ++n) {
        GroundState& s = g_ground[(start + n) & (kGroundSlots - 1)];
        const void* cur = s.phys.load(std::memory_order_acquire);
        if (cur == phys) {
            return &s;
        }
        // Claim a free slot, or one whose object has not updated for a minute.
        if (!cur || now - s.last_ms > 60000) {
            const void* expected = cur;
            if (s.phys.compare_exchange_strong(expected, phys)) {
                s.base = 0.0f;
                s.written = -1.0f;
                std::memset(s.drop, 0, sizeof(s.drop));
                s.head = 0;
                s.last_ms = now;
                return &s;
            }
        }
    }
    return nullptr;
}

// Ladder exit: the same snap is applied unbalanced (lift skipped) at the bottom of
// a ladder slide, pulling the body down 0.31 per FRAME through the collision
// floor until it reaches a lower one (2+ units at 240 FPS). Where 0x1F4 is set and
// the proxy is not ~0.31 above the frame-start height, cap the snap at one retail
// frame's worth (0.31) per episode, spread over frames by 60*dt.
struct SnapEpisode {
    const void* phys = nullptr;
    float applied = 0.0f;
    ULONGLONG last_ms = 0;
};
SnapEpisode g_snap_episodes[8];

float ladder_snap_factor(const void* phys, float dt, float lift_height) {
    const ULONGLONG now = GetTickCount64();
    SnapEpisode* slot = nullptr;
    SnapEpisode* stale = &g_snap_episodes[0];
    for (auto& s : g_snap_episodes) {
        if (s.phys == phys) {
            slot = &s;
            break;
        }
        if (s.last_ms < stale->last_ms) {
            stale = &s;
        }
    }
    if (!slot) {
        slot = stale;
        slot->phys = phys;
        slot->applied = 0.0f;
    }
    if (lift_height >= 0.15f) {  // the lift happened: balanced frame, new episode next time
        slot->applied = 0.0f;
        slot->last_ms = now;
        return 1.0f;
    }
    if (now - slot->last_ms > 250) {
        slot->applied = 0.0f;
    }
    slot->last_ms = now;
    constexpr float kStep = 0.31f;
    constexpr float kCap = 0.31f;
    float factor = dt * 60.0f;
    if (factor > 1.0f) {
        factor = 1.0f;
    }
    const float remaining = kCap - slot->applied;
    if (remaining <= 0.0f) {
        return 0.0f;
    }
    if (kStep * factor > remaining) {
        factor = remaining / kStep;
    }
    slot->applied += kStep * factor;
    return factor;
}

// Called by the snap-apply cave once per physics update, right before the snap is
// added to the proxy position. `snap` is the vertical offset about to be applied.
void ground_snap_step(const void* phys, float dt, float proxy_y, float start_y, float* snap) {
    auto* bytes = static_cast<const uint8_t*>(phys);
    const ULONGLONG now = GetTickCount64();

    if (bytes[0x1F4] != 0) {
        *snap *= ladder_snap_factor(phys, dt, proxy_y - start_y);
    }

    GroundState* state = ground_slot(phys, now);
    if (!state) {
        return;
    }
    // Net height change this frame: proxy after the move, plus the snap, versus
    // where the body started the frame. (The lift is already inside proxy_y.)
    const float net = proxy_y + *snap - start_y;
    state->last_ms = now;
    state->drop[state->head & 7] = net < 0.0f ? -net : 0.0f;
    state->head++;

    auto* reach = reinterpret_cast<float*>(const_cast<uint8_t*>(bytes) + kReachOffset);
    const float current = *reach;
    if (current != state->written) {
        state->base = current;  // the game (re)wrote it: that is the retail value
    }
    // Frames per retail 1/60 s frame at this frame time (1 at <= 60 FPS).
    int window = dt > 0.0f ? static_cast<int>(1.0f / (60.0f * dt) + 0.5f) : 1;
    window = window < 1 ? 1 : (window > 8 ? 8 : window);
    float recent = 0.0f;  // descent over the last (window - 1) frames, incl. the one just done
    for (int i = 0; i < window - 1; ++i) {
        recent += state->drop[(state->head - 1 - i) & 7];
    }
    constexpr float kTolerance = 1.2f;
    float allowed = state->base * kTolerance - recent;
    if (allowed > state->base) {
        allowed = state->base;
    }
    const float floor_reach = state->base * 0.05f;
    if (allowed < floor_reach) {
        allowed = floor_reach;
    }
    state->written = allowed;
    *reach = state->written;
}

bool patch_ground_snap() {
    auto* site = image_rva(kSnapApplySite);
    // movss xmm1, [rbx+0xC4] followed by xorps xmm3, xmm3
    static constexpr uint8_t kExpected[11] = {0xF3, 0x0F, 0x10, 0x8B, 0xC4, 0x00, 0x00, 0x00,
                                              0x0F, 0x57, 0xDB};
    if (std::memcmp(site, kExpected, sizeof(kExpected)) != 0) {
        LOG_ERROR("Snap apply site does not match this build");
        return false;
    }
    auto* page = static_cast<uint8_t*>(alloc_near(site));
    if (!page) {
        LOG_ERROR("Could not allocate the snap cave (Win32=%lu)", GetLastError());
        return false;
    }
    CaveEmitter e{page};
    for (uint8_t b : {0xF3, 0x0F, 0x10, 0x8B, 0xC4, 0x00, 0x00, 0x00}) e.b(b);   // movss xmm1,[rbx+0xC4]
    for (uint8_t b : {0x48, 0x83, 0xEC, 0x40}) e.b(b);                            // sub rsp,0x40 (16-aligned here)
    for (uint8_t b : {0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x30}) e.b(b);                // movss [rsp+0x30],xmm1 (snap)
    for (uint8_t b : {0x48, 0x89, 0xD9}) e.b(b);                                  // mov rcx,rbx           (phys)
    for (uint8_t b : {0xF3, 0x0F, 0x10, 0x4F, 0x08}) e.b(b);                      // movss xmm1,[rdi+8]    (dt)
    for (uint8_t b : {0xF3, 0x0F, 0x10, 0x54, 0x24, 0x64}) e.b(b);                // movss xmm2,[rsp+0x64] (proxy y, was rsp+0x24)
    for (uint8_t b : {0xF3, 0x0F, 0x10, 0x5B, 0x14}) e.b(b);                      // movss xmm3,[rbx+0x14] (frame-start y)
    for (uint8_t b : {0x48, 0x8D, 0x44, 0x24, 0x30}) e.b(b);                      // lea rax,[rsp+0x30]
    for (uint8_t b : {0x48, 0x89, 0x44, 0x24, 0x20}) e.b(b);                      // mov [rsp+0x20],rax    (5th arg: &snap)
    e.b(0x48); e.b(0xB8);                                                         // mov rax, ground_snap_step
    const size_t fn = e.imm64();
    e.fix64(fn, reinterpret_cast<uint64_t>(&ground_snap_step));
    e.b(0xFF); e.b(0xD0);                                                         // call rax
    for (uint8_t b : {0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x30}) e.b(b);                // movss xmm1,[rsp+0x30] (snap, maybe scaled)
    for (uint8_t b : {0x48, 0x83, 0xC4, 0x40}) e.b(b);                            // add rsp,0x40
    e.b(0xE9);                                                                    // jmp back
    const size_t jb = e.imm32();
    e.fix32(jb, site + 8);
    if (e.len > 0x100) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Snap cave did not fit");
        return false;
    }
    DWORD protect = 0;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &protect)) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not protect the snap cave (Win32=%lu)", GetLastError());
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);

    const intptr_t rel = page - (site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        VirtualFree(page, 0, MEM_RELEASE);
        return false;
    }
    uint8_t patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const auto rel32 = static_cast<int32_t>(rel);
    std::memcpy(patch + 1, &rel32, sizeof(rel32));
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not unprotect the snap apply site (Win32=%lu)", GetLastError());
        return false;
    }
    std::memcpy(site, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(site, sizeof(patch), old, &ignored);
    LOG_INFO("Ground snap managed per frame (chained-descent reach scaling, ladder-exit cap)");
    return true;
}

// Move-control dispatch: two call paths load a hardcoded 1/60 into xmm1 right
// before the action functions run, so anything they scale by dt (ladder slide
// speed, for one) moves 4x too far per frame at 240 FPS. In both places the real
// frame time is already sitting in xmm6 (saved from the incoming xmm1), so the
// 8-byte `movss xmm1, [rip+disp]` becomes `movaps xmm1, xmm6` plus NOPs.
constexpr uint32_t kMoveDtSites[] = {0x379D94, 0x379DF2};

std::atomic<int> g_move_dt_state{0};

bool patch_move_dt() {
    if (g_fix_move_dt.load(std::memory_order_relaxed) == 0) {
        LOG_INFO("Move-control dt fix disabled by FixMoveDt=false");
        return true;
    }
    for (uint32_t rva : kMoveDtSites) {
        auto* site = image_rva(rva);
        if (site[0] != 0xF3 || site[1] != 0x0F || site[2] != 0x10 || site[3] != 0x0D) {
            LOG_ERROR("Move-control dt load at 0x%08X does not match this build", rva);
            return false;
        }
        int32_t disp = 0;
        std::memcpy(&disp, site + 4, sizeof(disp));
        const auto* constant = reinterpret_cast<const float*>(site + 8 + disp);
        if (std::fabs(*constant - 1.0f / 60.0f) > 0.000001f) {
            LOG_ERROR("Move-control dt load at 0x%08X is not 1/60", rva);
            return false;
        }
    }
    static constexpr uint8_t kPatch[8] = {0x0F, 0x10, 0xCE, 0x90, 0x90, 0x90, 0x90, 0x90};
    for (uint32_t rva : kMoveDtSites) {
        auto* site = image_rva(rva);
        DWORD old = 0;
        if (!VirtualProtect(site, sizeof(kPatch), PAGE_EXECUTE_READWRITE, &old)) {
            LOG_ERROR("Could not unprotect the move-control dt load (Win32=%lu)", GetLastError());
            return false;
        }
        std::memcpy(site, kPatch, sizeof(kPatch));
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(kPatch));
        DWORD ignored = 0;
        VirtualProtect(site, sizeof(kPatch), old, &ignored);
    }
    LOG_INFO("Move-control dispatch now uses the real frame time (2 sites)");
    return true;
}

// Movement tracer, only installed when the INI asks for Trace=1.
//
// ChrIns::Update(chr, dt) runs once per displayed frame with the real (already
// scaled) dt, so anything inside it that assumes sixty steps a second shows up
// as a per-frame error at a high target FPS. The cave logs the character,
// move-control and physics state immediately before and after the original
// call: the difference between a pre row and the previous post row is what the
// physics step did, and the difference between a pre row and its own post row
// is what the update itself did. Three call sites are retargeted at the cave.
constexpr uint32_t kChrUpdate = 0x320AE0;
constexpr uint32_t kChrUpdateSites[] = {0x36FBBD, 0x36FD49, 0x36FF2A};

std::atomic<int> g_trace_state{0};

bool build_trace_cave(uint8_t* page, const void* update_fn) {
    CaveEmitter e{page};
    e.b(0x50);        // push rax
    e.b(0x51);        // push rcx
    e.b(0x52);        // push rdx
    e.b(0x41); e.b(0x50);   // push r8
    e.b(0x41); e.b(0x51);   // push r9
    e.b(0x41); e.b(0x52);   // push r10
    e.b(0x41); e.b(0x53);   // push r11
    e.b(0x48); e.b(0x81); e.b(0xEC); e.d(0xC0);                  // sub rsp, 0xC0
    e.b(0x48); e.b(0x89); e.b(0x8C); e.b(0x24); e.d(0xB0);       // mov [rsp+0xB0], rcx   (chr)
    e.b(0x48); e.b(0x8B); e.b(0x84); e.b(0x24); e.d(0xF8);       // mov rax, [rsp+0xF8]   (return address)
    e.b(0x48); e.b(0x83); e.b(0xE8); e.b(0x05);                  // sub rax, 5            (call site)
    e.b(0x48); e.b(0x89); e.b(0x84); e.b(0x24); e.d(0xB8);       // mov [rsp+0xB8], rax
    for (int n = 0; n < 8; ++n) {                                // movdqu [rsp+0x30+n*0x10], xmmN
        e.b(0xF3); e.b(0x0F); e.b(0x7F);
        e.b(static_cast<uint8_t>(0x84 | (n << 3))); e.b(0x24);
        e.d(static_cast<uint32_t>(0x30 + n * 0x10));
    }
    // trace_log(chr, dt, site, 0). MSVC maps parameters positionally: arg3
    // (site) lands in r8 and arg4 (phase) in r9, not rdx/r8.
    e.b(0x48); e.b(0x8B); e.b(0x8C); e.b(0x24); e.d(0xB0);       // mov rcx, [rsp+0xB0]
    e.b(0xF3); e.b(0x0F); e.b(0x10); e.b(0x4C); e.b(0x24); e.b(0x40);  // movss xmm1, [rsp+0x40]
    e.b(0x4C); e.b(0x8B); e.b(0x84); e.b(0x24); e.d(0xB8);       // mov r8, [rsp+0xB8]
    e.b(0x45); e.b(0x31); e.b(0xC9);                              // xor r9d, r9d
    e.b(0x48); e.b(0xB8); const size_t fn_pre = e.imm64();        // mov rax, trace_log
    e.b(0xFF); e.b(0xD0);                                         // call rax
    // ChrIns::Update(chr, dt)
    e.b(0x48); e.b(0x8B); e.b(0x8C); e.b(0x24); e.d(0xB0);
    e.b(0xF3); e.b(0x0F); e.b(0x10); e.b(0x4C); e.b(0x24); e.b(0x40);
    e.b(0xE8); const size_t call_update = e.imm32();               // call ChrIns::Update
    // trace_log(chr, dt, site, 1)
    e.b(0x48); e.b(0x8B); e.b(0x8C); e.b(0x24); e.d(0xB0);
    e.b(0xF3); e.b(0x0F); e.b(0x10); e.b(0x4C); e.b(0x24); e.b(0x40);
    e.b(0x4C); e.b(0x8B); e.b(0x84); e.b(0x24); e.d(0xB8);   // mov r8, [rsp+0xB8]
    e.b(0x41); e.b(0xB9); e.d(1);                              // mov r9d, 1
    e.b(0x48); e.b(0xB8); const size_t fn_post = e.imm64();
    e.b(0xFF); e.b(0xD0);
    for (int n = 0; n < 8; ++n) {                                  // movdqu xmmN, [rsp+0x30+n*0x10]
        e.b(0xF3); e.b(0x0F); e.b(0x6F);
        e.b(static_cast<uint8_t>(0x84 | (n << 3))); e.b(0x24);
        e.d(static_cast<uint32_t>(0x30 + n * 0x10));
    }
    e.b(0x48); e.b(0x81); e.b(0xC4); e.d(0xC0);                    // add rsp, 0xC0
    e.b(0x41); e.b(0x5B); e.b(0x41); e.b(0x5A);                    // pop r11, r10
    e.b(0x41); e.b(0x59); e.b(0x41); e.b(0x58);                    // pop r9, r8
    e.b(0x5A); e.b(0x59); e.b(0x58);                               // pop rdx, rcx, rax
    e.b(0xC3);                                                     // ret
    if (e.len > 0xF00) {
        return false;
    }
    e.fix32(call_update, update_fn);
    e.fix64(fn_pre, reinterpret_cast<uint64_t>(&trace_log));
    e.fix64(fn_post, reinterpret_cast<uint64_t>(&trace_log));
    return true;
}

bool patch_trace() {
    auto* update = image_rva(kChrUpdate);
    if (std::memcmp(update, "\x48\x89\x5C\x24\x18", 5) != 0) {
        LOG_ERROR("ChrIns::Update prologue does not match this build");
        return false;
    }
    for (uint32_t rva : kChrUpdateSites) {
        auto* site = image_rva(rva);
        if (site[0] != 0xE8) {
            LOG_ERROR("Chr update call at 0x%08X has opcode %02X", rva, site[0]);
            return false;
        }
        int32_t disp = 0;
        std::memcpy(&disp, site + 1, sizeof(disp));
        if (site + 5 + disp != update) {
            LOG_ERROR("Chr update call at 0x%08X has an unexpected target", rva);
            return false;
        }
    }
    uint8_t* page = static_cast<uint8_t*>(alloc_near(image_rva(kChrUpdateSites[0])));
    if (!page) {
        LOG_ERROR("Could not allocate the movement trace cave (Win32=%lu)", GetLastError());
        return false;
    }
    if (!build_trace_cave(page, update)) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Movement trace cave did not fit");
        return false;
    }
    DWORD protect = 0;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &protect)) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not protect the movement trace cave (Win32=%lu)", GetLastError());
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    for (uint32_t rva : kChrUpdateSites) {
        auto* site = image_rva(rva);
        if (!write_disp32(site + 1, site + 5, page)) {
            LOG_ERROR("Could not retarget the Chr update call at 0x%08X", rva);
            return false;
        }
    }
    LOG_INFO("Movement trace active (3 ChrIns::Update call sites)");
    return true;
}

// Lock-on camera. ChrFollowCam::Update (0x236C60; rcx = camera, xmm1 = step, r8 = character) smooths
// the camera toward its target with blend weights applied once per frame and tuned for 60 FPS:
// [camera+0x23C] for the lock-on yaw/pitch state and [camera+0x1BC] for the orientation filter
// (both 0.1 in the retail constructor). At a higher frame rate the same per-frame weights finish
// a pan in a fraction of the time, so switching targets snaps. The weights are replaced before
// every update by the equivalent for this frame's step: k applied n = step * 60 times is
// 1 - (1 - k)^n. Two call sites, both retargeted at a stub that does this and forwards.
constexpr uint32_t kFollowUpdate = 0x236C60;
constexpr uint32_t kFollowCallSites[] = {0x23518C, 0x23527A};
constexpr uint32_t kFollowFields[] = {0x23C, 0x1BC};

using FollowFn = void (*)(void*, float, void*, void*);
FollowFn g_follow = nullptr;
std::atomic<int> g_camera_state{0};

struct FollowField {
    float original = 0.0f;
    float written = -1.0f;
};
struct FollowCamera {
    void* object = nullptr;
    FollowField field[2];
    bool logged = false;
};
FollowCamera g_follow_cameras[4];

FollowCamera* follow_slot(void* object) {
    FollowCamera* free_slot = nullptr;
    for (auto& c : g_follow_cameras) {
        if (c.object == object) {
            return &c;
        }
        if (!c.object && !free_slot) {
            free_slot = &c;
        }
    }
    if (!free_slot) {
        free_slot = &g_follow_cameras[0];
    }
    *free_slot = FollowCamera();
    free_slot->object = object;
    return free_slot;
}

float scaled_weight(float original, double n) {
    if (!(original > 0.0f) || original >= 1.0f) {
        return original;
    }
    if (original <= 0.5f) {
        return static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(original), n));
    }
    return static_cast<float>(std::pow(static_cast<double>(original), n));
}

bool follow_read(void* object, uint32_t offset, float* out) {
    __try {
        *out = *reinterpret_cast<volatile float*>(static_cast<uint8_t*>(object) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool follow_write(void* object, uint32_t offset, float value) {
    __try {
        *reinterpret_cast<volatile float*>(static_cast<uint8_t*>(object) + offset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void scale_follow_camera(void* object, float step) {
    if (!object) {
        return;
    }
    // n = how many "frames" of the tuned rate this step covers. Remastered keeps the weights it
    // inherited from the 30 FPS game, so at its native 60 FPS the pan is twice as fast as the
    // original; CameraPtdeSpeed evaluates them at 30 frames a second instead.
    const double tuned_rate = g_camera_ptde_speed.load(std::memory_order_relaxed) != 0 ? 30.0 : 60.0;
    double n = static_cast<double>(step) * tuned_rate;
    if (n < 0.02) {
        n = 0.02;
    }
    if (n > 10.0) {
        n = 10.0;
    }
    FollowCamera* cam = follow_slot(object);
    for (int i = 0; i < 2; ++i) {
        float value = 0.0f;
        if (!follow_read(object, kFollowFields[i], &value)) {
            return;
        }
        FollowField& f = cam->field[i];
        if (value != f.written) {
            f.original = value;  // the game (re)wrote it: that is the 60 FPS value
        }
        const float target = scaled_weight(f.original, n);
        f.written = target;
        follow_write(object, kFollowFields[i], target);
    }
    if (!cam->logged) {
        cam->logged = true;
        LOG_INFO("Follow camera %p: blend weights +0x23C=%.4f +0x1BC=%.4f (retail values)", object,
                 cam->field[0].original, cam->field[1].original);
    }
}

void hook_follow(void* camera, float step, void* chr, void* extra) {
    scale_follow_camera(camera, step);
    g_follow(camera, step, chr, extra);
}

bool patch_camera() {
    if (g_fix_camera.load(std::memory_order_relaxed) == 0) {
        LOG_INFO("Lock-on camera fix disabled by FixCamera=false");
        return true;
    }
    for (uint32_t rva : kFollowCallSites) {
        auto* site = image_rva(rva);
        int32_t disp = 0;
        std::memcpy(&disp, site + 1, sizeof(disp));
        if (site[0] != 0xE8 || site + 5 + disp != image_rva(kFollowUpdate)) {
            LOG_ERROR("Follow camera call at 0x%08X does not match this build", rva);
            return false;
        }
    }
    auto* page = static_cast<uint8_t*>(alloc_near(image_rva(kFollowCallSites[0])));
    if (!page) {
        LOG_ERROR("Could not allocate the follow camera stub (Win32=%lu)", GetLastError());
        return false;
    }
    // jmp [rip+0] ; the address of hook_follow
    const uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* target = reinterpret_cast<void*>(hook_follow);
    std::memcpy(page, jump, sizeof(jump));
    std::memcpy(page + sizeof(jump), &target, sizeof(target));
    DWORD protect = 0;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &protect)) {
        VirtualFree(page, 0, MEM_RELEASE);
        LOG_ERROR("Could not protect the follow camera stub (Win32=%lu)", GetLastError());
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, sizeof(jump) + sizeof(target));
    g_follow = reinterpret_cast<FollowFn>(image_rva(kFollowUpdate));
    for (uint32_t rva : kFollowCallSites) {
        auto* site = image_rva(rva);
        if (!write_disp32(site + 1, site + 5, page)) {
            LOG_ERROR("Could not retarget the follow camera call at 0x%08X", rva);
            return false;
        }
    }
    LOG_INFO("Lock-on camera smoothing follows the frame time (2 call sites)");
    return true;
}

void try_delayed_patches() {
    // Arxan rejects .text writes during the protected startup path. Wait until
    // the scheduler switch has stuck and the process has been up for a few seconds.
    if (g_scheduler_active.load(std::memory_order_acquire) == 0 || GetTickCount64() - g_install_ms < 5000) {
        return;
    }
    int expected = 0;
    if (g_havok_state.compare_exchange_strong(expected, 1)) {
        const bool ok = patch_havok();
        g_havok_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_menu_state.compare_exchange_strong(expected, 1)) {
        const bool ok = patch_menu();
        g_menu_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_speed_state.compare_exchange_strong(expected, 1)) {
        const bool ok = !g_fix_graze.load(std::memory_order_relaxed) || patch_speed();
        g_speed_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_decay_state.compare_exchange_strong(expected, 1)) {
        const bool ok = !g_fix_damping.load(std::memory_order_relaxed) || patch_decay();
        g_decay_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_slide_state.compare_exchange_strong(expected, 1)) {
        const bool ok = !g_fix_slide.load(std::memory_order_relaxed) || patch_slide();
        g_slide_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_step_down_state.compare_exchange_strong(expected, 1)) {
        const bool ok = !g_fix_step_down.load(std::memory_order_relaxed) ||
                        patch_ground_snap();
        g_step_down_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_move_dt_state.compare_exchange_strong(expected, 1)) {
        const bool ok = patch_move_dt();
        g_move_dt_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_gauge_state.compare_exchange_strong(expected, 1)) {
        const bool ok = g_fix_ui.load(std::memory_order_relaxed) == 0 || patch_gauge();
        g_gauge_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_swirl_state.compare_exchange_strong(expected, 1)) {
        const bool ok = true;  // the swirl counters are handled in hook_loading
        g_swirl_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_camera_state.compare_exchange_strong(expected, 1)) {
        const bool ok = patch_camera();
        g_camera_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_trace_state.compare_exchange_strong(expected, 1)) {
        const bool ok = !trace_active() || patch_trace();
        g_trace_state.store(ok ? 2 : -1);
    }
}

void note_presses_impl(void* detector, void* input_state, void* resolved, size_t stride, size_t action_offset,
                       size_t bits_offset, uint8_t kind, bool* held) {
    if (g_scheduler_active.load(std::memory_order_acquire) == 0 || !detector || !input_state || !resolved) {
        return;
    }
    auto* base = static_cast<uint8_t*>(resolved);
    auto* entry = *reinterpret_cast<uint8_t**>(base + 0x10);
    auto* end = *reinterpret_cast<uint8_t**>(base + 0x18);
    auto* bits = *reinterpret_cast<uint32_t**>(static_cast<uint8_t*>(input_state) + bits_offset);
    if (!entry || !end || end < entry || static_cast<size_t>(end - entry) > 0x1000 || !bits) {
        return;
    }

    spin_lock(g_pulse_lock);
    *held = true;
    PulseSource* source = nullptr;
    for (auto& candidate : g_sources) {
        if ((candidate.detector == detector && candidate.kind == kind) || !candidate.detector) {
            source = &candidate;
            if (!candidate.detector) {
                candidate.detector = detector;
                candidate.kind = kind;
            }
            break;
        }
    }
    if (!source) {
        *held = false;
        spin_unlock(g_pulse_lock);
        return;
    }
    for (; entry + stride <= end; entry += stride) {
        const int action = *reinterpret_cast<int*>(entry + action_offset);
        if (action < 0 || action >= kMaxActions) {
            continue;
        }
        const uint32_t mask = 1u << (action & 31);
        uint32_t* word = &source->bits[static_cast<unsigned>(action) >> 5];
        const bool active = (bits[static_cast<unsigned>(action) >> 5] & mask) != 0;
        const bool was = (*word & mask) != 0;
        if (active) {
            *word |= mask;
        } else {
            *word &= ~mask;
        }
        if (!active || was) {
            continue;
        }
        const uint64_t now = GetTickCount64();
        if (g_input_log.load(std::memory_order_relaxed) != 0 && ((action >= 0x50 && action <= 0x54) || action == 0x70)) {
            LOG_INFO("[input] pulse action=0x%02X source=%s", action,
                     kind == 0 ? "press" : (kind == 1 ? "press-alt" : (kind == 2 ? "repeat" : "repeat-alt")));
        }
        const uint64_t batch = static_cast<uint64_t>(g_pulse_batch_ms.load(std::memory_order_relaxed));
        int64_t generation = g_pulse_generation.load(std::memory_order_relaxed);
        if (generation == 0 || now - batch > 40) {
            generation = g_pulse_generation.fetch_add(1) + 1;
            g_pulse_batch_ms.store(static_cast<int64_t>(now), std::memory_order_relaxed);
        }
        g_action_generation[action].store(generation, std::memory_order_relaxed);
        g_action_ms[action].store(static_cast<int64_t>(now), std::memory_order_relaxed);
    }
    *held = false;
    spin_unlock(g_pulse_lock);
}

// The input structures belong to the game; if one of them is ever not what the layout above
// expects, skip the pulse instead of faulting inside the game's input thread.
void note_presses(void* detector, void* input_state, void* resolved, size_t stride, size_t action_offset,
                  size_t bits_offset, uint8_t kind) {
    bool held = false;
    __try {
        note_presses_impl(detector, input_state, resolved, stride, action_offset, bits_offset, kind, &held);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (held) {
            spin_unlock(g_pulse_lock);
        }
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 3) {
            LOG_ERROR("Input pulse read faulted and was skipped (layout mismatch?)");
        }
    }
}

void try_install_hold_state();

void hook_sim(void* step, float frame_time) {
    if (g_scheduler_active.load(std::memory_order_acquire) == 0) {
        g_sim(step, frame_time);
        return;
    }
    g_sim_count.fetch_add(1, std::memory_order_relaxed);
    // Measure first: the work below (first-time patching, logging) must not count as frame time.
    const float dt = advance_frame_dt();
    apply_frame_step(dt);
    write_gauge_factors(dt);
    if (g_fix_hold.load(std::memory_order_relaxed) != 0) {
        try_install_hold_state();
    }
    try_delayed_patches();
    update_speed_factors();
    const float corrected = scaled_frame(frame_time);
    if (g_sim_logged.exchange(1) == 0) {
        LOG_INFO("Simulation step: incoming=%.9f corrected=%.9f target=%u variable=%d", frame_time, corrected,
                 g_target_fps.load(), g_variable_dt.load());
    }
    g_sim(step, corrected);
}

// Title, common, and in-game menus are their own scheduler steps. Each still
// receives a fixed 1/60 on every displayed frame, so widget animation runs at
// TargetFPS/60. Scale that step the same way as simulation.
float menu_step_time(float frame_time) {
    if (g_scheduler_active.load(std::memory_order_acquire) == 0) {
        return frame_time;
    }
    const float corrected = scaled_frame(frame_time);
    if (g_menu_step_logged.exchange(1) == 0) {
        LOG_INFO("Menu animation step: incoming=%.9f corrected=%.9f", frame_time, corrected);
    }
    return corrected;
}

void hook_common_menu(void* step, float frame_time, void* context) {
    g_common_menu(step, menu_step_time(frame_time), context);
}

void hook_ingame_menu(void* step, float frame_time, void* context) {
    g_ingame_menu(step, menu_step_time(frame_time), context);
}

void hook_title_menu(void* step, float frame_time, void* context) {
    g_title_menu(step, menu_step_time(frame_time), context);
}

// The loading screen dialog (FrpgMenuDlgNowLoading, vtable slot 16 = update) is not run by the menu
// steps hooked above, and it is drawn while the simulation is stopped, so it gets a fixed 1/60 on
// every frame it is drawn and its swirl ran at the display rate. It is given the real time since
// its previous update instead.
constexpr uint32_t kLoadingVtable = 0x13CA258;
constexpr uint32_t kLoadingFn = 0x6FEC50;
using DialogUpdateFn = void (*)(void* dialog, float frame_time);
DialogUpdateFn g_loading = nullptr;
void** g_loading_slot = nullptr;
std::atomic<int> g_loading_logged{0};

void hook_loading(void* dialog, float frame_time) {
    static int64_t freq = 0;
    static int64_t last = 0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (freq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    float dt = frame_time;
    if (last != 0) {
        const double elapsed = static_cast<double>(now.QuadPart - last) / static_cast<double>(freq);
        if (elapsed > 0.0 && elapsed < 0.25) {
            dt = static_cast<float>(elapsed);
        }
    }
    last = now.QuadPart;
    if (g_input_log.load(std::memory_order_relaxed) != 0) {
        static int64_t mark = 0;
        static uint32_t calls = 0;
        static double sum = 0.0;
        static uint32_t swirl_mark = 0;
        if (mark == 0) {
            mark = now.QuadPart;
        }
        ++calls;
        sum += frame_time;
        const double wall = static_cast<double>(now.QuadPart - mark) / static_cast<double>(freq);
        if (wall >= 1.0) {
            const uint32_t swirl = g_swirl_total.load(std::memory_order_relaxed);
            LOG_INFO("[loading] %u updates, incoming dt summed %.3f s over %.3f s wall, swirl steps %u", calls, sum, wall,
                     swirl - swirl_mark);
            swirl_mark = swirl;
            calls = 0;
            sum = 0.0;
            mark = now.QuadPart;
        }
    }
    if (g_loading_logged.exchange(1) == 0) {
        LOG_INFO("Loading screen step: incoming=%.9f corrected=%.9f", frame_time, dt);
    }
    // The dialog adds one to two counters ([+0x208], [+0x20C], wrapping at 100) every time it is
    // updated; they turn the bonfire swirl. Set them so that its own increment lands on the value
    // they should have after the 1/30 s steps that really elapsed. (Done here, through the vtable,
    // so it works from the very first loading screen, before code patches are allowed.)
    {
        auto* counters = reinterpret_cast<int*>(static_cast<uint8_t*>(dialog) + 0x208);
        const uint32_t steps = swirl_steps();
        if (counters[0] >= 0 && counters[0] < 100 && counters[1] >= 0 && counters[1] < 100) {
            counters[0] = static_cast<int>((static_cast<uint32_t>(counters[0]) + steps) % 100) - 1;
            counters[1] = static_cast<int>((static_cast<uint32_t>(counters[1]) + steps) % 100) - 1;
        }
    }
    g_loading(dialog, dt);
}

bool install_loading_hook() {
    g_loading_slot = reinterpret_cast<void**>(image_rva(kLoadingVtable) + 16 * sizeof(void*));
    if (*g_loading_slot != image_rva(kLoadingFn)) {
        LOG_ERROR("Loading screen update slot does not match this build");
        g_loading_slot = nullptr;
        return false;
    }
    g_loading = reinterpret_cast<DialogUpdateFn>(*g_loading_slot);
    if (!patch_pointer(g_loading_slot, reinterpret_cast<void*>(hook_loading))) {
        g_loading = nullptr;
        g_loading_slot = nullptr;
        LOG_ERROR("Could not hook the loading screen update (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("Loading screen update follows real time");
    return true;
}

// Held-button counters. The HUD shortcut handler asks the input system for the state of the D-pad
// buttons (index 0xC = down, 0xD = up; slot 6 of the input object's vtable): the answer is the
// number of frames the button has been held, and 15 triggers the hold action (down: back to the
// first quick item). Counted in frames, 15 is a quarter of a second at 60 FPS and 62 ms at 240, so
// the hold fired during an ordinary tap. The answer is limited to the count a 60 FPS game would
// have reached in the real time the button has been down, and 15 is reported once.
constexpr uint32_t kInputObjectRva = 0x1CB5420;
constexpr uint32_t kHoldStateFn = 0xC95E50;
using HoldStateFn = int (*)(void* self, int index);
HoldStateFn g_hold_orig = nullptr;
std::atomic<int> g_hold_state{0};
int g_hold_attempts = 0;

struct HoldTrack {
    int64_t start = 0;
    int last = 0;
};
HoldTrack g_hold[2];

int hook_hold_state(void* self, int index) {
    const int result = g_hold_orig(self, index);
    if (index != 0xC && index != 0xD) {
        return result;
    }
    static int64_t freq = 0;
    HoldTrack& t = g_hold[index - 0xC];
    if (result <= 0) {
        t.start = 0;
        t.last = 0;
        return result;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (freq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    if (t.start == 0 || result == 1) {
        t.start = now.QuadPart;
        t.last = 0;
    }
    const double held = static_cast<double>(now.QuadPart - t.start) / static_cast<double>(freq);
    int n = 1 + static_cast<int>(held * 60.0);
    if (n > result) {
        n = result;
    }
    int answer = n;
    if (n == t.last) {
        answer = n | 0x100;  // already reported this count; a threshold test must not see it twice
    } else {
        t.last = n;
    }
    if (g_input_log.load(std::memory_order_relaxed) != 0 && (answer == 0xF || answer == 1)) {
        LOG_INFO("[input] hold index=0x%X answer=0x%X (game counted %d, %.3f s held)", index, answer, result, held);
    }
    return answer;
}

void try_install_hold_state() {
    if (g_hold_state.load(std::memory_order_relaxed) != 0 || ++g_hold_attempts > 3000) {
        return;
    }
    void* object = nullptr;
    __try {
        object = *reinterpret_cast<void**>(image_rva(kInputObjectRva));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!object) {
        return;
    }
    void** slot = nullptr;
    __try {
        slot = *reinterpret_cast<void***>(object) + 6;
        if (*slot != image_rva(kHoldStateFn)) {
            LOG_ERROR("Input state slot does not match this build");
            g_hold_state.store(-1);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    g_hold_orig = reinterpret_cast<HoldStateFn>(*slot);
    if (!patch_pointer(slot, reinterpret_cast<void*>(hook_hold_state))) {
        g_hold_orig = nullptr;
        LOG_ERROR("Could not hook the held-button counters (Win32=%lu)", GetLastError());
        g_hold_state.store(-1);
        return;
    }
    g_hold_state.store(1);
    LOG_INFO("D-pad hold counter follows real time (hold = 15 frames at 60 FPS)");
}

void hook_fx(void* manager, float frame_time) {
    // Code patches are applied only from the simulation hook (the thread that runs the patched
    // code), never from here, so no other thread can be executing a site while it is rewritten.
    if (g_scheduler_active.load(std::memory_order_acquire) == 0) {
        g_fx(manager, frame_time);
        return;
    }
    const float corrected = scaled_frame(frame_time);
    if (g_fx_logged.exchange(1) == 0) {
        LOG_INFO("FX step: incoming=%.9f corrected=%.9f", frame_time, corrected);
    }
    g_fx(manager, corrected);
}

void hook_remo(void* step, float frame_time, void* task) {
    if (g_scheduler_active.load(std::memory_order_acquire) == 0) {
        g_remo(step, frame_time, task);
        return;
    }
    const float corrected = scaled_frame(frame_time);
    if (g_remo_logged.exchange(1) == 0) {
        LOG_INFO("Remo step: incoming=%.9f corrected=%.9f", frame_time, corrected);
    }
    g_remo(step, corrected, task);
}

void hook_press(void* detector, void* input_state, void* entries, void* settings, float frame_time) {
    void* resolved = g_press_lookup(settings, 0);
    g_press(detector, input_state, entries, settings, frame_time);
    note_presses(detector, input_state, resolved, 8, 4, 0x198, 0);
}

void hook_press_alt(void* detector, void* input_state, void* entries, void* settings, float frame_time) {
    void* resolved = g_press_alt_lookup(settings, 0);
    g_press_alt(detector, input_state, entries, settings, frame_time);
    note_presses(detector, input_state, resolved, 8, 4, 0x50, 1);
}

void hook_repeat(void* detector, void* input_state, void* entries, void* settings, float frame_time) {
    void* resolved = g_repeat_lookup(settings, 0);
    g_repeat(detector, input_state, entries, settings, correct_repeat_time(frame_time));
    note_presses(detector, input_state, resolved, 12, 8, 0x198, 2);
}

void hook_repeat_alt(void* detector, void* input_state, void* entries, void* settings, float frame_time) {
    void* resolved = g_repeat_alt_lookup(settings, 0);
    g_repeat_alt(detector, input_state, entries, settings, correct_repeat_time(frame_time));
    note_presses(detector, input_state, resolved, 12, 8, 0x50, 3);
}

bool writable_private(const MEMORY_BASIC_INFORMATION& info) {
    return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && (info.Protect & 0xFF) == PAGE_READWRITE &&
           (info.Protect & PAGE_GUARD) == 0;
}

// Memory scans. The game keeps allocating and releasing memory while these walk it, so a page
// that VirtualQuery reported as committed can be gone by the time it is read. Every read here is
// inside a structured-exception guard: a vanished page ends that region's scan instead of taking
// the game down with an access violation (the most likely cause of start-up crashes).
struct ScanStats {
    unsigned regions = 0;
    unsigned faults = 0;
    uint64_t bytes = 0;
};

uint8_t* scan_region_for_scheduler(uintptr_t begin, uintptr_t end, uintptr_t vtable, bool* faulted) {
    uint8_t* found = nullptr;
    __try {
        auto cursor = (begin + 7) & ~uintptr_t{7};
        for (; cursor + 16 <= end; cursor += 8) {
            auto* candidate = reinterpret_cast<uintptr_t*>(cursor);
            if (candidate[0] == vtable && candidate[1] == 0) {
                found = reinterpret_cast<uint8_t*>(candidate);
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
    }
    return found;
}

LONG* scan_region_for_mode(uintptr_t begin, uintptr_t end, const uint8_t* scheduler, bool* faulted) {
    LONG* found = nullptr;
    __try {
        auto cursor = (begin + 15) & ~uintptr_t{7};
        for (; cursor + 8 <= end; cursor += 8) {
            if (*reinterpret_cast<uint8_t**>(cursor) == scheduler && *reinterpret_cast<LONG*>(cursor - 8) == 4) {
                found = reinterpret_cast<LONG*>(cursor - 8);
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
    }
    return found;
}

// Walks the writable private regions. `preferred_heap` restricts the walk to the game's
// 0x02001000-byte heap block (fast); otherwise every other writable private region up to
// `max_region` bytes is walked. The retail layout keeps the object in the first kind or in a
// small region; larger limits are only used after the quick passes have failed for a while.
constexpr uint64_t kQuickRegionLimit = 0x08000000;
constexpr uint64_t kAnyRegionLimit = ~uint64_t{0};
uint8_t* find_scheduler(uintptr_t vtable, bool preferred_heap, uint64_t max_region, ScanStats* stats) {
    uintptr_t address = 0x10000;
    MEMORY_BASIC_INFORMATION info{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) && info.RegionSize) {
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= address) {
            break;
        }
        const bool size_ok = preferred_heap ? info.RegionSize == 0x02001000
                                             : info.RegionSize != 0x02001000 && info.RegionSize <= max_region;
        if (writable_private(info) && size_ok) {
            bool faulted = false;
            stats->regions++;
            stats->bytes += info.RegionSize;
            uint8_t* hit = scan_region_for_scheduler(reinterpret_cast<uintptr_t>(info.BaseAddress), next, vtable, &faulted);
            if (faulted) {
                stats->faults++;
            }
            if (hit) {
                return hit;
            }
        }
        address = next;
    }
    return nullptr;
}

LONG* find_mode(uint8_t* scheduler, uint64_t max_region, ScanStats* stats) {
    uintptr_t address = 0x10000;
    MEMORY_BASIC_INFORMATION info{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) && info.RegionSize) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const uintptr_t next = base + info.RegionSize;
        if (next <= address) {
            break;
        }
        if (writable_private(info) && info.RegionSize <= max_region) {
            bool faulted = false;
            stats->regions++;
            stats->bytes += info.RegionSize;
            LONG* hit = scan_region_for_mode(base, next, scheduler, &faulted);
            if (faulted) {
                stats->faults++;
            }
            if (hit) {
                return hit;
            }
        }
        address = next;
    }
    return nullptr;
}

struct WindowSearch {
    DWORD pid;
    bool found;
};

BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search->pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

bool has_window() {
    WindowSearch search{GetCurrentProcessId(), false};
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool try_hook_remo() {
    if (g_remo) {
        return true;
    }
    void* value = *g_remo_slot;
    if (value != image_rva(kBuild.remo_fn)) {
        return false;
    }
    g_remo = reinterpret_cast<RemoFn>(value);
    if (!patch_pointer(g_remo_slot, reinterpret_cast<void*>(hook_remo))) {
        g_remo = nullptr;
        LOG_ERROR("Could not hook Remo (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("Remo callback hooked");
    return true;
}

void rollback() {
    if (g_qpc_iat && g_qpc) {
        patch_pointer(g_qpc_iat, reinterpret_cast<void*>(g_qpc));
    }
    if (g_sim_slot && g_sim) {
        patch_pointer(g_sim_slot, reinterpret_cast<void*>(g_sim));
    }
    if (g_fx_slot && g_fx) {
        patch_pointer(g_fx_slot, reinterpret_cast<void*>(g_fx));
    }
    if (g_press_slot && g_press) {
        patch_pointer(g_press_slot, reinterpret_cast<void*>(g_press));
    }
    if (g_press_alt_slot && g_press_alt) {
        patch_pointer(g_press_alt_slot, reinterpret_cast<void*>(g_press_alt));
    }
    if (g_repeat_slot && g_repeat) {
        patch_pointer(g_repeat_slot, reinterpret_cast<void*>(g_repeat));
    }
    if (g_repeat_alt_slot && g_repeat_alt) {
        patch_pointer(g_repeat_alt_slot, reinterpret_cast<void*>(g_repeat_alt));
    }
    if (g_common_menu_slot && g_common_menu) {
        patch_pointer(g_common_menu_slot, reinterpret_cast<void*>(g_common_menu));
    }
    if (g_ingame_menu_slot && g_ingame_menu) {
        patch_pointer(g_ingame_menu_slot, reinterpret_cast<void*>(g_ingame_menu));
    }
    if (g_title_menu_slot && g_title_menu) {
        patch_pointer(g_title_menu_slot, reinterpret_cast<void*>(g_title_menu));
    }
    if (g_remo_slot && g_remo) {
        patch_pointer(g_remo_slot, reinterpret_cast<void*>(g_remo));
    }
    if (g_loading_slot && g_loading) {
        patch_pointer(g_loading_slot, reinterpret_cast<void*>(g_loading));
    }
    g_scheduler_active.store(0, std::memory_order_release);
}

// Waits for the game's window. The hooks below are installed only once it exists, so nothing is
// modified while the game (and its copy protection) is still starting up.
bool wait_for_window(DWORD timeout_ms) {
    for (DWORD waited = 0; waited < timeout_ms; waited += 250) {
        if (has_window()) {
            LOG_INFO("Game window is up after %lu ms", waited);
            return true;
        }
        Sleep(250);
    }
    return false;
}

constexpr DWORD kSearchTimeoutMs = 150000;

uint8_t* g_scheduler_object = nullptr;
LONG* g_scheduler_mode = nullptr;

// One line of flipper state for the heartbeat: a rest or a map reload may replace the scheduler
// object, and this shows whether the one the mod switched is still the active one.
// Once a second: log when the flipper object's state changes (the game switching it back, or
// replacing the object after a rest or map load), with the time since the mod switched it.
void flipper_tick() {
    if (!g_scheduler_object) {
        return;
    }
    static int last = -1;
    static int lines = 0;
    uintptr_t vtable = 0;
    LONG mode = -1;
    bool ok = true;
    __try {
        vtable = *reinterpret_cast<uintptr_t*>(g_scheduler_object);
        mode = *g_scheduler_mode;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    const uintptr_t v60 = reinterpret_cast<uintptr_t>(image_rva(kBuild.flipper60_vtable));
    const uintptr_t v140 = reinterpret_cast<uintptr_t>(image_rva(kBuild.flipper140_vtable));
    const int state = !ok ? 3 : (vtable == v140 ? 1 : (vtable == v60 ? 0 : 2)) + 10 * static_cast<int>(mode);
    if (state != last && lines < 30) {
        ++lines;
        last = state;
        LOG_INFO("Flipper state: %s, mode %ld", !ok ? "unreadable" : (vtable == v140 ? "140 Hz" : (vtable == v60 ? "60 Hz" : "other")), mode);
    }
}

void describe_flipper(char* out, unsigned size) {
    uintptr_t vtable = 0;
    LONG mode = -1;
    bool ok = true;
    __try {
        vtable = *reinterpret_cast<uintptr_t*>(g_scheduler_object);
        mode = *g_scheduler_mode;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    const uintptr_t v60 = reinterpret_cast<uintptr_t>(image_rva(kBuild.flipper60_vtable));
    const uintptr_t v140 = reinterpret_cast<uintptr_t>(image_rva(kBuild.flipper140_vtable));
    std::snprintf(out, size, "flipper %s, mode %ld, menu queries suppressed %llu",
                  !ok ? "unreadable" : (vtable == v140 ? "140 Hz (switched)" : (vtable == v60 ? "60 Hz (the game resets it right after the switch; normal)" : "other")),
                  mode, static_cast<unsigned long long>(g_menu_suppressed.load(std::memory_order_relaxed)));
}

bool activate_scheduler() {
    const uintptr_t vtable60 = reinterpret_cast<uintptr_t>(image_rva(kBuild.flipper60_vtable));
    const uintptr_t vtable140 = reinterpret_cast<uintptr_t>(image_rva(kBuild.flipper140_vtable));
    bool remo_warned = false;
    bool logged_scheduler = false;
    bool logged_no_mode = false;
    ScanStats stats{};
    unsigned passes = 0;
    LOG_INFO("Looking for the 60 Hz flipper (for up to %lu s).", kSearchTimeoutMs / 1000);
    for (DWORD waited = 0; waited < kSearchTimeoutMs;) {
        try_hook_remo();
        ++passes;
        // Quick passes cover the usual places; after 20 s, every fourth pass also walks regions of
        // any size in case this system's allocator lays the heap out differently.
        const bool wide = waited >= 20000 && passes % 4 == 0;
        const uint64_t limit = wide ? kAnyRegionLimit : kQuickRegionLimit;
        uint8_t* scheduler = find_scheduler(vtable60, true, limit, &stats);
        if (!scheduler) {
            scheduler = find_scheduler(vtable60, false, limit, &stats);
        }
        if (scheduler) {
            if (!logged_scheduler) {
                logged_scheduler = true;
                LOG_INFO("60 Hz flipper object found at %p after %lu ms", scheduler, waited);
            }
            LONG* mode = find_mode(scheduler, waited >= 20000 ? kAnyRegionLimit : kQuickRegionLimit, &stats);
            if (mode) {
                // The 140 Hz flipper writes the high-refresh timing constants.
                // Mode 4 is the retail 60 Hz owner state; 5 keeps that flipper selected.
                for (int attempt = 0; attempt < 5; ++attempt) {
                    patch_pointer(reinterpret_cast<void**>(scheduler), reinterpret_cast<void*>(vtable140));
                    InterlockedExchange(mode, 5);
                    if (*reinterpret_cast<uintptr_t*>(scheduler) == vtable140 && *mode == 5) {
                        g_scheduler_object = scheduler;
                        g_scheduler_mode = mode;
                        g_scheduler_active.store(1, std::memory_order_release);
                        LOG_INFO("Flipper switched at %p. Owner mode at %p is 5. TargetFPS=%u", scheduler, mode,
                                 g_target_fps.load());
                        return true;
                    }
                    Sleep(100);
                }
                LOG_ERROR("Flipper switch did not stick (vtable %p, mode %ld); will look again",
                          *reinterpret_cast<void**>(scheduler), *mode);
            } else if (!logged_no_mode) {
                logged_no_mode = true;
                LOG_INFO("Found the flipper but not its owner mode yet (still starting up?); will keep looking");
            }
        }
        if (!g_remo && waited >= 30000 && !remo_warned) {
            remo_warned = true;
            LOG_INFO("Remo callback is %p (expected %p). Cinematics stay on the retail clock.",
                     *g_remo_slot, image_rva(kBuild.remo_fn));
        }
        // Quick polling while the game is starting, then a gentler one.
        const DWORD pause = waited < 20000 ? 250 : 1000;
        Sleep(pause);
        waited += pause;
    }
    // Explain the failure: is a 140 Hz flipper already selected (display frequency set high)?
    ScanStats probe{};
    uint8_t* already = find_scheduler(vtable140, true, kAnyRegionLimit, &probe);
    if (!already) {
        already = find_scheduler(vtable140, false, kAnyRegionLimit, &probe);
    }
    LOG_ERROR("The 60 Hz flipper was not found within %lu s (%u passes, %u regions, %u guarded faults). "
              "A 140 Hz flipper object %s. In System > PC Settings > Display, set Frequency to your monitor's "
              "refresh rate and Vertical sync to off, then restart the game.",
              kSearchTimeoutMs / 1000, passes, stats.regions, stats.faults,
              already ? "IS already present (the game selected it itself)" : "was not found either");
    return false;
}

}  // namespace

bool patches_apply() {
    g_image = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_image);
    if (!g_image || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LOG_ERROR("Could not read the game image");
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        LOG_ERROR("DarkSoulsRemastered.exe is not the expected 64-bit image");
        return false;
    }
    LOG_INFO("EXE timestamp=%08lX size=%08lX checksum=%08lX", nt->FileHeader.TimeDateStamp,
             nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum);
    if (nt->FileHeader.TimeDateStamp != kBuild.timestamp ||
        nt->OptionalHeader.SizeOfImage != kBuild.image_size ||
        nt->OptionalHeader.CheckSum != kBuild.checksum) {
        LOG_ERROR("This DarkSoulsRemastered.exe is not the supported 2022 Steam build");
        return false;
    }
    LOG_INFO("Recognized %s", kBuild.name);
    if (!validate_static_sites()) {
        return false;
    }

    g_sim_slot = reinterpret_cast<void**>(image_rva(kBuild.sim_vtable));
    g_fx_slot = reinterpret_cast<void**>(image_rva(kBuild.fx_vtable));
    g_remo_slot = reinterpret_cast<void**>(image_rva(kBuild.remo_slot));
    g_press_slot = reinterpret_cast<void**>(image_rva(kBuild.press_vtable));
    g_press_alt_slot = reinterpret_cast<void**>(image_rva(kBuild.press_alt_vtable));
    g_repeat_slot = reinterpret_cast<void**>(image_rva(kBuild.repeat_vtable));
    g_repeat_alt_slot = reinterpret_cast<void**>(image_rva(kBuild.repeat_alt_vtable));
    g_common_menu_slot = reinterpret_cast<void**>(image_rva(kBuild.common_menu_vtable));
    g_ingame_menu_slot = reinterpret_cast<void**>(image_rva(kBuild.ingame_menu_vtable));
    g_title_menu_slot = reinterpret_cast<void**>(image_rva(kBuild.title_menu_vtable));

    g_qpc = reinterpret_cast<QpcFn>(*g_qpc_iat);
    g_sim = reinterpret_cast<SimFn>(*g_sim_slot);
    g_fx = reinterpret_cast<FxFn>(*g_fx_slot);
    g_menu = reinterpret_cast<MenuFn>(image_rva(kBuild.menu_query));
    g_press = reinterpret_cast<InputFn>(*g_press_slot);
    g_press_alt = reinterpret_cast<InputFn>(*g_press_alt_slot);
    g_repeat = reinterpret_cast<InputFn>(*g_repeat_slot);
    g_repeat_alt = reinterpret_cast<InputFn>(*g_repeat_alt_slot);
    g_common_menu = reinterpret_cast<MenuStepFn>(*g_common_menu_slot);
    g_ingame_menu = reinterpret_cast<MenuStepFn>(*g_ingame_menu_slot);
    g_title_menu = reinterpret_cast<MenuStepFn>(*g_title_menu_slot);
    g_press_lookup = reinterpret_cast<LookupFn>(image_rva(kBuild.press_lookup));
    g_press_alt_lookup = reinterpret_cast<LookupFn>(image_rva(kBuild.press_alt_lookup));
    g_repeat_lookup = reinterpret_cast<LookupFn>(image_rva(kBuild.repeat_lookup));
    g_repeat_alt_lookup = reinterpret_cast<LookupFn>(image_rva(kBuild.repeat_alt_lookup));

    if (!wait_for_window(120000)) {
        LOG_ERROR("The game window did not appear within 120 seconds");
        return false;
    }
    Sleep(1500);  // let start-up settle before anything is modified
    g_install_ms = GetTickCount64();
    if (!patch_pointer(g_qpc_iat, reinterpret_cast<void*>(hook_qpc)) ||
        !patch_pointer(g_sim_slot, reinterpret_cast<void*>(hook_sim)) ||
        !patch_pointer(g_fx_slot, reinterpret_cast<void*>(hook_fx)) ||
        !patch_pointer(g_press_slot, reinterpret_cast<void*>(hook_press)) ||
        !patch_pointer(g_press_alt_slot, reinterpret_cast<void*>(hook_press_alt)) ||
        !patch_pointer(g_repeat_slot, reinterpret_cast<void*>(hook_repeat)) ||
        !patch_pointer(g_repeat_alt_slot, reinterpret_cast<void*>(hook_repeat_alt)) ||
        !patch_pointer(g_common_menu_slot, reinterpret_cast<void*>(hook_common_menu)) ||
        !patch_pointer(g_ingame_menu_slot, reinterpret_cast<void*>(hook_ingame_menu)) ||
        !patch_pointer(g_title_menu_slot, reinterpret_cast<void*>(hook_title_menu))) {
        LOG_ERROR("Could not install a vtable hook (Win32=%lu)", GetLastError());
        rollback();
        return false;
    }
    if (!activate_scheduler()) {
        rollback();
        return false;
    }
    if (g_fix_ui.load(std::memory_order_relaxed) != 0) {
        install_loading_hook();
    }
    diag_watchdog_start(
        [] { return g_sim_count.load(std::memory_order_relaxed); },
        [] { return g_frame_dt.load(std::memory_order_relaxed) * 1000.0f; },
        describe_flipper, flipper_tick);
    LOG_INFO("DS1 Remastered FPS Unlock v1.1.0 active. Frame cap %u FPS, %s step.", g_target_fps.load(),
             g_variable_dt.load() ? "measured frame time" : "fixed 1/TargetFPS");
    return true;
}
