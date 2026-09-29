#include "limiter.h"

#include "intro.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

// The limiter is a thiscall taking three stack arguments (`ret 0xC`); its first five
// bytes (sub esp,0x10 / push ebp / push edi) are position independent.
constexpr uint32_t kLimiterFn = 0x006455D0;
constexpr uint8_t kPrologue[7] = {0x83, 0xEC, 0x10, 0x55, 0x57, 0x8B, 0xE9};

using LimiterFn = void(__fastcall*)(void* self, void* edx, int a, int b, int c);
LimiterFn g_original = nullptr;

// __fastcall hands ecx/edx to the first two parameters and pops the rest on return,
// which matches the original's thiscall + `ret 0xC`.
void __fastcall hook_limiter(void* self, void* edx, int a, int b, int c) {
    if (g_intro_skipping) {
        return;
    }
    g_original(self, edx, a, b, c);
}

}  // namespace

bool limiter_install() {
    auto* target = reinterpret_cast<uint8_t*>(kLimiterFn);
    if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
        LOG_ERROR("Engine frame limiter at %08X does not match this build. Not hooking it.", kLimiterFn);
        return false;
    }
    // Trampoline: the stolen five bytes, then a jump back to the rest of the function.
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!tramp) {
        LOG_ERROR("Could not allocate the limiter trampoline (Win32=%lu)", GetLastError());
        return false;
    }
    std::memcpy(tramp, target, 5);
    tramp[5] = 0xE9;
    const int32_t back = static_cast<int32_t>((kLimiterFn + 5) - (reinterpret_cast<uint32_t>(tramp) + 10));
    std::memcpy(tramp + 6, &back, sizeof(back));
    FlushInstructionCache(GetCurrentProcess(), tramp, 32);
    g_original = reinterpret_cast<LimiterFn>(tramp);

    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR("Could not unprotect the engine frame limiter (Win32=%lu)", GetLastError());
        return false;
    }
    uint8_t jump[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&hook_limiter) - (kLimiterFn + 5));
    std::memcpy(jump + 1, &rel, sizeof(rel));
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    DWORD ignored = 0;
    VirtualProtect(target, 5, old, &ignored);
    LOG_INFO("Engine frame limiter hooked at %08X (bypassed only while the intro is skipped)", kLimiterFn);
    return true;
}
