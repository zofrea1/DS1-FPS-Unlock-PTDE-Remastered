#include "flick.h"

#include "log.h"
#include "stickflick.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kFlickFn = 0x00E16F80;
constexpr uint32_t kFlickCall = 0x00E1D6B0;    // call 0E16F80h
constexpr uint32_t kPadUpdateFn = 0x00E1C380;  // PadManipulator::Update
constexpr uint32_t kPadUpdateSlot = 0x0118E25C;  // its vtable slot (PadManipulator vtable 0x118E23C, slot 8)
constexpr uint32_t kWindow = 0x012DEF68;       // 0.06 s
constexpr uint32_t kOn = 0x012DEF6C;           // 0.3
constexpr uint32_t kNeutral = 0x012DEF70;      // 0.2

using PadUpdateFn = void(__thiscall*)(void* self, float dt, void* chr);
PadUpdateFn g_pad_update = nullptr;

void __fastcall hk_pad_update(void* self, void* /*edx*/, float dt, void* chr) {
    g_pad_update(self, dt, chr);
    stickflick_after_update(static_cast<uint8_t*>(self));
}

// The game's call: ecx = manipulator, eax = result, xmm3 = forward, [esp+4] = frame time, [esp+8] = side;
// ret 8 with eax = result. ecx and edx are kept as the game's function keeps ecx.
__declspec(naked) void flick_stub() {
    __asm {
        push ebp
        mov ebp, esp
        push ecx
        push edx
        sub esp, 4
        movss dword ptr [esp], xmm3  // forward (fifth argument)
        push dword ptr [ebp + 12]    // side
        push dword ptr [ebp + 8]     // frame time
        push eax                     // result
        push ecx                     // manipulator
        call stickflick_compute      // returns the result in eax
        add esp, 20
        pop edx
        pop ecx
        pop ebp
        ret 8
    }
}

bool write_code(uint32_t address, const void* bytes, size_t size) {
    auto* p = reinterpret_cast<uint8_t*>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

bool near_value(uint32_t address, float expected) {
    float v = 0.0f;
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(v));
    return v > expected - 1e-4f && v < expected + 1e-4f;
}

}  // namespace

bool flick_install() {
    static const uint8_t kCallExpect[5] = {0xE8, 0xCB, 0x98, 0xFF, 0xFF};  // call 0E16F80h
    static const uint8_t kFnExpect[14] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xF3,
                                          0x0F, 0x10, 0x0D, 0x50, 0x7D, 0x1E, 0x01};
    const uint32_t slot_value = *reinterpret_cast<const uint32_t*>(kPadUpdateSlot);
    if (std::memcmp(reinterpret_cast<const void*>(kFlickCall), kCallExpect, sizeof(kCallExpect)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kFlickFn), kFnExpect, sizeof(kFnExpect)) != 0 ||
        slot_value != kPadUpdateFn || !near_value(kWindow, 0.06f) || !near_value(kOn, 0.3f) ||
        !near_value(kNeutral, 0.2f)) {
        LOG_ERROR("Stick flick at %08X does not match this build. Not patching.", kFlickCall);
        return false;
    }
    StickFlickLayout layout;
    layout.neutral = 0x239;
    layout.timers = 0x2F4;
    StickFlickConsts consts;
    consts.window = reinterpret_cast<const float*>(kWindow);
    consts.on = reinterpret_cast<const float*>(kOn);
    consts.neutral = reinterpret_cast<const float*>(kNeutral);
    stickflick_configure(layout, consts, 30.0);

    g_pad_update = reinterpret_cast<PadUpdateFn>(kPadUpdateFn);
    uint8_t call[5];
    call[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&flick_stub) - (kFlickCall + 5));
    std::memcpy(call + 1, &rel, 4);
    const uint32_t hook = reinterpret_cast<uint32_t>(&hk_pad_update);
    if (!write_code(kPadUpdateSlot, &hook, sizeof(hook))) {
        LOG_ERROR("Could not hook the pad update (Win32=%lu)", GetLastError());
        return false;
    }
    if (!write_code(kFlickCall, call, sizeof(call))) {
        write_code(kPadUpdateSlot, &kPadUpdateFn, sizeof(kPadUpdateFn));
        LOG_ERROR("Could not patch the stick flick (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("Kick and jumping attack input (stick flick) follow the original game's 30 FPS timing (call %08X, "
             "pad update slot %08X)",
             kFlickCall, kPadUpdateSlot);
    return true;
}
