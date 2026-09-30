#include "snap.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

// The movement controller's frame (fn 0xEC08B0) is: lift the physics proxy by the step
// height (setPosition at 0xEC09BA), move it, then put it back down by adding [body+0xB4]
// (-0.3) to its height (the load at 0xEC0AF8). When the lift is skipped, as at the bottom of
// a ladder slide, the -0.3 is still applied, so the body is pulled down 0.3 through the
// floor on every frame of the exit until it finds a lower one: one frame at 30 FPS, many at
// high rates. The loaded operand is scaled here so an unbalanced frame is capped at the one
// retail frame's worth (0.3) per episode, spread over frames by dt*30.
//
// Whether a frame was balanced is recorded exactly: a stub in front of the lift's setPosition
// call notes which body was lifted; the snap site reads and clears that note.
constexpr uint32_t kLiftCall = 0x00EC09BA;   // call 0x8F2380 (proxy setPosition, lifted)
constexpr uint32_t kSetPosition = 0x008F2380;
constexpr uint32_t kSite = 0x00EC0AF8;       // movss xmm1, [ebx+0xB4]
constexpr uint32_t kReturn = 0x00EC0B00;

constexpr float kStep = 0.3f;
constexpr float kCap = 0.3f;

struct Episode {
    const void* body = nullptr;
    float applied = 0.0f;
    ULONGLONG last_ms = 0;
};
Episode g_episodes[16];
volatile long g_capped = 0;
volatile long g_enabled = 1;

// Written by the lift stub (machine code): slot (body >> 4) & 0xFF holds the body pointer.
uint32_t g_lifted[256] = {};

struct Debug {
    const void* body = nullptr;
    float dy = 0.0f;
    float factor = 1.0f;
    int lifted = 0;
    unsigned calls = 0;
};
Debug g_debug[64];

Debug* debug_slot(const void* body) {
    return &g_debug[(reinterpret_cast<uintptr_t>(body) >> 4) & 63];
}

float __cdecl snap_factor(const void* body, float proxy_y, float start_y) {
    const uint32_t key = (reinterpret_cast<uint32_t>(body) >> 4) & 0xFF;
    const bool lifted = g_lifted[key] == reinterpret_cast<uint32_t>(body);
    g_lifted[key] = 0;
    float result = 1.0f;
    if (g_enabled && !lifted) {
        const float dt = static_cast<float>(fixes_last_dt());
        const ULONGLONG now = GetTickCount64();
        Episode* slot = nullptr;
        Episode* stale = &g_episodes[0];
        for (auto& e : g_episodes) {
            if (e.body == body) {
                slot = &e;
                break;
            }
            if (e.last_ms < stale->last_ms) stale = &e;
        }
        if (!slot) {
            slot = stale;
            slot->body = body;
            slot->applied = 0.0f;
        }
        if (now - slot->last_ms > 250) slot->applied = 0.0f;
        slot->last_ms = now;
        float factor = dt * 30.0f;
        if (factor > 1.0f) factor = 1.0f;
        const float remaining = kCap - slot->applied;
        if (remaining <= 0.0f) {
            InterlockedIncrement(&g_capped);
            factor = 0.0f;
        } else {
            if (kStep * factor > remaining) factor = remaining / kStep;
            slot->applied += kStep * factor;
        }
        result = factor;
    } else if (lifted) {
        for (auto& e : g_episodes) {
            if (e.body == body) e.applied = 0.0f;  // balanced frame: the next unbalanced run is a new episode
        }
    }
    Debug* d = debug_slot(body);
    d->body = body;
    d->dy = proxy_y - start_y;
    d->factor = result;
    d->lifted = lifted ? 1 : 0;
    ++d->calls;
    return result;
}

}  // namespace

bool snap_install() {
    auto* site = reinterpret_cast<uint8_t*>(kSite);
    static const uint8_t kExpected[8] = {0xF3, 0x0F, 0x10, 0x8B, 0xB4, 0x00, 0x00, 0x00};
    if (std::memcmp(site, kExpected, sizeof(kExpected)) != 0) {
        LOG_ERROR("Snap site %08X does not match this build. Not patching.", kSite);
        return false;
    }
    auto* call = reinterpret_cast<uint8_t*>(kLiftCall);
    int32_t call_rel = 0;
    std::memcpy(&call_rel, call + 1, 4);
    if (call[0] != 0xE8 || static_cast<uint32_t>(kLiftCall + 5 + call_rel) != kSetPosition) {
        LOG_ERROR("Lift call at %08X is not a call to %08X. Not patching.", kLiftCall, kSetPosition);
        return false;
    }
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x200, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) {
        LOG_ERROR("Could not allocate the snap cave (Win32=%lu)", GetLastError());
        return false;
    }
    uint8_t* p = cave;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    };
    auto emit32 = [&](uint32_t v) {
        std::memcpy(p, &v, 4);
        p += 4;
    };

    // Lift stub: note the body, then continue into setPosition.
    uint8_t* lift_stub = p;
    emit({0x50});                                      // push eax
    emit({0x89, 0xD8});                                // mov eax,ebx
    emit({0xC1, 0xE8, 0x04});                          // shr eax,4
    emit({0x25, 0xFF, 0x00, 0x00, 0x00});              // and eax,0xFF
    emit({0x89, 0x1C, 0x85});                          // mov [g_lifted + eax*4], ebx
    emit32(reinterpret_cast<uint32_t>(g_lifted));
    emit({0x58});                                      // pop eax
    emit({0xE9});                                      // jmp setPosition
    emit32(kSetPosition - (reinterpret_cast<uint32_t>(p) + 4));

    // Snap stub: xmm1 = [ebx+0xB4] * snap_factor(body, proxy y, body y)
    uint8_t* snap_stub = p;
    emit({0x9C});                                      // pushfd
    emit({0x60});                                      // pushad
    emit({0x83, 0xEC, 0x10});                          // sub esp,0x10
    emit({0x0F, 0x11, 0x04, 0x24});                    // movups [esp],xmm0
    emit({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x48});        // movss xmm0,[esp+0x48]    (proxy y = original [esp+0x14])
    emit({0x83, 0xEC, 0x0C});                          // sub esp,0xC
    emit({0xF3, 0x0F, 0x11, 0x44, 0x24, 0x04});        // movss [esp+4],xmm0       (arg 2: proxy y)
    emit({0x8B, 0x43, 0x14});                          // mov eax,[ebx+0x14]       (body y, the frame-start height)
    emit({0x89, 0x44, 0x24, 0x08});                    // mov [esp+8],eax          (arg 3)
    emit({0x89, 0x1C, 0x24});                          // mov [esp],ebx            (arg 1: body)
    emit({0xB8});                                      // mov eax, snap_factor
    emit32(reinterpret_cast<uint32_t>(&snap_factor));
    emit({0xFF, 0xD0});                                // call eax
    emit({0xD9, 0x1C, 0x24});                          // fstp dword [esp]
    emit({0xF3, 0x0F, 0x10, 0x0C, 0x24});              // movss xmm1,[esp]         (factor)
    emit({0x83, 0xC4, 0x0C});                          // add esp,0xC
    emit({0x0F, 0x10, 0x04, 0x24});                    // movups xmm0,[esp]
    emit({0x83, 0xC4, 0x10});                          // add esp,0x10
    emit({0xF3, 0x0F, 0x59, 0x8B, 0xB4, 0x00, 0x00, 0x00});  // mulss xmm1,[ebx+0xB4]
    emit({0x61});                                      // popad
    emit({0x9D});                                      // popfd
    emit({0xE9});                                      // jmp back
    emit32(kReturn - (reinterpret_cast<uint32_t>(p) + 4));
    FlushInstructionCache(GetCurrentProcess(), cave, 0x200);

    DWORD old = 0;
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("Could not unprotect the snap site (Win32=%lu)", GetLastError());
        return false;
    }
    uint8_t patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(snap_stub) - (kSite + 5));
    std::memcpy(patch + 1, &rel, 4);
    std::memcpy(site, patch, 8);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    DWORD ignored = 0;
    VirtualProtect(site, 8, old, &ignored);

    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("Could not unprotect the lift call (Win32=%lu)", GetLastError());
        return false;
    }
    const int32_t lift_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(lift_stub) - (kLiftCall + 5));
    std::memcpy(call + 1, &lift_rel, 4);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    VirtualProtect(call + 1, 4, old, &ignored);
    LOG_INFO("Ladder/ledge snap cap installed (snap %08X, lift call %08X, cave %p)", kSite, kLiftCall, cave);
    return true;
}

long snap_capped_count() {
    return g_capped;
}

void snap_set_enabled(bool on) {
    g_enabled = on ? 1 : 0;
}

bool snap_enabled() {
    return g_enabled != 0;
}

bool snap_debug(const void* body, float* dy, float* factor, int* lifted, unsigned* calls) {
    const Debug* d = debug_slot(body);
    if (d->body != body) return false;
    *dy = d->dy;
    *factor = d->factor;
    *lifted = d->lifted;
    *calls = d->calls;
    return true;
}
