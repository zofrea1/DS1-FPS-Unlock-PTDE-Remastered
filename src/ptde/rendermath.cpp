#include "rendermath.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <emmintrin.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uintptr_t kImageBase = 0x400000;

// ---------------------------------------------------------------------------------------------------------------
// The engine's four-float helpers (ported Xbox 360 vector code compiled as scalar SSE through double). A double sum,
// difference or product of two floats rounds to the same float as the float operation (double has more than twice
// float's precision), so addps/subps/mulps give the same bits. The multiply-add rounds a*b+c once from double, which
// two float operations would not reproduce, so it stays in double, two lanes at a time.

// cdecl, out returned in eax; clobber only eax, ecx, edx and xmm registers like the originals.
float* __cdecl vec4_add(float* out, const float* a, const float* b) {
    _mm_storeu_ps(out, _mm_add_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
    return out;
}

float* __cdecl vec4_sub(float* out, const float* a, const float* b) {
    _mm_storeu_ps(out, _mm_sub_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
    return out;
}

float* __cdecl vec4_mul(float* out, const float* a, const float* b) {
    _mm_storeu_ps(out, _mm_mul_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
    return out;
}

float* __cdecl vec4_madd(float* out, const float* a, const float* b, const float* c) {
    const __m128 va = _mm_loadu_ps(a), vb = _mm_loadu_ps(b), vc = _mm_loadu_ps(c);
    const __m128d lo = _mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(va), _mm_cvtps_pd(vb)), _mm_cvtps_pd(vc));
    const __m128d hi = _mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(va, va)), _mm_cvtps_pd(_mm_movehl_ps(vb, vb))),
                                  _mm_cvtps_pd(_mm_movehl_ps(vc, vc)));
    _mm_storeu_ps(out, _mm_movelh_ps(_mm_cvtpd_ps(lo), _mm_cvtpd_ps(hi)));
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

// ---------------------------------------------------------------------------------------------------------------
// The effects library's wrappers around them (unoptimized debug code: an aligned frame, the result copied through two
// stack temporaries). cdecl with the result's address first, returned in eax.

uint32_t* __cdecl ffx_set4(uint32_t* out, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
    return out;
}

uint32_t* __cdecl ffx_splat(uint32_t* out, uint32_t v) {
    out[0] = out[1] = out[2] = out[3] = v;
    return out;
}

// The original loads and stores the float through the x87 stack, which leaves every value but a signaling NaN as it
// was; the same is done here so even that stays the same.
float* __cdecl ffx_splat_x87(float* out, float v) {
    float f;
    __asm {
        fld v
        fstp f
    }
    out[0] = out[1] = out[2] = out[3] = f;
    return out;
}

float* __cdecl ffx_sub(float* out, const float* a, const float* b) {
    return vec4_sub(out, a, b);
}

struct Site {
    const char* name;
    uintptr_t address;
    const uint8_t* expect;  // checked at the address
    size_t expect_size;
    const uint8_t* expect2;  // checked at address + expect2_at (0 = none)
    size_t expect2_at;
    size_t expect2_size;
    const void* replacement;
};

const uint8_t kAddHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x4D, 0x0C, 0x8B, 0x55, 0x10, 0x8B, 0x45, 0x08,
                            0xF3, 0x0F, 0x10, 0x01, 0xF3, 0x0F, 0x10, 0x0A, 0x0F, 0x5A, 0xC0, 0x0F, 0x5A, 0xC9, 0xF2,
                            0x0F, 0x58, 0xC1};
const uint8_t kSubHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x4D, 0x0C, 0x8B, 0x55, 0x10, 0x8B, 0x45, 0x08,
                            0xF3, 0x0F, 0x10, 0x01, 0xF3, 0x0F, 0x10, 0x0A, 0x0F, 0x5A, 0xC0, 0x0F, 0x5A, 0xC9, 0xF2,
                            0x0F, 0x5C, 0xC1};
const uint8_t kMulHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x10, 0x8B, 0x45, 0x0C, 0x8B, 0x4D,
                            0x10, 0xF3, 0x0F, 0x10, 0x00, 0xF3, 0x0F, 0x10, 0x09, 0x0F, 0x5A, 0xC0, 0x0F, 0x5A,
                            0xC9, 0xF2, 0x0F, 0x59, 0xC1};
const uint8_t kMaddHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x10, 0x8B, 0x45, 0x0C, 0x8B, 0x4D,
                             0x10, 0x8B, 0x55, 0x14, 0xF3, 0x0F, 0x10, 0x00, 0xF3, 0x0F, 0x10, 0x09, 0x0F, 0x5A,
                             0xC0, 0x0F, 0x5A, 0xC9, 0xF2, 0x0F, 0x59, 0xC1, 0xF3, 0x0F, 0x10, 0x0A, 0x0F, 0x5A,
                             0xC9, 0xF2, 0x0F, 0x58, 0xC1};
const uint8_t kMat34Head[] = {0x51, 0x56, 0x50, 0x8B, 0x44, 0x24, 0x10, 0x8B, 0xF1, 0xE8, 0x62};

// The effects library's debug prologue up to the stack fill (FastEffects changes the fill's count, not these).
const uint8_t kFfxHead30[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B,
                              0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x83, 0xEC, 0x30, 0x57, 0x8D, 0x7D};
const uint8_t kFfxHead50[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B,
                              0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x83, 0xEC, 0x50, 0x57, 0x8D, 0x7D};
// What follows the fill (at +0x29).
const uint8_t kSet4Body[] = {0x8B, 0x43, 0x0C, 0x89, 0x45, 0xE0, 0x8B, 0x4B, 0x10, 0x89, 0x4D, 0xE4, 0x8B, 0x53, 0x14, 0x89};
const uint8_t kSplatBody[] = {0x8B, 0x43, 0x0C, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8B, 0x53, 0x0C, 0x52, 0x8B, 0x43, 0x0C, 0x50};
const uint8_t kSplatX87Body[] = {0x51, 0xD9, 0x43, 0x0C, 0xD9, 0x1C, 0x24, 0x8D, 0x45, 0xB0, 0x50, 0xE8, 0x67, 0xF1, 0xA7, 0xFF};
const uint8_t kSubWrapBody[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x86, 0x28, 0xA9};

const Site kCoreSites[] = {
    {"four-float add", 0x4078D0, kAddHead, sizeof(kAddHead), nullptr, 0, 0, reinterpret_cast<const void*>(&vec4_add)},
    {"four-float subtract", 0x417420, kSubHead, sizeof(kSubHead), nullptr, 0, 0, reinterpret_cast<const void*>(&vec4_sub)},
    {"four-float multiply", 0x5537E0, kMulHead, sizeof(kMulHead), nullptr, 0, 0, reinterpret_cast<const void*>(&vec4_mul)},
    {"four-float multiply-add", 0x553700, kMaddHead, sizeof(kMaddHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&vec4_madd)},
    {"3x4 matrix add", 0x421E70, kMat34Head, sizeof(kMat34Head), nullptr, 0, 0, reinterpret_cast<const void*>(&mat34_add)},
};

const Site kFfxSites[] = {
    {"set", 0x984890, kFfxHead30, sizeof(kFfxHead30), kSet4Body, 0x29, sizeof(kSet4Body),
     reinterpret_cast<const void*>(&ffx_set4)},
    {"splat", 0x9847C0, kFfxHead50, sizeof(kFfxHead50), kSplatBody, 0x29, sizeof(kSplatBody),
     reinterpret_cast<const void*>(&ffx_splat)},
    {"splat (x87)", 0x984920, kFfxHead50, sizeof(kFfxHead50), kSplatX87Body, 0x29, sizeof(kSplatX87Body),
     reinterpret_cast<const void*>(&ffx_splat_x87)},
    {"subtract", 0x984B60, kFfxHead50, sizeof(kFfxHead50), kSubWrapBody, 0x29, sizeof(kSubWrapBody),
     reinterpret_cast<const void*>(&ffx_sub)},
};

bool matches(const Site& s) {
    if (std::memcmp(reinterpret_cast<const void*>(s.address), s.expect, s.expect_size) != 0) return false;
    return !s.expect2 ||
           std::memcmp(reinterpret_cast<const void*>(s.address + s.expect2_at), s.expect2, s.expect2_size) == 0;
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

// All or nothing for one table: every site is checked before any is written.
int install_table(const char* what, const Site* sites, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!matches(sites[i])) {
            LOG_ERROR("%s: the %s at 0x%X does not match this build. Not patching.", what, sites[i].name,
                      static_cast<unsigned>(sites[i].address));
            return 0;
        }
    }
    int done = 0;
    for (size_t i = 0; i < count; ++i) {
        if (write_jump(sites[i].address, sites[i].replacement)) {
            ++done;
        } else {
            LOG_ERROR("%s: could not write the jump for the %s", what, sites[i].name);
        }
    }
    return done;
}

}  // namespace

bool rendermath_install(bool core, bool effects) {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        LOG_ERROR("FastRenderMath needs the game at its preferred base. Not patching.");
        return false;
    }
    bool ok = true;
    if (core) {
        const int n = install_table("FastRenderMath", kCoreSites, sizeof(kCoreSites) / sizeof(kCoreSites[0]));
        ok = ok && n == static_cast<int>(sizeof(kCoreSites) / sizeof(kCoreSites[0]));
        if (n) {
            LOG_INFO("FastRenderMath: %d of the engine's vector helpers (four-float add, subtract, multiply, "
                     "multiply-add, 3x4 matrix add) run as SSE with the same results",
                     n);
        }
    }
    if (effects) {
        const int n = install_table("FastEffects", kFfxSites, sizeof(kFfxSites) / sizeof(kFfxSites[0]));
        ok = ok && n == static_cast<int>(sizeof(kFfxSites) / sizeof(kFfxSites[0]));
        if (n) {
            LOG_INFO("FastEffects: %d of the effects library's vector wrappers (set, splat, subtract) replaced with "
                     "direct code (same results)",
                     n);
        }
    }
    return ok;
}
