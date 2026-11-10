#include "ffx.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uintptr_t kImageBase = 0x400000;

// The library's code (all 761 debug prologues and all 1,210 guard checks lie in here).
constexpr uintptr_t kLibLo = 0x960000;
constexpr uintptr_t kLibHi = 0xBA6000;
constexpr int kExpectFills = 761;

// _RTC_CheckStackVars(frame in ecx, descriptor in edx): its callers save the registers they need around the call.
constexpr uintptr_t kCheckStackVars = 0xAC6A98;
constexpr uint8_t kCheckStackVarsExpect[11] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x51, 0x51, 0x53, 0x56, 0x8B, 0xF2};

// lea edi, [ebp-X] ; mov ecx, N ; mov eax, 0CCCCCCCCh ; rep stosd
bool is_fill(const uint8_t* p, size_t avail) {
    if (avail < 12) return false;
    return p[0] == 0xB9 && p[5] == 0xB8 && p[6] == 0xCC && p[7] == 0xCC && p[8] == 0xCC && p[9] == 0xCC &&
           p[10] == 0xF3 && p[11] == 0xAB;
}

bool after_lea_edi(const uint8_t* p) {
    return (p[-3] == 0x8D && p[-2] == 0x7D) || (p[-6] == 0x8D && p[-5] == 0xBD);
}

}  // namespace

bool ffx_install() {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        LOG_ERROR("FastEffects needs the game at its preferred base. Not patching.");
        return false;
    }
    auto* check = reinterpret_cast<uint8_t*>(kCheckStackVars);
    if (std::memcmp(check, kCheckStackVarsExpect, sizeof(kCheckStackVarsExpect)) != 0) {
        LOG_ERROR("FastEffects: _RTC_CheckStackVars does not match this build. Not patching.");
        return false;
    }
    auto* lo = reinterpret_cast<uint8_t*>(kLibLo);
    const size_t size = kLibHi - kLibLo;
    uint8_t* fills[kExpectFills + 1];
    int count = 0;
    for (size_t i = 8; i + 12 <= size && count <= kExpectFills; ++i) {
        if (is_fill(lo + i, size - i) && after_lea_edi(lo + i)) {
            fills[count++] = lo + i;
            i += 11;
        }
    }
    if (count != kExpectFills) {
        LOG_ERROR("FastEffects: found %d debug prologues in the effects library, expected %d. Not patching.", count,
                  kExpectFills);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(lo, size, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("FastEffects: could not unprotect the effects library (Win32=%lu)", GetLastError());
        return false;
    }
    unsigned long long dwords = 0;
    for (int i = 0; i < count; ++i) {
        uint32_t n = 0;
        std::memcpy(&n, fills[i] + 1, 4);
        dwords += n;
        const uint32_t zero = 0;
        std::memcpy(fills[i] + 1, &zero, 4);  // mov ecx, 0: rep stosd stores nothing, and ecx ends at 0 as before
    }
    DWORD ignored = 0;
    VirtualProtect(lo, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), lo, size);

    DWORD old_check = 0;
    if (!VirtualProtect(check, 1, PAGE_EXECUTE_READWRITE, &old_check)) {
        LOG_ERROR("FastEffects: could not unprotect _RTC_CheckStackVars (Win32=%lu)", GetLastError());
        return false;
    }
    *check = 0xC3;  // ret
    VirtualProtect(check, 1, old_check, &ignored);
    FlushInstructionCache(GetCurrentProcess(), check, 1);

    LOG_INFO("FastEffects: the effects library's debug checks are off (%d stack fills of %llu bytes in all, and the "
             "guard check on return)",
             count, dwords * 4);
    return true;
}
