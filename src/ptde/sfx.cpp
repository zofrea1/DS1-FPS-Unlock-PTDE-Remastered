#include "sfx.h"

#include "log.h"
#include "sfxwatch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kRegisterEffect = 0x00D19CF0;
constexpr float kMinInterval = 1.0f / 30.0f;
// What an interval below 1/30 s becomes: just under 1/30 s. The emitter spawns once the time since its last spawn
// reaches the interval, so exactly 1/30 s sits on the 30 FPS frame: a frame a hair shorter (the frame time varies by
// a fraction of a millisecond) spawned nothing. Measured at the Demon Ruins bonfire (2026-11-16, particles emitted a
// second): the game as shipped 878 at 30 FPS; with 1/30 s, 646 at 30 FPS and 654 at 60; with 0.98/30 s, 876 at 30,
// 884 at 60 and 852 at 214. 0.95/30 s leaves 1.7 ms for frames that come early.
constexpr float kRaisedInterval = 0.95f / 30.0f;

using RegisterFn = void*(__stdcall*)(const void* name, uint8_t* data, uint32_t size);
RegisterFn g_orig = nullptr;
std::atomic<int> g_effects{0};
std::atomic<int> g_raised{0};
bool g_raise = true;  // FixSfxSpawnRate; the hook also serves SfxWatch alone

uint32_t rd32(const uint8_t* d, uint32_t o) {
    uint32_t v;
    std::memcpy(&v, d + o, 4);
    return v;
}

// PTDE effect file: 'FXR\0', version, data start, pointer table offset (+0x0C), pointer count (+0x10).
// The table lists the file offset of every pointer field. A pond3 pointer is the last field of an AST
// (24 bytes: pond1 ptr, data3 count, the count again, three 0/1 flag bytes and a 0, pond2 ptr, pond3 ptr),
// so a pointer field whose preceding 20 bytes have that shape leads to a pond3 struct, whose first int is
// its type (0-7). Checked against a full parse of all 3,849 PTDE effects: it finds exactly their type 2
// and type 3 structs.
int raise_intervals(uint8_t* d, uint32_t size) {
    if (!d || size < 0x20 || std::memcmp(d, "FXR\0", 4) != 0) return 0;
    const uint32_t table = rd32(d, 0x0C);
    const uint32_t count = rd32(d, 0x10);
    if (table > size || count > (size - table) / 4) return 0;
    std::vector<uint32_t> fields(count);
    for (uint32_t i = 0; i < count; ++i) fields[i] = rd32(d, table + i * 4);
    std::sort(fields.begin(), fields.end());
    auto listed = [&](uint32_t o) { return std::binary_search(fields.begin(), fields.end(), o); };

    int raised = 0;
    for (uint32_t o : fields) {
        if (o < 0x14 || o > size - 4) continue;
        const uint32_t ast = o - 0x14;
        const uint32_t pond1 = rd32(d, ast), n1 = rd32(d, ast + 4), n2 = rd32(d, ast + 8);
        const uint8_t* flags = d + ast + 12;
        const uint32_t pond2 = rd32(d, ast + 16);
        if (n1 != n2 || n1 >= 4096 || flags[3] != 0 || flags[0] > 1 || flags[1] > 1 || flags[2] > 1) continue;
        if ((pond1 && !listed(ast)) || (pond2 && !listed(ast + 16))) continue;
        const uint32_t target = rd32(d, o);
        if (target > size - 0x18) continue;
        const uint32_t type = rd32(d, target);
        uint32_t at = 0;
        if (type == 2) {
            at = target + 0x10;
        } else if (type == 3) {
            at = target + 0x0C;
        } else {
            continue;
        }
        float v;
        std::memcpy(&v, d + at, 4);
        if (v >= 0.0f && v < kMinInterval - 1e-6f) {
            std::memcpy(d + at, &kRaisedInterval, 4);
            ++raised;
        }
    }
    return raised;
}

void* __stdcall hk_register(const void* name, uint8_t* data, uint32_t size) {
    int raised = 0;
    bool watching = false;
    __try {
        watching = sfxwatch_before_load(data, size);
        raised = g_raise ? raise_intervals(data, size) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        raised = 0;
    }
    const int effects = ++g_effects;
    const int total = (g_raised += raised);
    // The first effect shows the hook was in place before the common effects loaded; then a running total.
    if (effects == 1 || effects % 500 == 0) {
        LOG_INFO("Effect spawn intervals: %d raised to just under 1/30 s in the %d effects loaded so far", total,
                 effects);
    }
    void* resource = g_orig(name, data, size);
    if (watching) sfxwatch_after_load(resource);
    return resource;
}

// Both action dispatchers end in `call [ebp-8] ; add esp,10h` (cdecl routine(handler, node, args, x)): 0xB80F70
// (an action id from a state's list) and 0xB81010 (an action another action runs, as 0x62 runs 4).
constexpr uint32_t kActionCalls[2] = {0x00B80FB5, 0x00B81053};
constexpr uint8_t kActionCallBytes[6] = {0xFF, 0x55, 0xF8, 0x83, 0xC4, 0x10};

struct Held {
    uint8_t* node;
    float was;
    float set;
};
// The clocks the action calls on a thread have moved, innermost last (an action can run another). Kept in a
// TlsAlloc slot rather than in thread_local variables, so the DLL has no static TLS section.
struct HeldStack {
    Held held[16];
    int depth;
};
DWORD g_held_slot = TLS_OUT_OF_INDEXES;

// This thread's stack, made on its first action call; nullptr only if that allocation failed.
HeldStack* held_stack() {
    const DWORD error = GetLastError();  // TlsGetValue clears the thread's last error; the game's code keeps its own
    auto* stack = static_cast<HeldStack*>(TlsGetValue(g_held_slot));
    if (!stack) {
        stack = static_cast<HeldStack*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HeldStack)));
        if (stack) TlsSetValue(g_held_slot, stack);
    }
    SetLastError(error);
    return stack;
}

void __stdcall action_begin(uint8_t* node) {
    HeldStack* stack = held_stack();
    if (!stack) return;  // nothing is moved, and action_end then finds an empty stack
    Held h{nullptr, 0.0f, 0.0f};
    if (node) {
        __try {
            float* clock = reinterpret_cast<float*>(node + 0x7C);
            const float t = *clock;
            // The next 30 FPS frame at or after t, with a tenth of a frame of slack for a 30 FPS frame that ran
            // a little long.
            const double frames = std::ceil(static_cast<double>(t) * 30.0 - 0.1);
            const float q = frames > 0.0 ? static_cast<float>(frames / 30.0) : 0.0f;
            if (t >= 0.0f && q > t) {
                *clock = q;
                h = Held{node, t, q};
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            h = Held{nullptr, 0.0f, 0.0f};
        }
    }
    if (stack->depth < 16) stack->held[stack->depth] = h;
    ++stack->depth;
}

void __stdcall action_end() {
    HeldStack* stack = held_stack();
    if (!stack || stack->depth <= 0) return;
    --stack->depth;
    if (stack->depth >= 16) return;
    const Held h = stack->held[stack->depth];
    if (!h.node) return;
    __try {
        float* clock = reinterpret_cast<float*>(h.node + 0x7C);
        if (*clock == h.set) *clock = h.was;  // an action that set the clock itself keeps its value
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Trace statistics: calls per action routine (the routine's address), logged once a second by sfx_actions_tick.
bool g_action_stats = false;
constexpr int kActionSlots = 256;
volatile LONG g_action_key[kActionSlots];
volatile LONG g_action_count[kActionSlots];

void __stdcall action_note(uint32_t routine) {
    if (!g_action_stats || !routine) return;
    uint32_t i = (routine >> 4) & (kActionSlots - 1);
    for (int n = 0; n < kActionSlots; ++n, i = (i + 1) & (kActionSlots - 1)) {
        const LONG k = g_action_key[i];
        if (k == static_cast<LONG>(routine) ||
            (k == 0 && InterlockedCompareExchange(&g_action_key[i], static_cast<LONG>(routine), 0) == 0) ||
            g_action_key[i] == static_cast<LONG>(routine)) {
            InterlockedIncrement(&g_action_count[i]);
            return;
        }
    }
}

// Entered by `call` in place of `call [ebp-8] ; add esp,10h`: [esp] the return, then handler, node, args, x.
__declspec(naked) void action_call_stub() {
    __asm {
        push dword ptr [esp + 8]
        call action_begin
        push dword ptr [ebp - 8]     // the routine
        call action_note
        push dword ptr [esp + 10h]
        push dword ptr [esp + 10h]
        push dword ptr [esp + 10h]
        push dword ptr [esp + 10h]
        call dword ptr [ebp - 8]
        add esp, 10h
        push eax  // the action's result, as the game's code after the call sees it
        call action_end
        pop eax
        ret 10h
    }
}

}  // namespace

bool sfx_actions_install() {
    for (uint32_t site : kActionCalls) {
        if (std::memcmp(reinterpret_cast<const void*>(site), kActionCallBytes, sizeof(kActionCallBytes)) != 0) {
            LOG_ERROR("Effect action call at %08X does not match this build. Not patching.", site);
            return false;
        }
    }
    if (g_held_slot == TLS_OUT_OF_INDEXES) g_held_slot = TlsAlloc();
    if (g_held_slot == TLS_OUT_OF_INDEXES) {
        LOG_ERROR("Effect actions: no thread-local slot is free. Not patching.");
        return false;
    }
    for (uint32_t site : kActionCalls) {
        auto* p = reinterpret_cast<uint8_t*>(site);
        DWORD old = 0;
        if (!VirtualProtect(p, sizeof(kActionCallBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
        p[0] = 0xE8;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&action_call_stub) - (site + 5));
        std::memcpy(p + 1, &rel, 4);
        p[5] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), p, sizeof(kActionCallBytes));
        DWORD ignored = 0;
        VirtualProtect(p, sizeof(kActionCallBytes), old, &ignored);
    }
    LOG_INFO("Effect actions read their node's clock on the 30 FPS frame grid (sites %08X, %08X)", kActionCalls[0],
             kActionCalls[1]);
    return true;
}

void sfx_actions_stats(bool on) { g_action_stats = on; }

void sfx_actions_tick() {
    static LARGE_INTEGER freq{}, since{};
    static LONG frames = 0;
    static int lines = 0;
    if (!g_action_stats) return;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&since);
    }
    ++frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double secs = static_cast<double>(now.QuadPart - since.QuadPart) / static_cast<double>(freq.QuadPart);
    if (secs < 1.0) return;
    char buf[1024];
    int len = 0;
    LONG total = 0;
    for (int i = 0; i < kActionSlots; ++i) {
        const LONG c = InterlockedExchange(&g_action_count[i], 0);
        if (!c) continue;
        total += c;
        if (len < 900) len += std::snprintf(buf + len, sizeof(buf) - len, " %lX:%ld", g_action_key[i], c);
    }
    const LONG f = frames;
    frames = 0;
    since = now;
    if (!total || lines >= 1800) return;
    ++lines;
    buf[len] = 0;
    LOG_INFO("SfxActions: %.2f s, %ld frames (%.0f FPS), %ld action calls:%s", secs, f, f / secs, total, buf);
}

namespace {

// 0xB94C19: fld [ebp-0Ch] (clock now); fsub [ebp-10h] (clock at the previous update); fstp [ebp-14h] (the step).
constexpr uint32_t kDistortionStep = 0x00B94C19;
constexpr uint8_t kDistortionStepBytes[] = {0xD9, 0x45, 0xF4, 0xD8, 0x65, 0xF0, 0xD9, 0x5D, 0xEC};
// What follows it, as a check that this is the function the patch was written for.
constexpr uint8_t kDistortionNextBytes[] = {0x8B, 0x43, 0x0C, 0x8B, 0x48, 0x70};

float grid30(float t) { return std::floor(t * 30.0f + 0.5f) / 30.0f; }

float __cdecl distortion_grid_step(float now, float prev) { return grid30(now) - grid30(prev); }

// Called in place of the three instructions, inside the game function's frame (ebp is the game's).
__declspec(naked) void distortion_step_stub() {
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [ebp - 10h]
        push dword ptr [ebp - 0Ch]
        call distortion_grid_step
        add esp, 8
        fstp dword ptr [ebp - 14h]
        pop edx
        pop ecx
        pop eax
        ret
    }
}

}  // namespace

bool sfx_distortion_install() {
    auto* p = reinterpret_cast<uint8_t*>(kDistortionStep);
    if (std::memcmp(p, kDistortionStepBytes, sizeof(kDistortionStepBytes)) != 0 ||
        std::memcmp(p + sizeof(kDistortionStepBytes), kDistortionNextBytes, sizeof(kDistortionNextBytes)) != 0) {
        LOG_ERROR("Heat haze step at %08X does not match this build. Not patching.", kDistortionStep);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(kDistortionStepBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&distortion_step_stub) - (kDistortionStep + 5));
    std::memcpy(p + 1, &rel, 4);
    std::memset(p + 5, 0x90, sizeof(kDistortionStepBytes) - 5);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(kDistortionStepBytes));
    DWORD ignored = 0;
    VirtualProtect(p, sizeof(kDistortionStepBytes), old, &ignored);
    LOG_INFO("Heat haze scrolls on the 30 FPS frame grid (%08X)", kDistortionStep);
    return true;
}

// ---------------------------------------------------------------------------------------------- swaying particles
// FXClusterMovement_Yurayura (effect node type 84) and FXClusterMovement_PartialFollow (type 105) turn every particle's
// velocity by a random angle (0xA50100) once their counter (+0x10) passes the node's limit (10 or 20 thirtieths of a
// second: data +0x40), and the counter goes up by one each time the cluster's per-call function runs (0x9E6050,
// 0x9E50C0), once a frame. At 240 FPS the particles turned eight times as often: the SfxSway diagnostic (2026-11-15,
// Blighttown) counted about 60 turns a second at 30 FPS and 500 at 220 FPS for the same 43 clusters, and the
// spider offspring's magma tentacle (effect 13401) ended in a jitter that stretched far behind it instead of
// staying on its body. The counter now counts the 30 FPS frames of the game's clock: before the per-call function
// adds its one, the counter moves by the number of 1/30 s steps the clock crossed this frame, less one. At 30 FPS
// that is exactly one a frame, as before.
namespace {

constexpr uint32_t kSwayCalls[2] = {0x009E6050, 0x009E50C0};
constexpr uint8_t kSwayCallBytes[2][6] = {
    {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x00},  // push ebp ; mov ebp,esp ; push -1
    {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08},  // push ebx ; mov ebx,esp ; sub esp,8
};
constexpr int kSwayCallLen[2] = {5, 6};
void* g_sway_tramp[2] = {};
volatile LONG g_sway_ticks = 1;  // 1/30 s steps the game's clock crossed this frame
double g_sway_clock = 0.0;

void __stdcall sway_step(uint8_t* self) {
    const LONG ticks = g_sway_ticks;
    if (ticks == 1 || !self) return;
    __try {
        *reinterpret_cast<int32_t*>(self + 0x10) += ticks - 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// thiscall (ecx = the movement): the counter moved, then on into the moved instructions.
__declspec(naked) void sway_stub_0() {
    __asm {
        pushad
        push ecx
        call sway_step
        popad
        jmp dword ptr [g_sway_tramp]
    }
}

__declspec(naked) void sway_stub_1() {
    __asm {
        pushad
        push ecx
        call sway_step
        popad
        jmp dword ptr [g_sway_tramp + 4]
    }
}

}  // namespace

bool sfx_sway_install() {
    for (int i = 0; i < 2; ++i) {
        if (std::memcmp(reinterpret_cast<const void*>(kSwayCalls[i]), kSwayCallBytes[i], kSwayCallLen[i]) != 0) {
            LOG_ERROR("Effect sway counter at %08X does not match this build. Not patching.", kSwayCalls[i]);
            return false;
        }
    }
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) return false;
    void (*const stubs[2])() = {&sway_stub_0, &sway_stub_1};
    for (int i = 0; i < 2; ++i) {
        uint8_t* t = mem + i * 16;
        const int n = kSwayCallLen[i];
        std::memcpy(t, kSwayCallBytes[i], n);
        t[n] = 0xE9;
        const int32_t back = static_cast<int32_t>((kSwayCalls[i] + n) - reinterpret_cast<uintptr_t>(t + n + 5));
        std::memcpy(t + n + 1, &back, 4);
        g_sway_tramp[i] = t;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, 32);
    for (int i = 0; i < 2; ++i) {
        auto* p = reinterpret_cast<uint8_t*>(kSwayCalls[i]);
        DWORD old = 0;
        if (!VirtualProtect(p, kSwayCallLen[i], PAGE_EXECUTE_READWRITE, &old)) return false;
        p[0] = 0xE9;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stubs[i]) - (kSwayCalls[i] + 5));
        std::memcpy(p + 1, &rel, 4);
        for (int k = 5; k < kSwayCallLen[i]; ++k) p[k] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), p, kSwayCallLen[i]);
        DWORD ignored = 0;
        VirtualProtect(p, kSwayCallLen[i], old, &ignored);
    }
    LOG_INFO("Swaying effect particles turn on the 30 FPS clock (sites %08X, %08X)", kSwayCalls[0], kSwayCalls[1]);
    return true;
}

// The jittering follow: FXMovementFollow's variant with vtable 0x115822C (made by 0xB7AD00 when the game's effect
// entity asks for a jitter curve, entity +0x78, and an interval, +0x7C) attaches an effect to its owner and, in its
// update (0xB7AE50, slot 0), counts the update in +0x58 and once the count reaches the interval (+0x54) turns the
// effect by a random angle within the curve's value (+0x50) and starts the count again: once a frame, so at 240 FPS
// eight times as often. The same 30 FPS ticks: before the update adds its one, the count moves by ticks - 1.
namespace {

constexpr uint32_t kJitterUpdate = 0x00B7AE50;
constexpr uint8_t kJitterUpdateBytes[6] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};  // push ebx ; mov ebx,esp ; sub esp,8
void* g_jitter_tramp = nullptr;
bool g_jitter_stats = false;
volatile LONG g_jitter_updates = 0;
volatile LONG g_jitter_turns = 0;
volatile LONG g_jitter_low = 0x7FFFFFFF;  // the shortest and longest intervals seen (with a curve)
volatile LONG g_jitter_high = 0;
LARGE_INTEGER g_jitter_freq{};
LARGE_INTEGER g_jitter_since{};
int g_jitter_lines = 0;

void __stdcall jitter_step(uint8_t* self) {
    if (!self) return;
    const LONG ticks = g_sway_ticks;
    __try {
        if (ticks != 1) *reinterpret_cast<int32_t*>(self + 0x58) += ticks - 1;
        if (g_jitter_stats) {
            InterlockedIncrement(&g_jitter_updates);
            const LONG limit = *reinterpret_cast<const LONG*>(self + 0x54);
            if (*reinterpret_cast<const uint32_t*>(self + 0x50) != 0) {
                if (static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(self + 0x58) + 1) >=
                    static_cast<uint32_t>(limit))
                    InterlockedIncrement(&g_jitter_turns);
                if (limit < g_jitter_low) InterlockedExchange(&g_jitter_low, limit);
                if (limit > g_jitter_high) InterlockedExchange(&g_jitter_high, limit);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(naked) void jitter_stub() {
    __asm {
        pushad
        push ecx
        call jitter_step
        popad
        jmp dword ptr [g_jitter_tramp]
    }
}

}  // namespace

bool sfx_jitter_install(bool stats) {
    auto* p = reinterpret_cast<uint8_t*>(kJitterUpdate);
    if (std::memcmp(p, kJitterUpdateBytes, sizeof(kJitterUpdateBytes)) != 0) {
        LOG_ERROR("Effect follow jitter at %08X does not match this build. Not patching.", kJitterUpdate);
        return false;
    }
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    std::memcpy(t, kJitterUpdateBytes, sizeof(kJitterUpdateBytes));
    t[6] = 0xE9;
    const int32_t back = static_cast<int32_t>((kJitterUpdate + 6) - reinterpret_cast<uintptr_t>(t + 11));
    std::memcpy(t + 7, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    g_jitter_tramp = t;
    g_jitter_stats = stats;
    QueryPerformanceFrequency(&g_jitter_freq);
    QueryPerformanceCounter(&g_jitter_since);
    DWORD old = 0;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&jitter_stub) - (kJitterUpdate + 5));
    std::memcpy(p + 1, &rel, 4);
    p[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    DWORD ignored = 0;
    VirtualProtect(p, 6, old, &ignored);
    LOG_INFO("Jittering effect follows turn on the 30 FPS clock (%08X)", kJitterUpdate);
    return true;
}

void sfx_jitter_tick() {
    if (!g_jitter_stats) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double secs =
        static_cast<double>(now.QuadPart - g_jitter_since.QuadPart) / static_cast<double>(g_jitter_freq.QuadPart);
    if (secs < 1.0) return;
    g_jitter_since = now;
    const LONG updates = InterlockedExchange(&g_jitter_updates, 0), turns = InterlockedExchange(&g_jitter_turns, 0);
    const LONG low = InterlockedExchange(&g_jitter_low, 0x7FFFFFFF), high = InterlockedExchange(&g_jitter_high, 0);
    if (updates == 0 || g_jitter_lines >= 600) return;
    ++g_jitter_lines;
    LOG_INFO("SfxJitter: %.2f s: %ld jittering-follow updates, %ld turns, intervals %ld..%ld", secs, updates, turns,
             high ? low : 0, high);
}

void sfx_sway_frame(double step) {
    if (!(step > 0.0) || step > 1.0) step = 1.0 / 30.0;
    const double before = g_sway_clock;
    g_sway_clock += step;
    if (g_sway_clock > 86400.0) g_sway_clock -= 86400.0;  // a day: the 1/30 s grid is whole there (2592000 steps)
    const double was = before > g_sway_clock ? before - 86400.0 : before;
    LONG ticks = static_cast<LONG>(std::floor(g_sway_clock * 30.0 + 1e-4) - std::floor(was * 30.0 + 1e-4));
    if (ticks < 0) ticks = 0;
    if (ticks > 15) ticks = 15;
    InterlockedExchange(&g_sway_ticks, ticks);
}

// --------------------------------------------------------------------------------------------------- effect trails
// FXParticleAppearance_Tracer (effect node type 40) draws a ribbon through a particle's recent positions. Each update
// (0xB90CD0) adds the current position, plus the node's interpolated points in between (data +0x60, so n points in
// all), and commits them to the trail's history once more than the node's step (data +0x54, seconds) has passed since
// the last commit; points older than the trail's lifetime (data +0x58) drop off the end. The history holds a fixed
// number of points, sized when the trail starts (0xB93590): min(data +0x5C * n, the lifetime at 60 a second * n) + 2,
// so the trail was built for one commit per 1/60 s at most (data +0x5C is the lifetime in sixtieths in nearly every
// effect). 112 of PTDE's 165 trail nodes have a step of 0, which commits every frame: above 60 FPS the history fills
// before the lifetime ends and the oldest points are dropped early, so the trail is cut to lifetime * 60 / FPS. At 240
// FPS the usual 1/3 s trail (sorceries, enemy attacks) was a quarter as long. The step is raised so that the history
// holds the whole lifetime, at 90% of it as at 60 FPS, and never beyond 0.03 s (just under a 30 FPS frame). Up to 60
// FPS every frame still commits, as before; above it the trail gets a point about every 1/60 s, and its head still
// follows the particle every frame (the uncommitted points are redrawn each update).
namespace {

// 0xB9242F: fld dword ptr [edx+54h] (the node's step; edx = the node's data) ; fcompp (with ST1 = now - last
// commit, the x87 stack's only value); then fnstsw ax ; test ah,5 ; jp no-commit. [ebp-14h] = the trail.
constexpr uint32_t kTrailCommit = 0x00B9242F;
constexpr uint8_t kTrailCommitBytes[5] = {0xD9, 0x42, 0x54, 0xDE, 0xD9};
constexpr float kTrailMaxStep = 0.03f;
constexpr float kTrailFill = 0.9f;
volatile LONG g_trail_fix = 0;
bool g_trail_stats = false;

// Trace statistics (once a second): updates, commits, updates of a trail whose history is full, and for those the
// time its points cover against its lifetime (thousandths, summed, and the least).
volatile LONG g_trail_updates = 0;
volatile LONG g_trail_commits = 0;
volatile LONG g_trail_full = 0;
volatile LONG g_trail_cover_sum = 0;
volatile LONG g_trail_cover_min = 1000000;
LARGE_INTEGER g_trail_freq{};
LARGE_INTEGER g_trail_since{};
LONG g_trail_frames = 0;
int g_trail_lines = 0;

void trail_note(const uint8_t* self, uint32_t cap, uint32_t slack, float life, float elapsed, float use) {
    InterlockedIncrement(&g_trail_updates);
    if (use < elapsed) InterlockedIncrement(&g_trail_commits);
    __try {
        const uint32_t count = *reinterpret_cast<const uint32_t*>(self + 0x5C);
        if (count + slack < cap || !(life > 0.0f)) return;
        const float* stamps = *reinterpret_cast<float* const*>(self + 0x68);
        const float now = elapsed + *reinterpret_cast<const float*>(self + 0x0C);
        const LONG cover = static_cast<LONG>((now - stamps[0]) / life * 1000.0f);
        InterlockedIncrement(&g_trail_full);
        InterlockedExchangeAdd(&g_trail_cover_sum, cover);
        LONG cur = g_trail_cover_min;
        while (cover < cur) {
            const LONG seen = InterlockedCompareExchange(&g_trail_cover_min, cover, cur);
            if (seen == cur) break;
            cur = seen;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

float __cdecl trail_step(const uint8_t* data, const uint8_t* self, float elapsed) {
    float use = 0.0f;
    uint32_t cap = 0, slack = 0;
    float life = 0.0f;
    __try {
        use = *reinterpret_cast<const float*>(data + 0x54);
        const int sub = *reinterpret_cast<const int*>(data + 0x60);
        life = *reinterpret_cast<const float*>(data + 0x58);
        cap = *reinterpret_cast<const uint32_t*>(self + 0x58);
        const uint32_t n = sub > 0 ? static_cast<uint32_t>(sub) + 1 : 1;
        slack = 2 * n + 1;  // the update keeps room for n new points, n pending and one more
        if (g_trail_fix && life > 0.0f && cap > slack) {
            float need = kTrailFill * static_cast<float>(n) * life / static_cast<float>(cap - slack);
            if (need > kTrailMaxStep) need = kTrailMaxStep;
            if (need > use) use = need;  // a NaN step stays NaN: no commit, as before
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (g_trail_stats) trail_note(self, cap, slack, life, elapsed, use);
    return use;
}

// Called in place of the 5 bytes: ST0 = elapsed on entry; returns with fcompp done (step against elapsed).
__declspec(naked) void trail_stub() {
    __asm {
        sub esp, 4
        fstp dword ptr [esp]         // elapsed; the x87 stack is empty for the call
        push eax
        push ecx
        push edx
        push dword ptr [esp + 12]    // elapsed
        push dword ptr [ebp - 14h]   // the trail
        push edx                     // the node's data
        call trail_step              // ST0 = the step to use
        add esp, 12
        pop edx
        pop ecx
        pop eax
        fld dword ptr [esp]          // ST0 = elapsed, ST1 = step
        fxch st(1)
        add esp, 4
        fcompp
        ret
    }
}

}  // namespace

bool sfx_trails_install(bool fix, bool stats) {
    auto* p = reinterpret_cast<uint8_t*>(kTrailCommit);
    if (std::memcmp(p, kTrailCommitBytes, sizeof(kTrailCommitBytes)) != 0) {
        LOG_ERROR("Effect trail commit at %08X does not match this build. Not patching.", kTrailCommit);
        return false;
    }
    g_trail_fix = fix ? 1 : 0;
    g_trail_stats = stats;
    QueryPerformanceFrequency(&g_trail_freq);
    QueryPerformanceCounter(&g_trail_since);
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(kTrailCommitBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xE8;  // call trail_stub
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&trail_stub) - (kTrailCommit + 5));
    std::memcpy(p + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(kTrailCommitBytes));
    DWORD ignored = 0;
    VirtualProtect(p, sizeof(kTrailCommitBytes), old, &ignored);
    if (fix) LOG_INFO("Effect trails keep their whole length at any frame rate (%08X)", kTrailCommit);
    if (stats) LOG_INFO("SfxTrails: effect trail updates are counted once a second while they run");
    return true;
}

void sfx_trails_set(bool fix) { InterlockedExchange(&g_trail_fix, fix ? 1 : 0); }

void sfx_trails_tick() {
    if (!g_trail_stats) return;
    ++g_trail_frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double secs =
        static_cast<double>(now.QuadPart - g_trail_since.QuadPart) / static_cast<double>(g_trail_freq.QuadPart);
    if (secs < 1.0) return;
    const LONG updates = InterlockedExchange(&g_trail_updates, 0), commits = InterlockedExchange(&g_trail_commits, 0);
    const LONG full = InterlockedExchange(&g_trail_full, 0), sum = InterlockedExchange(&g_trail_cover_sum, 0);
    const LONG least = InterlockedExchange(&g_trail_cover_min, 1000000);
    const LONG frames = g_trail_frames;
    g_trail_frames = 0;
    g_trail_since = now;
    if (updates == 0 || g_trail_lines >= 1800) return;
    ++g_trail_lines;
    LOG_INFO("SfxTrails: %.2f s, %ld frames (%.0f FPS), fix %s: %ld trail updates, %ld commits; %ld updates with a "
             "full history, covering %.2f of the lifetime on average (least %.2f)",
             secs, frames, frames / secs, g_trail_fix ? "on" : "off", updates, commits, full,
             full ? sum / 1000.0 / full : 0.0, full ? least / 1000.0 : 0.0);
}

bool sfx_install(bool raise) {
    g_raise = raise;
    // mov eax,[0x013787C8] ; push esi ; push edi
    static const uint8_t kExpect[7] = {0xA1, 0xC8, 0x87, 0x37, 0x01, 0x56, 0x57};
    auto* site = reinterpret_cast<uint8_t*>(kRegisterEffect);
    if (std::memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        LOG_ERROR("Effect registration at %08X does not match this build. Not patching.", kRegisterEffect);
        return false;
    }
    constexpr size_t kStolen = 5;  // the mov uses an absolute address, so it can move as is
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, site, kStolen);
    tramp[kStolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((kRegisterEffect + kStolen) - reinterpret_cast<uintptr_t>(tramp + kStolen + 5));
    std::memcpy(tramp + kStolen + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 32);
    g_orig = reinterpret_cast<RegisterFn>(tramp);

    DWORD old = 0;
    if (!VirtualProtect(site, kStolen, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_register) - (kRegisterEffect + 5));
    std::memcpy(site + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), site, kStolen);
    DWORD ignored = 0;
    VirtualProtect(site, kStolen, old, &ignored);
    if (raise) {
        LOG_INFO("Effect spawn intervals below 1/30 s are raised to just under 1/30 s as effects load (Remastered's "
                 "60 FPS rule, off the 30 FPS frame)");
    }
    return true;
}
