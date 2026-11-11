#include "rendermath.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <xmmintrin.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uintptr_t kImageBase = 0x400000;

constexpr uintptr_t kVec4Add = 0x4078D0;
constexpr uint8_t kVec4AddExpect[19] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x4D, 0x0C, 0x8B,
                                        0x55, 0x10, 0x8B, 0x45, 0x08, 0xF3, 0x0F, 0x10, 0x01};
constexpr uintptr_t kMat34Add = 0x421E70;
constexpr uint8_t kMat34AddExpect[11] = {0x51, 0x56, 0x50, 0x8B, 0x44, 0x24, 0x10, 0x8B, 0xF1, 0xE8, 0x62};

// cdecl, out returned in eax; clobbers ecx, edx, xmm0, xmm1 like the original.
float* __cdecl vec4_add(float* out, const float* a, const float* b) {
    _mm_storeu_ps(out, _mm_add_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
    return out;
}

// out (ecx) = A (eax) + B ([esp+4]), three rows of four floats; returns out in eax, keeps ecx, pops B.
__declspec(naked) void mat34_add() {
    __asm {
        mov edx, [esp + 4]
        movups xmm0, [eax]
        movups xmm1, [edx]
        addps xmm0, xmm1
        movups [ecx], xmm0
        movups xmm0, [eax + 16]
        movups xmm1, [edx + 16]
        addps xmm0, xmm1
        movups [ecx + 16], xmm0
        movups xmm0, [eax + 32]
        movups xmm1, [edx + 32]
        addps xmm0, xmm1
        movups [ecx + 32], xmm0
        mov eax, ecx
        ret 4
    }
}

bool write_jump(uintptr_t site, const void* target) {
    uint8_t bytes[5] = {0xE9};
    const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(bytes + 1, &rel, 4);
    auto* p = reinterpret_cast<uint8_t*>(site);
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(bytes), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(p, bytes, sizeof(bytes));
    DWORD ignored = 0;
    VirtualProtect(p, sizeof(bytes), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(bytes));
    return true;
}

}  // namespace

bool rendermath_install() {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        LOG_ERROR("FastRenderMath needs the game at its preferred base. Not patching.");
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kVec4Add), kVec4AddExpect, sizeof(kVec4AddExpect)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kMat34Add), kMat34AddExpect, sizeof(kMat34AddExpect)) != 0) {
        LOG_ERROR("FastRenderMath: the math helpers do not match this build. Not patching.");
        return false;
    }
    if (!write_jump(kMat34Add, reinterpret_cast<const void*>(&mat34_add)) ||
        !write_jump(kVec4Add, reinterpret_cast<const void*>(&vec4_add))) {
        LOG_ERROR("FastRenderMath: could not write the jumps");
        return false;
    }
    LOG_INFO("FastRenderMath: the render thread's four-float and 3x4 matrix adds run as SSE (same results)");
    return true;
}
