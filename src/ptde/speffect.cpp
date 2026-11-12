#include "speffect.h"

#include "effectlife.h"
#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kNewSite = 0x00DBE5E6;      // comiss xmm1, xmm0 ; movss [ebx], xmm0   (fn 0xDBE5A0)
constexpr uint32_t kRefreshSite = 0x00DBE560;  // comiss xmm1, xmm0 ; movss [esi], xmm0   (fn 0xDBE520)
constexpr uint32_t kTickSite = 0x00DBE800;     // movss xmm0, [ecx+0Ch] ; movss [eax+8], xmm0 (fn 0xDBE7B0)
constexpr double kRate = 30.0;

void __cdecl store_life(float* slot, float life) {
    *slot = effectlife_adjust(life, fixes_last_dt(), kRate);
}

void __cdecl store_tick(float* slot, float interval, float dt) {
    *slot = effecttick_next(*slot, interval, dt, kRate);
}

// A tick (fn 0xDBE7B0): eax = the node (tick timer at +8), ecx = its SpEffectParam row (motionInterval at +0xC),
// xmm2 = the frame time. Stores the timer and leaves it in xmm0 as the game's two instructions did; every other
// register is kept (xmm1 = 0 and edx = 0 are used after).
__declspec(naked) void tick_stub() {
    __asm {
        pushad
        sub esp, 0x80
        movups xmmword ptr [esp + 0x00], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        sub esp, 4
        movss dword ptr [esp], xmm2
        push dword ptr [ecx + 0x0C]
        lea edx, [eax + 8]
        push edx
        call store_tick
        add esp, 12
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        movss xmm0, dword ptr [eax + 8]
        ret
    }
}

// xmm0 = the game's time, xmm1 = 0; ebx (new) or esi (refreshed) = the node, its remaining time at +0.
// Every register is kept; the flags come from the game's comiss.
__declspec(naked) void new_stub() {
    __asm {
        pushad
        sub esp, 0x80
        movups xmmword ptr [esp + 0x00], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        sub esp, 4
        movss dword ptr [esp], xmm0
        push ebx
        call store_life
        add esp, 8
        movups xmm0, xmmword ptr [esp + 0x00]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        comiss xmm1, xmm0
        ret
    }
}

__declspec(naked) void refresh_stub() {
    __asm {
        pushad
        sub esp, 0x80
        movups xmmword ptr [esp + 0x00], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        sub esp, 4
        movss dword ptr [esp], xmm0
        push esi
        call store_life
        add esp, 8
        movups xmm0, xmmword ptr [esp + 0x00]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        comiss xmm1, xmm0
        ret
    }
}

bool write_call(uint32_t site, size_t size, const void* target) {
    uint8_t bytes[16];
    if (size < 5 || size > sizeof(bytes)) return false;
    bytes[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(bytes + 1, &rel, 4);
    for (size_t i = 5; i < size; ++i) bytes[i] = 0x90;
    auto* p = reinterpret_cast<uint8_t*>(site);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

}  // namespace

bool speffect_install() {
    static const uint8_t kNewExpect[9] = {0x0F, 0x2F, 0xC8, 0xF3, 0x0F, 0x11, 0x03, 0x72, 0x12};
    static const uint8_t kRefreshExpect[11] = {0x0F, 0x2F, 0xC8, 0xF3, 0x0F, 0x11, 0x06, 0xB0, 0x01, 0x72, 0x11};
    if (std::memcmp(reinterpret_cast<const void*>(kNewSite), kNewExpect, sizeof(kNewExpect)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kRefreshSite), kRefreshExpect, sizeof(kRefreshExpect)) != 0) {
        LOG_ERROR("SpEffect timing at %08X does not match this build. Not patching.", kNewSite);
        return false;
    }
    if (!write_call(kNewSite, 7, &new_stub) || !write_call(kRefreshSite, 7, &refresh_stub)) {
        LOG_ERROR("Could not patch the SpEffect timing (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("SpEffect lifetimes expire on the original game's 30 FPS frames (sites %08X, %08X)", kNewSite,
             kRefreshSite);
    static const uint8_t kTickExpect[12] = {0xF3, 0x0F, 0x10, 0x41, 0x0C, 0xF3, 0x0F, 0x11, 0x40, 0x08, 0xEB, 0x0E};
    if (std::memcmp(reinterpret_cast<const void*>(kTickSite), kTickExpect, sizeof(kTickExpect)) != 0) {
        LOG_ERROR("SpEffect ticks at %08X do not match this build. Not patching them.", kTickSite);
    } else if (!write_call(kTickSite, 10, &tick_stub)) {
        LOG_ERROR("Could not patch the SpEffect ticks (Win32=%lu)", GetLastError());
    } else {
        LOG_INFO("SpEffect ticks (heals and damage over time, lava) come on the original game's 30 FPS frames "
                 "(site %08X)", kTickSite);
    }
    return true;
}
