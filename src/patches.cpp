#include "patches.h"

#include "log.h"
#include "profile.h"
#include "state.h"
#include "trace.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>

#include <cmath>
#include <cstdint>
#include <cstring>

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

float scaled_frame(float frame_time) {
    const float target = static_cast<float>(g_target_fps.load(std::memory_order_relaxed));
    return frame_time * 60.0f / target;
}

// The retail scheduler hands every displayed frame a 1/60 step, including the
// dormant 140 Hz flipper. Callers that already pass 1/TargetFPS are left alone.
float correct_repeat_time(float frame_time) {
    if (g_scheduler_active.load(std::memory_order_acquire) == 0) {
        return frame_time;
    }
    const float target = static_cast<float>(g_target_fps.load(std::memory_order_relaxed));
    const float native_dt = std::fabs(frame_time - (1.0f / 60.0f));
    const float target_dt = std::fabs(frame_time - (1.0f / target));
    const float corrected = native_dt < target_dt ? frame_time * 60.0f / target : frame_time;
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

    const float scale = 60.0f / static_cast<float>(g_target_fps.load());
    uint8_t code[17] = {
        0xF3, 0x0F, 0x59, 0x0D, 0x05, 0x00, 0x00, 0x00,  // mulss xmm1, [rip+5]
        0xE9, 0x00, 0x00, 0x00, 0x00,                    // jmp stub
    };
    const intptr_t jump = target - (static_cast<uint8_t*>(g_havok_relay) + 13);
    if (jump < INT32_MIN || jump > INT32_MAX) {
        VirtualFree(g_havok_relay, 0, MEM_RELEASE);
        g_havok_relay = nullptr;
        LOG_ERROR("Havok stub is out of range of the relay");
        return false;
    }
    const auto rel = static_cast<int32_t>(jump);
    std::memcpy(code + 9, &rel, sizeof(rel));
    std::memcpy(code + 13, &scale, sizeof(scale));
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
    LOG_INFO("Havok step scaled by %.6f", scale);
    return true;
}

uint8_t hook_menu(void* menu, int action) {
    const uint8_t active = g_menu(menu, action);
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
    return suppress ? 0 : active;
}

bool patch_menu() {
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
    const double target = static_cast<double>(g_target_fps.load(std::memory_order_relaxed));
    auto* factor = static_cast<float*>(page);
    *factor = static_cast<float>(std::exp(std::log(0.975) * 60.0 / target));

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
    LOG_INFO("Velocity damp retargeted (per-frame factor %.6f instead of 0.975)", *factor);
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
        const bool ok = patch_speed();
        g_speed_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_decay_state.compare_exchange_strong(expected, 1)) {
        const bool ok = patch_decay();
        g_decay_state.store(ok ? 2 : -1);
    }
    expected = 0;
    if (g_trace_state.compare_exchange_strong(expected, 1)) {
        const bool ok = !trace_active() || patch_trace();
        g_trace_state.store(ok ? 2 : -1);
    }
}

void note_presses(void* detector, void* input_state, void* resolved, size_t stride, size_t action_offset,
                  size_t bits_offset, uint8_t kind) {
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
        const uint64_t batch = static_cast<uint64_t>(g_pulse_batch_ms.load(std::memory_order_relaxed));
        int64_t generation = g_pulse_generation.load(std::memory_order_relaxed);
        if (generation == 0 || now - batch > 40) {
            generation = g_pulse_generation.fetch_add(1) + 1;
            g_pulse_batch_ms.store(static_cast<int64_t>(now), std::memory_order_relaxed);
        }
        g_action_generation[action].store(generation, std::memory_order_relaxed);
        g_action_ms[action].store(static_cast<int64_t>(now), std::memory_order_relaxed);
    }
    spin_unlock(g_pulse_lock);
}

void hook_sim(void* step, float frame_time) {
    if (g_scheduler_active.load(std::memory_order_acquire) == 0) {
        g_sim(step, frame_time);
        return;
    }
    try_delayed_patches();
    update_speed_factors();
    const float corrected = scaled_frame(frame_time);
    if (g_sim_logged.exchange(1) == 0) {
        LOG_INFO("Simulation step: incoming=%.9f corrected=%.9f target=%u", frame_time, corrected,
                 g_target_fps.load());
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

void hook_fx(void* manager, float frame_time) {
    try_delayed_patches();
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

uint8_t* find_scheduler(uintptr_t vtable, bool preferred_heap) {
    uintptr_t address = 0x10000;
    MEMORY_BASIC_INFORMATION info{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) && info.RegionSize) {
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= address) {
            break;
        }
        const bool size_ok = preferred_heap ? info.RegionSize == 0x02001000
                                             : info.RegionSize != 0x02001000 && info.RegionSize <= 0x08000000;
        if (writable_private(info) && size_ok) {
            auto cursor = (reinterpret_cast<uintptr_t>(info.BaseAddress) + 7) & ~uintptr_t{7};
            const uintptr_t end = next;
            for (; cursor + 16 <= end; cursor += 8) {
                auto* candidate = reinterpret_cast<uintptr_t*>(cursor);
                if (candidate[0] == vtable && candidate[1] == 0) {
                    return reinterpret_cast<uint8_t*>(candidate);
                }
            }
        }
        address = next;
    }
    return nullptr;
}

LONG* find_mode(uint8_t* scheduler) {
    uintptr_t address = 0x10000;
    MEMORY_BASIC_INFORMATION info{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) && info.RegionSize) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const uintptr_t next = base + info.RegionSize;
        if (next <= address) {
            break;
        }
        if (writable_private(info) && info.RegionSize <= 0x08000000) {
            auto cursor = (base + 15) & ~uintptr_t{7};
            for (; cursor + 8 <= next; cursor += 8) {
                if (*reinterpret_cast<uint8_t**>(cursor) == scheduler &&
                    *reinterpret_cast<LONG*>(cursor - 8) == 4) {
                    return reinterpret_cast<LONG*>(cursor - 8);
                }
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
    g_scheduler_active.store(0, std::memory_order_release);
}

bool activate_scheduler() {
    const uintptr_t vtable60 = reinterpret_cast<uintptr_t>(image_rva(kBuild.flipper60_vtable));
    void* vtable140 = image_rva(kBuild.flipper140_vtable);
    bool saw_window = false;
    bool remo_warned = false;
    for (DWORD waited = 0; waited < 45000; waited += 250) {
        try_hook_remo();
        if (!saw_window && has_window()) {
            saw_window = true;
            LOG_INFO("Game window is up. Looking for the 60 Hz flipper.");
        }
        if (saw_window) {
            uint8_t* scheduler = find_scheduler(vtable60, true);
            if (!scheduler) {
                scheduler = find_scheduler(vtable60, false);
            }
            if (scheduler) {
                LONG* mode = find_mode(scheduler);
                if (mode) {
                    // The 140 Hz flipper writes the high-refresh timing constants.
                    // Mode 4 is the retail 60 Hz owner state; 5 keeps that flipper selected.
                    patch_pointer(reinterpret_cast<void**>(scheduler), vtable140);
                    InterlockedExchange(mode, 5);
                    if (*reinterpret_cast<void**>(scheduler) != vtable140 || *mode != 5) {
                        LOG_ERROR("Flipper switch did not stick");
                        return false;
                    }
                    g_scheduler_active.store(1, std::memory_order_release);
                    LOG_INFO("Flipper switched at %p. Owner mode at %p is 5. TargetFPS=%u", scheduler, mode,
                             g_target_fps.load());
                    return true;
                }
            }
        }
        if (!g_remo && waited >= 30000 && !remo_warned) {
            remo_warned = true;
            LOG_INFO("Remo callback is %p (expected %p). Cinematics stay on the retail clock.",
                     *g_remo_slot, image_rva(kBuild.remo_fn));
        }
        Sleep(250);
    }
    LOG_ERROR(saw_window ? "The 60 Hz flipper was not found within 45 seconds"
                         : "The game window did not appear within 45 seconds");
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
    LOG_INFO("DS1 Remastered FPS Unlock v0.0.1 active. Step scale is 60/%u.", g_target_fps.load());
    return true;
}
