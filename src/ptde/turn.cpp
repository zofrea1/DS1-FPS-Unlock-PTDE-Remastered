#include "turn.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kBoneUpdate = 0x00D901D0;
constexpr uint32_t kGainOffset = 0x2C;

using BoneFn = void*(__stdcall*)(void* controller, void* out, void* pose);
BoneFn g_orig = nullptr;
volatile long g_enabled = 1;

void* __stdcall hk_bone(void* controller, void* out, void* pose) {
    auto* gain = reinterpret_cast<float*>(static_cast<uint8_t*>(controller) + kGainOffset);
    const float saved = *gain;
    if (!g_enabled || !(saved > 0.0f) || saved >= 1.0f) {
        return g_orig(controller, out, pose);
    }
    double n = fixes_last_dt() * 30.0;
    if (n < 0.01) n = 0.01;
    if (n > 4.0) n = 4.0;
    *gain = static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(saved), n));
    void* result = g_orig(controller, out, pose);
    *gain = saved;
    return result;
}

}  // namespace

bool turn_install() {
    // push ebp ; mov ebp,esp ; and esp,-16 ; xorps xmm0,xmm0
    static const uint8_t kExpect[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x0F, 0x57, 0xC0};
    auto* site = reinterpret_cast<uint8_t*>(kBoneUpdate);
    if (std::memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        LOG_ERROR("Bone rotation controller update at %08X does not match this build. Not patching.", kBoneUpdate);
        return false;
    }
    constexpr size_t kStolen = 6;
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, site, kStolen);
    tramp[kStolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((kBoneUpdate + kStolen) - reinterpret_cast<uintptr_t>(tramp + kStolen + 5));
    std::memcpy(tramp + kStolen + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 32);
    g_orig = reinterpret_cast<BoneFn>(tramp);

    DWORD old = 0;
    if (!VirtualProtect(site, kStolen, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_bone) - (kBoneUpdate + 5));
    std::memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), site, kStolen);
    DWORD ignored = 0;
    VirtualProtect(site, kStolen, old, &ignored);
    LOG_INFO("Lock-on body turn (bone rotation blend) follows the frame time");
    return true;
}

void turn_set_enabled(bool on) {
    g_enabled = on ? 1 : 0;
}

bool turn_enabled() {
    return g_enabled != 0;
}
