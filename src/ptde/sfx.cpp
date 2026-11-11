#include "sfx.h"

#include "log.h"
#include "sfxwatch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kRegisterEffect = 0x00D19CF0;
constexpr float kMinInterval = 1.0f / 30.0f;

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
            std::memcpy(d + at, &kMinInterval, 4);
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
        LOG_INFO("Effect spawn intervals: %d raised to 1/30 s in the %d effects loaded so far", total, effects);
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

// Entered by `call` in place of `call [ebp-8] ; add esp,10h`: [esp] the return, then handler, node, args, x.
__declspec(naked) void action_call_stub() {
    __asm {
        push dword ptr [esp + 8]
        call action_begin
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
        LOG_INFO("Effect spawn intervals below 1/30 s are raised to 1/30 s as effects load (Remastered's 60 FPS rule)");
    }
    return true;
}
