#include "roll.h"

#include "log.h"
#include "rollhistory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kWalk = 0x00E17709;      // mov ecx, [ebx+2F0h]   (start of the ring walk)
constexpr uint32_t kAfterWalk = 0x00E1783C;  // mov edi, [ebp+10h]
constexpr uint32_t kRingSize = 0x012DEF5C;   // int 6 (the debug menu's lever input period in frames)

alignas(16) float g_out[4];

void __cdecl walk(uint8_t* manip, float dt) {
    rollhistory_compute(manip, dt, g_out);
}

__declspec(naked) void walk_stub() {
    __asm {
        pushad
        push dword ptr [ebp + 0x0C]  // frame time
        push ebx                     // manipulator
        call walk
        add esp, 8
        popad
        movss xmm0, dword ptr [g_out + 0]
        movss dword ptr [esp + 0x18], xmm0
        movaps xmm4, xmm0
        movss xmm0, dword ptr [g_out + 4]
        movss dword ptr [esp + 0x1C], xmm0
        movss xmm0, dword ptr [g_out + 8]
        movss dword ptr [esp + 0x20], xmm0
        movss xmm3, dword ptr [g_out + 12]
        movss dword ptr [esp + 0x40], xmm3
        xorps xmm0, xmm0
        push 0x00E1783C
        ret
    }
}

bool write_jump(uint32_t site, size_t size, const void* dest) {
    auto* p = reinterpret_cast<uint8_t*>(site);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(dest) - (site + 5));
    p[0] = 0xE9;
    std::memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < size; ++i) p[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

}  // namespace

bool roll_install() {
    static const uint8_t kWalkExpect[12] = {0x8B, 0x8B, 0xF0, 0x02, 0x00, 0x00,   // mov ecx, [ebx+2F0h]
                                            0x8B, 0x3D, 0x5C, 0xEF, 0x2D, 0x01};  // mov edi, [012DEF5Ch]
    static const uint8_t kAfterExpect[3] = {0x8B, 0x7D, 0x10};                    // mov edi, [ebp+10h]
    static const uint8_t kStoreExpect[9] = {0xF3, 0x0F, 0x11, 0x64, 0x24, 0x18,   // movss [esp+18h], xmm4
                                            0x8B, 0x7D, 0x10};
    if (std::memcmp(reinterpret_cast<const void*>(kWalk), kWalkExpect, sizeof(kWalkExpect)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kAfterWalk), kAfterExpect, sizeof(kAfterExpect)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kAfterWalk - 6), kStoreExpect, sizeof(kStoreExpect)) != 0) {
        LOG_ERROR("Roll direction history at %08X does not match this build. Not patching.", kWalk);
        return false;
    }
    RollHistoryLayout layout;
    layout.ring_y = 0x250;
    layout.ring_x = 0x2A0;
    layout.index = 0x2F0;
    rollhistory_configure(layout, reinterpret_cast<const int*>(kRingSize), 30.0);
    if (!write_jump(kWalk, 6, &walk_stub)) {
        LOG_ERROR("Could not patch the roll direction history (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("Roll direction reads the stick over the original game's last 4 frames of 1/30 s (site %08X)", kWalk);
    return true;
}
