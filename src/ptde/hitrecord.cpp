#include "hitrecord.h"

#include "effectlife.h"
#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kSite = 0x00FD7D54;  // fn 0xFD7CF0: movss xmm0, [esp+0Ch] ; mov [eax], ebx ; movss [eax+4], xmm0
constexpr size_t kSiteSize = 13;
constexpr double kRate = 30.0;

void __cdecl store_hit_time(uint8_t* entry, float life) {
    *reinterpret_cast<float*>(entry + 4) = hitrecord_adjust(life, fixes_last_dt(), kRate);
}

// At the site: eax = the new entry, ebx = the target id, [esp] = the caller's ebx, [esp+4] = return address,
// [esp+8] = id, [esp+0Ch] = the record's lifetime. Finishes the function: entry +0 = id, +4 = adjusted time,
// eax = entry, pop ebx, ret 8.
__declspec(naked) void tail_stub() {
    __asm {
        mov dword ptr [eax], ebx
        push eax
        push ecx
        push edx
        push dword ptr [esp + 18h]  // the lifetime ([esp+0Ch] at the site)
        push eax
        call store_hit_time
        add esp, 8
        pop edx
        pop ecx
        pop eax
        pop ebx
        ret 8
    }
}

}  // namespace

bool hitrecord_install() {
    static const uint8_t kExpect[17] = {0xF3, 0x0F, 0x10, 0x44, 0x24, 0x0C, 0x89, 0x18, 0xF3,
                                        0x0F, 0x11, 0x40, 0x04, 0x5B, 0xC2, 0x08, 0x00};
    auto* site = reinterpret_cast<uint8_t*>(kSite);
    if (std::memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        LOG_ERROR("Bullet hit record timing at %08X does not match this build. Not patching.", kSite);
        return false;
    }
    uint8_t patch[kSiteSize];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&tail_stub) - (kSite + 5));
    std::memcpy(patch + 1, &rel, 4);
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("Could not patch the bullet hit record timing (Win32=%lu)", GetLastError());
        return false;
    }
    std::memcpy(site, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(site, sizeof(patch), old, &ignored);
    LOG_INFO("Bullet hit records run out on the original game's 30 FPS frames (site %08X)", kSite);
    return true;
}
