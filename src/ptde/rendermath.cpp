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

// cdecl, out returned in eax; clobber only eax, ecx, edx and xmm registers like the originals. Written in assembly so
// each operation keeps the original's operand order: when both operands of an operation are NaN, the first one's NaN
// is the result, and a compiler may swap the operands of an add or multiply intrinsic. Every original puts a first
// (a+b, a-b, a*b) and adds the product first in a*b+c.
__declspec(naked) float* __cdecl vec4_add(float*, const float*, const float*) {
    __asm {
        mov eax, [esp + 4]
        mov ecx, [esp + 8]
        mov edx, [esp + 12]
        movups xmm0, [ecx]
        movups xmm1, [edx]
        addps xmm0, xmm1
        movups [eax], xmm0
        ret
    }
}

__declspec(naked) float* __cdecl vec4_sub(float*, const float*, const float*) {
    __asm {
        mov eax, [esp + 4]
        mov ecx, [esp + 8]
        mov edx, [esp + 12]
        movups xmm0, [ecx]
        movups xmm1, [edx]
        subps xmm0, xmm1
        movups [eax], xmm0
        ret
    }
}

__declspec(naked) float* __cdecl vec4_mul(float*, const float*, const float*) {
    __asm {
        mov eax, [esp + 4]
        mov ecx, [esp + 8]
        mov edx, [esp + 12]
        movups xmm0, [ecx]
        movups xmm1, [edx]
        mulps xmm0, xmm1
        movups [eax], xmm0
        ret
    }
}

__declspec(naked) float* __cdecl vec4_madd(float*, const float*, const float*, const float*) {
    __asm {
        mov eax, [esp + 4]
        mov ecx, [esp + 8]
        mov edx, [esp + 12]
        movups xmm0, [ecx]      // a
        movups xmm1, [edx]      // b
        mov edx, [esp + 16]
        movups xmm4, [edx]      // c (read before anything is written, as the original)
        cvtps2pd xmm2, xmm0
        cvtps2pd xmm3, xmm1
        mulpd xmm2, xmm3        // a*b, lanes 0 1
        movhlps xmm0, xmm0
        movhlps xmm1, xmm1
        cvtps2pd xmm0, xmm0
        cvtps2pd xmm1, xmm1
        mulpd xmm0, xmm1        // a*b, lanes 2 3
        cvtps2pd xmm3, xmm4
        addpd xmm2, xmm3        // product + c
        movhlps xmm4, xmm4
        cvtps2pd xmm4, xmm4
        addpd xmm0, xmm4
        cvtpd2ps xmm2, xmm2
        cvtpd2ps xmm0, xmm0
        movlhps xmm2, xmm0
        movups [eax], xmm2
        ret
    }
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

// (eax = A, [esp+4] = B, callee pops B) -> al = 1 when any of the four lanes differs. The original compares each lane
// as doubles (ucomisd; a NaN lane counts as different, -0 equals +0), which cmpeqps does the same way. eax ends as 0 or
// 1 like the original's; ebx, esi and edi are kept.
__declspec(naked) void vec4_not_equal() {
    __asm {
        mov edx, [esp + 4]
        movups xmm0, [eax]
        movups xmm1, [edx]
        cmpeqps xmm0, xmm1
        movmskps ecx, xmm0
        xor eax, eax
        cmp ecx, 15
        setne al
        ret 4
    }
}

// 4x4 transpose (cdecl: out, in). The original reads and writes in this exact order (through stack temporaries it
// reloads 8 bytes at a time, which stalls), so a call with out overlapping in gives the same result as before.
uint32_t* __cdecl mat44_transpose(uint32_t* out, const uint32_t* in) {
    const uint32_t a8 = in[8], a0 = in[0], a12 = in[12], a9 = in[9], a1 = in[1], a5 = in[5], a13 = in[13], a4 = in[4],
                   a15 = in[15];
    out[0] = a0;
    out[1] = a4;
    out[4] = a1;
    out[5] = a5;
    out[6] = a9;
    out[7] = a13;
    const uint32_t a14 = in[14], a10 = in[10], a6 = in[6], a2 = in[2];
    out[8] = a2;
    out[9] = a6;
    out[10] = a10;
    out[11] = a14;
    const uint32_t a3 = in[3], a7 = in[7], a11 = in[11];
    out[12] = a3;
    out[13] = a7;
    out[2] = a8;
    out[3] = a12;
    out[14] = a11;
    out[15] = a15;
    return out;
}

// 0x407720 saturate (cdecl: out, in): each lane clamped to [0, 1] against the engine's constant rows at 0x10D5260
// (0s) and 0x10D5280 (1s): lo if lo > x, then hi if that > hi, compared as doubles. A NaN lane fails both compares
// and stays; -0 is not below 0 and stays -0. The same compares as masks.
float* __cdecl vec4_saturate(float* out, const float* in) {
    const __m128 x = _mm_loadu_ps(in);
    const __m128 lo = _mm_loadu_ps(reinterpret_cast<const float*>(0x10D5260));
    const __m128 hi = _mm_loadu_ps(reinterpret_cast<const float*>(0x10D5280));
    const __m128 below = _mm_cmplt_ps(x, lo);
    const __m128 r = _mm_or_ps(_mm_and_ps(below, lo), _mm_andnot_ps(below, x));
    const __m128 above = _mm_cmplt_ps(hi, r);
    _mm_storeu_ps(out, _mm_or_ps(_mm_and_ps(above, hi), _mm_andnot_ps(above, r)));
    return out;
}

// One row step of the engine's matrix multiplies, as the originals do it: every lane in double, rounded to float, the
// row stored before the next step reads it (and the next B row) back from memory. Operand order (which NaN comes out
// when both are NaN), measured on the originals: the first step multiplies b*s in every lane, the later steps b*s in
// lane 0 and s*b in lanes 1 to 3, and every add is product + row. cdecl (row, b row, s).
__declspec(naked) void __cdecl row_first(float*, const float*, float) {
    __asm {
        mov eax, [esp + 4]
        mov ecx, [esp + 8]
        cvtss2sd xmm2, dword ptr [esp + 12]
        unpcklpd xmm2, xmm2
        movups xmm0, [ecx]
        cvtps2pd xmm1, xmm0
        movhlps xmm0, xmm0
        cvtps2pd xmm0, xmm0
        mulpd xmm1, xmm2        // b*s
        mulpd xmm0, xmm2
        cvtpd2ps xmm1, xmm1
        cvtpd2ps xmm0, xmm0
        movlhps xmm1, xmm0
        movups [eax], xmm1
        ret
    }
}

__declspec(naked) void __cdecl row_madd(float*, const float*, float) {
    __asm {
        mov eax, [esp + 4]
        mov ecx, [esp + 8]
        cvtss2sd xmm2, dword ptr [esp + 12]
        unpcklpd xmm2, xmm2     // s s
        movups xmm0, [ecx]      // b row
        movups xmm3, [eax]      // the row so far
        cvtps2pd xmm1, xmm0     // b0 b1
        movhlps xmm0, xmm0
        cvtps2pd xmm0, xmm0     // b2 b3
        movapd xmm4, xmm1
        mulpd xmm4, xmm2        // b*s
        movapd xmm5, xmm2
        mulpd xmm5, xmm1        // s*b
        movsd xmm5, xmm4        // lane 0 b*s, lane 1 s*b
        movapd xmm6, xmm2
        mulpd xmm6, xmm0        // lanes 2 3: s*b
        cvtps2pd xmm7, xmm3
        addpd xmm5, xmm7        // product + row
        movhlps xmm3, xmm3
        cvtps2pd xmm3, xmm3
        addpd xmm6, xmm3
        cvtpd2ps xmm5, xmm5
        cvtpd2ps xmm6, xmm6
        movlhps xmm5, xmm6
        movups [eax], xmm5
        ret
    }
}

// out (4x4) = A (4x4) x B (4x4), cdecl (out, A, B). Each row's four factors are read before the row is written.
float* __cdecl mat44_mul(float* out, const float* a, const float* b) {
    for (int r = 0; r < 4; ++r) {
        const float a0 = a[r * 4 + 0], a1 = a[r * 4 + 1], a2 = a[r * 4 + 2], a3 = a[r * 4 + 3];
        float* o = out + r * 4;
        row_first(o, b, a0);
        row_madd(o, b + 4, a1);
        row_madd(o, b + 8, a2);
        row_madd(o, b + 12, a3);
    }
    return out;
}

// out (3x4) = A (3x4) x [B (3x4); the engine's constant row 0 0 0 1 at 0x10D52D0], cdecl (out, B, A).
float* __cdecl mat34_mul(float* out, const float* b, const float* a) {
    const float* w = reinterpret_cast<const float*>(0x10D52D0);
    for (int r = 0; r < 3; ++r) {
        const float a0 = a[r * 4 + 0], a1 = a[r * 4 + 1], a2 = a[r * 4 + 2], a3 = a[r * 4 + 3];
        float* o = out + r * 4;
        row_first(o, b, a0);
        row_madd(o, b + 4, a1);
        row_madd(o, b + 8, a2);
        row_madd(o, w, a3);
    }
    return out;
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

// Four 32-bit integers = each lane of v times scale, truncated toward zero as a 64-bit integer, low 32 bits kept. The
// originals (0x9B21A0 x1, 0x9B32A0 x65536, 0x9B3030 x256, 0x9B3510 x2^24: particle colours and coordinates) multiply
// through the engine's four-float multiply, then for every lane switch the x87 rounding to truncation, fistp a qword
// and switch it back. fisttp truncates without touching the control word and stores the same integer (NaN and
// overflow included: the integer indefinite); the product is a float multiply either way. cdecl (out, v).
uint32_t* ffx_trunc4(uint32_t* out, const float* v, float scale) {
    alignas(16) float p[4];
    _mm_store_ps(p, _mm_mul_ps(_mm_loadu_ps(v), _mm_set1_ps(scale)));
    alignas(8) uint32_t q[8];
    __asm {
        fld dword ptr [p]
        fisttp qword ptr [q]
        fld dword ptr [p + 4]
        fisttp qword ptr [q + 8]
        fld dword ptr [p + 8]
        fisttp qword ptr [q + 16]
        fld dword ptr [p + 12]
        fisttp qword ptr [q + 24]
    }
    out[0] = q[0];
    out[1] = q[2];
    out[2] = q[4];
    out[3] = q[6];
    return out;
}

// The effects library's forwarding wrappers: each calls one engine helper into a stack temporary, copies the result
// through two more temporaries into out and returns out (an aligned debug frame around it). Here the helper is called
// at the same address with the same arguments into one temporary, which is copied to out, so a call with out also an
// input behaves as before.
template <uintptr_t F>
uint32_t* __cdecl ffx_fwd1(uint32_t* out, uint32_t a) {
    alignas(16) uint32_t t[4];
    reinterpret_cast<void*(__cdecl*)(void*, uint32_t)>(F)(t, a);
    std::memcpy(out, t, 16);
    return out;
}
template <uintptr_t F>
uint32_t* __cdecl ffx_fwd2(uint32_t* out, uint32_t a, uint32_t b) {
    alignas(16) uint32_t t[4];
    reinterpret_cast<void*(__cdecl*)(void*, uint32_t, uint32_t)>(F)(t, a, b);
    std::memcpy(out, t, 16);
    return out;
}
template <uintptr_t F>
uint32_t* __cdecl ffx_fwd3(uint32_t* out, uint32_t a, uint32_t b, uint32_t c) {
    alignas(16) uint32_t t[4];
    reinterpret_cast<void*(__cdecl*)(void*, uint32_t, uint32_t, uint32_t)>(F)(t, a, b, c);
    std::memcpy(out, t, 16);
    return out;
}

// 0x983C40 (out, a, b, c): a + (b - a) * c through the subtract wrapper (0x984B60 -> 0x417420) and the multiply-add
// (0x553700: d*c + a).
uint32_t* __cdecl ffx_lerp(uint32_t* out, uint32_t a, uint32_t b, uint32_t c) {
    alignas(16) uint32_t d[4], t[4];
    reinterpret_cast<void*(__cdecl*)(void*, uint32_t, uint32_t)>(0x417420)(d, b, a);
    reinterpret_cast<void*(__cdecl*)(void*, const void*, uint32_t, uint32_t)>(0x553700)(t, d, c, a);
    std::memcpy(out, t, 16);
    return out;
}

// 0x9B4650, the effects parameter evaluator (thiscall: this = the parameter, stack out, a, b, c; pops 16 bytes): calls
// 0x9B46E0, which calls the evaluator for the parameter's type (the table at 0x112D618, indexed by the parameter's
// first word) as (temporary, a, this, b, c) and copies the 16 bytes it returns a pointer to, through two temporaries,
// into out. The same table call, one copy. __fastcall with an unused edx stands in for thiscall.
uint32_t* __fastcall ffx_param_eval(uint32_t* self, void*, uint32_t* out, uint32_t a, uint32_t b, uint32_t c) {
    using Eval = const uint32_t*(__cdecl*)(void*, uint32_t, void*, uint32_t, uint32_t);
    alignas(16) uint32_t t[4];
    const Eval fn = reinterpret_cast<const Eval*>(0x112D618)[*self];
    const uint32_t* r = fn(t, a, self, b, c);
    std::memcpy(out, r, 16);
    return out;
}

// 0x983A90 (out, unused, stream): reads the next float of a parameter stream (and moves the stream on by one) and
// splats it; the float goes through the x87 stack as in the original (which only quiets a signaling NaN).
uint32_t* __cdecl ffx_read_splat(uint32_t* out, uint32_t, const uint32_t** stream) {
    const uint32_t* p = *stream;
    uint32_t v = *p;
    *stream = p + 1;
    __asm {
        fld dword ptr [v]
        fstp dword ptr [v]
    }
    out[0] = out[1] = out[2] = out[3] = v;
    return out;
}

uint32_t* __cdecl ffx_to_int_1(uint32_t* out, const float* v) {
    return ffx_trunc4(out, v, 1.0f);
}
uint32_t* __cdecl ffx_to_int_65536(uint32_t* out, const float* v) {
    return ffx_trunc4(out, v, *reinterpret_cast<const float*>(0x112DA6C));
}
uint32_t* __cdecl ffx_to_int_256(uint32_t* out, const float* v) {
    return ffx_trunc4(out, v, *reinterpret_cast<const float*>(0x11E8610));
}
uint32_t* __cdecl ffx_to_int_2p24(uint32_t* out, const float* v) {
    return ffx_trunc4(out, v, *reinterpret_cast<const float*>(0x112DA70));
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
const uint8_t kTransposeHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x10, 0x8B, 0x4D, 0x0C,
                                  0x8B, 0x45, 0x08, 0xF3, 0x0F, 0x10, 0x51, 0x20, 0xF3, 0x0F, 0x10, 0x19};
const uint8_t kNotEqualHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x51, 0x53, 0x8B, 0x5D, 0x08, 0xF3,
                                 0x0F, 0x10, 0x03, 0x56, 0x8B, 0xF0, 0xF3, 0x0F, 0x10, 0x0E, 0x0F, 0x5A};
const uint8_t kMat44MulHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x30, 0x8B, 0x4D, 0x10,
                                 0x8B, 0x55, 0x0C, 0xF3, 0x0F, 0x10, 0x4A, 0x08, 0xF3, 0x0F, 0x10, 0x52};
const uint8_t kMat34MulHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x30, 0x8B, 0x4D, 0x0C,
                                 0x8B, 0x55, 0x10, 0xF3, 0x0F, 0x10, 0x4A, 0x08, 0xF3, 0x0F, 0x10, 0x52};

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
// The float-to-integer converters' prologue (a 0x110-byte frame) and what follows their fill (at +0x2F: the scale).
const uint8_t kFfxHead110[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04,
                               0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC,
                               0x10, 0x01, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0xF0, 0xFE, 0xFF, 0xFF};
const uint8_t kToInt1Body[] = {0x51, 0xD9, 0xE8, 0xD9, 0x1C, 0x24, 0x8D, 0x85, 0x40, 0xFF, 0xFF, 0xFF, 0x50, 0xE8, 0x3F, 0x27};
const uint8_t kToInt65536Body[] = {0x51, 0xD9, 0x05, 0x6C, 0xDA, 0x12, 0x01, 0xD9, 0x1C, 0x24, 0x8D, 0x85, 0x40, 0xFF, 0xFF, 0xFF};
const uint8_t kToInt256Body[] = {0x51, 0xD9, 0x05, 0x10, 0x86, 0x1E, 0x01, 0xD9, 0x1C, 0x24, 0x8D, 0x85, 0x40, 0xFF, 0xFF, 0xFF};
const uint8_t kToInt2p24Body[] = {0x51, 0xD9, 0x05, 0x70, 0xDA, 0x12, 0x01, 0xD9, 0x1C, 0x24, 0x8D, 0x85, 0x40, 0xFF, 0xFF, 0xFF};
// Forwarding wrappers: the prologue through lea edi (0x50- and 0x70-byte frames), and the forwarded call after the fill
// (at +0x29), including its target.
const uint8_t kFfxHead50Lea[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B,
                                 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x83, 0xEC, 0x50, 0x57, 0x8D, 0x7D, 0xB0};
const uint8_t kFfxHead70Lea[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B,
                                 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x83, 0xEC, 0x70, 0x57, 0x8D, 0x7D, 0x90};
const uint8_t kSelectFwd[] = {0x8B, 0x43, 0x14, 0x50, 0x8B, 0x4B, 0x10, 0x51, 0x8B, 0x53, 0x0C, 0x52, 0x8D, 0x45, 0xB0, 0x50, 0xE8, 0x12, 0xE5, 0xA7};
const uint8_t k419490Fwd[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x36, 0x43, 0xA9, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t k985020Fwd[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x86, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x0C};
const uint8_t k407720Fwd[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0xAA, 0x0A, 0xA5, 0xFF, 0x83, 0xC4, 0x08};
const uint8_t kAddFwd[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x06, 0x1F, 0xA5, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t k430AC0Fwd[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0xEA, 0xBB, 0xAA, 0xFF, 0x83, 0xC4, 0x08};
const uint8_t k4193E0Fwd[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0xAA, 0x3E, 0xA9, 0xFF, 0x83, 0xC4, 0x08};
const uint8_t kSaturateHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x0C, 0xF3, 0x0F, 0x10,
                                 0x00, 0xF3, 0x0F, 0x10, 0x0D, 0x60, 0x52, 0x0D, 0x01, 0x83, 0xEC, 0x10};
const uint8_t kFfxHead30Ecx[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B,
                                 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x83, 0xEC, 0x30, 0x57, 0x51, 0x8D, 0x7D, 0xD0};
const uint8_t kParamEvalBody[] = {0x59, 0x89, 0x4D, 0xFC, 0x8B, 0x43, 0x14, 0x50, 0x8B, 0x4B, 0x10, 0x51, 0x8B, 0x55, 0xFC, 0x52, 0x8B, 0x43, 0x0C, 0x50};
const uint8_t kFfxHead30Lea[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B,
                                 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x83, 0xEC, 0x30, 0x57, 0x8D, 0x7D, 0xD0};
const uint8_t kReadSplatBody[] = {0x8B, 0x43, 0x10, 0x8B, 0x08, 0x89, 0x4D, 0xFC, 0x8B, 0x55, 0xFC, 0xD9, 0x02, 0xD9, 0x5D, 0xF8, 0x8B, 0x45, 0xFC, 0x83};
const uint8_t kLerpFwd[] = {0x8B, 0x43, 0x0C, 0x50, 0x8B, 0x4B, 0x10, 0x51, 0x8D, 0x55, 0xA0, 0x52, 0xE8, 0xE6, 0x0E, 0x00, 0x00, 0x83, 0xC4, 0x0C};

const Site kCoreSites[] = {
    {"four-float add", 0x4078D0, kAddHead, sizeof(kAddHead), nullptr, 0, 0, reinterpret_cast<const void*>(&vec4_add)},
    {"four-float subtract", 0x417420, kSubHead, sizeof(kSubHead), nullptr, 0, 0, reinterpret_cast<const void*>(&vec4_sub)},
    {"four-float multiply", 0x5537E0, kMulHead, sizeof(kMulHead), nullptr, 0, 0, reinterpret_cast<const void*>(&vec4_mul)},
    {"four-float multiply-add", 0x553700, kMaddHead, sizeof(kMaddHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&vec4_madd)},
    {"3x4 matrix add", 0x421E70, kMat34Head, sizeof(kMat34Head), nullptr, 0, 0, reinterpret_cast<const void*>(&mat34_add)},
    {"4x4 transpose", 0x423220, kTransposeHead, sizeof(kTransposeHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&mat44_transpose)},
    {"four-float not-equal", 0x407F50, kNotEqualHead, sizeof(kNotEqualHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&vec4_not_equal)},
    {"4x4 matrix multiply", 0x4144D0, kMat44MulHead, sizeof(kMat44MulHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&mat44_mul)},
    {"3x4 matrix multiply", 0x416100, kMat34MulHead, sizeof(kMat34MulHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&mat34_mul)},
    {"four-float saturate", 0x407720, kSaturateHead, sizeof(kSaturateHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&vec4_saturate)},
};

// More forwarding wrappers (found 2026-11-13 by scanning the library): each one's bytes from the argument pushes to its
// end equal those of a wrapper above with as many arguments, apart from the helper it calls.
const uint8_t kFwd984C20[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x86, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x0C};
const uint8_t kFwd9851E0[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x86, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x0C};
const uint8_t kFwd985370[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x86, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x0C};
const uint8_t kFwd9B5A50[] = {0x8B, 0x43, 0x14, 0x50, 0x8B, 0x4B, 0x10, 0x51, 0x8B, 0x53, 0x0C, 0x52, 0x8D, 0x45, 0xB0, 0x50, 0xE8, 0x72, 0x07, 0xA7, 0xFF, 0x83, 0xC4, 0x10};
const uint8_t kFwd9B5C20[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0x2A, 0xB0, 0xA7, 0xFF, 0x83, 0xC4, 0x08};
const uint8_t kFwd9B5CE0[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0x7A, 0x3B, 0xA6, 0xFF, 0x83, 0xC4, 0x08};
const uint8_t kFwd9B66B0[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x36, 0x05, 0xA6, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t kFwd9B6770[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x96, 0xE6, 0xA7, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t kFwd9B6830[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x86, 0x0C, 0xA5, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t kFwd9B68F0[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x56, 0x0C, 0xA5, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t kFwd9B69B0[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0x8A, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x08};
const uint8_t kFwd9B6B80[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0xFA, 0x0D, 0xA5, 0xFF, 0x83, 0xC4, 0x08};
const uint8_t kFwd9B6D00[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0x6A, 0x59, 0xA9, 0xFF, 0x83, 0xC4, 0x08};
const uint8_t kFwdA51E80[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0xD6, 0xC3, 0x9F, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t kFwdA51F40[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x46, 0x30, 0x9E, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t kFwdA52000[] = {0x8B, 0x43, 0x10, 0x50, 0x8B, 0x4B, 0x0C, 0x51, 0x8D, 0x55, 0xB0, 0x52, 0xE8, 0x46, 0x4B, 0x9C, 0xFF, 0x83, 0xC4, 0x0C};
const uint8_t kFwdA520C0[] = {0x8B, 0x43, 0x0C, 0x50, 0x8D, 0x4D, 0xB0, 0x51, 0xE8, 0xBA, 0x08, 0x9C, 0xFF, 0x83, 0xC4, 0x08};

const Site kFfxSites[] = {
    {"set", 0x984890, kFfxHead30, sizeof(kFfxHead30), kSet4Body, 0x29, sizeof(kSet4Body),
     reinterpret_cast<const void*>(&ffx_set4)},
    {"splat", 0x9847C0, kFfxHead50, sizeof(kFfxHead50), kSplatBody, 0x29, sizeof(kSplatBody),
     reinterpret_cast<const void*>(&ffx_splat)},
    {"splat (x87)", 0x984920, kFfxHead50, sizeof(kFfxHead50), kSplatX87Body, 0x29, sizeof(kSplatX87Body),
     reinterpret_cast<const void*>(&ffx_splat_x87)},
    {"subtract", 0x984B60, kFfxHead50, sizeof(kFfxHead50), kSubWrapBody, 0x29, sizeof(kSubWrapBody),
     reinterpret_cast<const void*>(&ffx_sub)},
    {"to-integer x1", 0x9B21A0, kFfxHead110, sizeof(kFfxHead110), kToInt1Body, 0x2F, sizeof(kToInt1Body),
     reinterpret_cast<const void*>(&ffx_to_int_1)},
    {"to-integer x65536", 0x9B32A0, kFfxHead110, sizeof(kFfxHead110), kToInt65536Body, 0x2F, sizeof(kToInt65536Body),
     reinterpret_cast<const void*>(&ffx_to_int_65536)},
    {"to-integer x256", 0x9B3030, kFfxHead110, sizeof(kFfxHead110), kToInt256Body, 0x2F, sizeof(kToInt256Body),
     reinterpret_cast<const void*>(&ffx_to_int_256)},
    {"to-integer x2^24", 0x9B3510, kFfxHead110, sizeof(kFfxHead110), kToInt2p24Body, 0x2F, sizeof(kToInt2p24Body),
     reinterpret_cast<const void*>(&ffx_to_int_2p24)},
    {"select wrapper", 0x9855C0, kFfxHead50Lea, sizeof(kFfxHead50Lea), kSelectFwd, 0x29, sizeof(kSelectFwd),
     reinterpret_cast<const void*>(&ffx_fwd3<0x403B10>)},
    {"0x419490 wrapper", 0x985120, kFfxHead50Lea, sizeof(kFfxHead50Lea), k419490Fwd, 0x29, sizeof(k419490Fwd),
     reinterpret_cast<const void*>(&ffx_fwd2<0x419490>)},
    {"0x985020 wrapper", 0x984F60, kFfxHead50Lea, sizeof(kFfxHead50Lea), k985020Fwd, 0x29, sizeof(k985020Fwd),
     reinterpret_cast<const void*>(&ffx_fwd2<0x985020>)},
    {"0x407720 wrapper", 0x9B6C40, kFfxHead50Lea, sizeof(kFfxHead50Lea), k407720Fwd, 0x29, sizeof(k407720Fwd),
     reinterpret_cast<const void*>(&ffx_fwd1<0x407720>)},
    {"add wrapper", 0x9B5990, kFfxHead50Lea, sizeof(kFfxHead50Lea), kAddFwd, 0x29, sizeof(kAddFwd),
     reinterpret_cast<const void*>(&ffx_fwd2<0x4078D0>)},
    {"0x430AC0 wrapper", 0x984EA0, kFfxHead50Lea, sizeof(kFfxHead50Lea), k430AC0Fwd, 0x29, sizeof(k430AC0Fwd),
     reinterpret_cast<const void*>(&ffx_fwd1<0x430AC0>)},
    {"0x4193E0 wrapper", 0x985500, kFfxHead50Lea, sizeof(kFfxHead50Lea), k4193E0Fwd, 0x29, sizeof(k4193E0Fwd),
     reinterpret_cast<const void*>(&ffx_fwd1<0x4193E0>)},
    {"lerp", 0x983C40, kFfxHead70Lea, sizeof(kFfxHead70Lea), kLerpFwd, 0x29, sizeof(kLerpFwd),
     reinterpret_cast<const void*>(&ffx_lerp)},
    {"parameter evaluator", 0x9B4650, kFfxHead30Ecx, sizeof(kFfxHead30Ecx), kParamEvalBody, 0x2A,
     sizeof(kParamEvalBody), reinterpret_cast<const void*>(&ffx_param_eval)},
    {"stream read-splat", 0x983A90, kFfxHead30Lea, sizeof(kFfxHead30Lea), kReadSplatBody, 0x29, sizeof(kReadSplatBody),
     reinterpret_cast<const void*>(&ffx_read_splat)},
    {"0x984CE0 wrapper", 0x984C20, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd984C20, 0x29, sizeof(kFwd984C20),
     reinterpret_cast<const void*>(&ffx_fwd2<0x984CE0>)},
    {"0x9852A0 wrapper", 0x9851E0, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9851E0, 0x29, sizeof(kFwd9851E0),
     reinterpret_cast<const void*>(&ffx_fwd2<0x9852A0>)},
    {"0x985430 wrapper", 0x985370, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd985370, 0x29, sizeof(kFwd985370),
     reinterpret_cast<const void*>(&ffx_fwd2<0x985430>)},
    {"0x426200 wrapper", 0x9B5A50, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B5A50, 0x29, sizeof(kFwd9B5A50),
     reinterpret_cast<const void*>(&ffx_fwd3<0x426200>)},
    {"0x430C80 wrapper", 0x9B5C20, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B5C20, 0x29, sizeof(kFwd9B5C20),
     reinterpret_cast<const void*>(&ffx_fwd1<0x430C80>)},
    {"0x419890 wrapper", 0x9B5CE0, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B5CE0, 0x29, sizeof(kFwd9B5CE0),
     reinterpret_cast<const void*>(&ffx_fwd1<0x419890>)},
    {"0x416C20 wrapper", 0x9B66B0, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B66B0, 0x29, sizeof(kFwd9B66B0),
     reinterpret_cast<const void*>(&ffx_fwd2<0x416C20>)},
    {"0x434E40 wrapper", 0x9B6770, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B6770, 0x29, sizeof(kFwd9B6770),
     reinterpret_cast<const void*>(&ffx_fwd2<0x434E40>)},
    {"0x4074F0 wrapper", 0x9B6830, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B6830, 0x29, sizeof(kFwd9B6830),
     reinterpret_cast<const void*>(&ffx_fwd2<0x4074F0>)},
    {"0x407580 wrapper", 0x9B68F0, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B68F0, 0x29, sizeof(kFwd9B68F0),
     reinterpret_cast<const void*>(&ffx_fwd2<0x407580>)},
    {"0x9B6A70 wrapper", 0x9B69B0, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B69B0, 0x29, sizeof(kFwd9B69B0),
     reinterpret_cast<const void*>(&ffx_fwd1<0x9B6A70>)},
    {"0x4079B0 wrapper", 0x9B6B80, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B6B80, 0x29, sizeof(kFwd9B6B80),
     reinterpret_cast<const void*>(&ffx_fwd1<0x4079B0>)},
    {"0x44C6A0 wrapper", 0x9B6D00, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwd9B6D00, 0x29, sizeof(kFwd9B6D00),
     reinterpret_cast<const void*>(&ffx_fwd1<0x44C6A0>)},
    {"0x44E290 wrapper", 0xA51E80, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwdA51E80, 0x29, sizeof(kFwdA51E80),
     reinterpret_cast<const void*>(&ffx_fwd2<0x44E290>)},
    {"0x434FC0 wrapper", 0xA51F40, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwdA51F40, 0x29, sizeof(kFwdA51F40),
     reinterpret_cast<const void*>(&ffx_fwd2<0x434FC0>)},
    {"0x416B80 wrapper", 0xA52000, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwdA52000, 0x29, sizeof(kFwdA52000),
     reinterpret_cast<const void*>(&ffx_fwd2<0x416B80>)},
    {"0x4129B0 wrapper", 0xA520C0, kFfxHead50Lea, sizeof(kFfxHead50Lea), kFwdA520C0, 0x29, sizeof(kFwdA520C0),
     reinterpret_cast<const void*>(&ffx_fwd1<0x4129B0>)},
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


// ---------------------------------------------------------------------------------------------------------------
// View culling: 0x593E40 (cdecl: planes = 6 x (a, b, c, d), box = two corners of 4 floats, the 4th ignored) gives
// 0 when the box is fully on the positive side of a plane, 1 when it straddles a plane, 2 when inside all six. Per
// plane, per axis, the sign of the coefficient (>= 0, or NaN) picks the first corner's value for the near point and
// the second's for the far point (the other way round when negative); near = float(((y*b + x*a) + z*c) + d) in x87
// arithmetic (each step rounded to the thread's precision, the sum stored as a float). near > 0 (or NaN): outside, no
// further planes. far > 0 (or NaN): straddles. The original runs it one plane at a time with x87 compares through
// fnstsw and integer masks; here four planes at a time in SSE.
// The render thread (the only caller) runs x87 at 24-bit precision (control word 0x007F, set by Direct3D). Each x87
// step then rounds to a 24-bit significand like an SSE single, but with x87's wide exponent: the results are the same
// whenever every intermediate stays a normal float. With every input zero or of magnitude 2^-40..2^40, products lie
// in 2^-80..2^80 and every nonzero sum is a multiple of 2^-103: never denormal, never overflowing. Anything else (another
// precision or rounding mode, NaN, inf, huge or tiny values) runs the original code.
using CullFn = int(__cdecl*)(const float*, const uint32_t*);
CullFn g_cull_original = reinterpret_cast<CullFn>(0x593E40);  // the trampoline once installed

inline __m128i cull_in_range(__m128 v) {
    const __m128i a = _mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF));
    // |v| in [2^-40, 2^40): biased exponent 87..166, i.e. bits 0x2B800000 <= a < 0x53800000; or zero.
    const __m128i lo = _mm_cmpgt_epi32(a, _mm_set1_epi32(0x2B800000 - 1));
    const __m128i hi = _mm_cmplt_epi32(a, _mm_set1_epi32(0x53800000));
    const __m128i zero = _mm_cmpeq_epi32(a, _mm_setzero_si128());
    return _mm_or_si128(_mm_and_si128(lo, hi), zero);
}

int __cdecl cull_box(const float* planes, const uint32_t* box) {
    unsigned short cw;
    __asm fnstcw cw
    // 24-bit precision (bits 8-9 = 00) and round to nearest (bits 10-11 = 00).
    if (cw & 0x0F00) return g_cull_original(planes, box);
    const __m128 p0 = _mm_loadu_ps(planes), p1 = _mm_loadu_ps(planes + 4), p2 = _mm_loadu_ps(planes + 8),
                 p3 = _mm_loadu_ps(planes + 12), p4 = _mm_loadu_ps(planes + 16), p5 = _mm_loadu_ps(planes + 20);
    const __m128 c0 = _mm_loadu_ps(reinterpret_cast<const float*>(box));
    const __m128 c1 = _mm_loadu_ps(reinterpret_cast<const float*>(box + 4));
    // Range check: the 24 plane floats and the corners' x, y, z (w becomes 1.0).
    const __m128 c0xyz = _mm_and_ps(c0, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
    const __m128 c1xyz = _mm_and_ps(c1, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
    __m128i ok = _mm_and_si128(cull_in_range(p0), cull_in_range(p1));
    ok = _mm_and_si128(ok, _mm_and_si128(cull_in_range(p2), cull_in_range(p3)));
    ok = _mm_and_si128(ok, _mm_and_si128(cull_in_range(p4), cull_in_range(p5)));
    ok = _mm_and_si128(ok, _mm_and_si128(cull_in_range(c0xyz), cull_in_range(c1xyz)));
    if (_mm_movemask_epi8(ok) != 0xFFFF) return g_cull_original(planes, box);

    const __m128 zero = _mm_setzero_ps();
    const __m128 ax = _mm_shuffle_ps(c0, c0, 0x00), ay = _mm_shuffle_ps(c0, c0, 0x55), az = _mm_shuffle_ps(c0, c0, 0xAA);
    const __m128 bx = _mm_shuffle_ps(c1, c1, 0x00), by = _mm_shuffle_ps(c1, c1, 0x55), bz = _mm_shuffle_ps(c1, c1, 0xAA);
    int outside = 0, straddle = 0;
    auto group = [&](__m128 q0, __m128 q1, __m128 q2, __m128 q3, int lanes) {
        _MM_TRANSPOSE4_PS(q0, q1, q2, q3);  // q0 = a, q1 = b, q2 = c, q3 = d of four planes
        const __m128 ma = _mm_cmpge_ps(q0, zero), mb = _mm_cmpge_ps(q1, zero), mc = _mm_cmpge_ps(q2, zero);
        const __m128 nx = _mm_or_ps(_mm_and_ps(ma, ax), _mm_andnot_ps(ma, bx));
        const __m128 ny = _mm_or_ps(_mm_and_ps(mb, ay), _mm_andnot_ps(mb, by));
        const __m128 nz = _mm_or_ps(_mm_and_ps(mc, az), _mm_andnot_ps(mc, bz));
        const __m128 fx = _mm_or_ps(_mm_and_ps(ma, bx), _mm_andnot_ps(ma, ax));
        const __m128 fy = _mm_or_ps(_mm_and_ps(mb, by), _mm_andnot_ps(mb, ay));
        const __m128 fz = _mm_or_ps(_mm_and_ps(mc, bz), _mm_andnot_ps(mc, az));
        const __m128 near_ = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(ny, q1), _mm_mul_ps(nx, q0)), _mm_mul_ps(nz, q2)), q3);
        const __m128 far_ = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(fy, q1), _mm_mul_ps(fx, q0)), _mm_mul_ps(fz, q2)), q3);
        outside |= _mm_movemask_ps(_mm_cmpgt_ps(near_, zero)) & lanes;
        straddle |= _mm_movemask_ps(_mm_cmpgt_ps(far_, zero)) & lanes;
    };
    group(p0, p1, p2, p3, 0xF);
    group(p4, p5, p4, p5, 0x3);
    if (outside) return 0;
    return straddle ? 1 : 2;
}

// The fallback: the original's first six bytes (push ebp; mov ebp, esp; and esp, -16), then a jump past them.
const uint8_t kCullHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xF4, 0x00, 0x00, 0x00, 0x53, 0x56, 0x57,
                             0x8B, 0x0D, 0x60, 0x52, 0x0D, 0x01, 0x8B, 0x15, 0x64, 0x52, 0x0D, 0x01};
uint8_t g_cull_off[8];  // the original's first 8 bytes (for rendermath_set_cull)
bool g_cull_installed = false;

bool install_cull() {
    if (std::memcmp(reinterpret_cast<const void*>(0x593E40), kCullHead, sizeof(kCullHead)) != 0) return false;
    // Its constants: zeros at 0x10D5260, ones at 0x10D5280, the xyz mask at 0x10D54F0.
    const uint32_t* zeros = reinterpret_cast<const uint32_t*>(0x10D5260);
    const uint32_t* ones = reinterpret_cast<const uint32_t*>(0x10D5280);
    const uint32_t* mask = reinterpret_cast<const uint32_t*>(0x10D54F0);
    for (int i = 0; i < 4; ++i) {
        if (zeros[i] != 0 || ones[i] != 0x3F800000u || mask[i] != (i < 3 ? 0xFFFFFFFFu : 0u)) return false;
    }
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    std::memcpy(t, kCullHead, 6);
    t[6] = 0xE9;
    const auto rel = static_cast<int32_t>(0x593E46 - (reinterpret_cast<uintptr_t>(t) + 11));
    std::memcpy(t + 7, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    g_cull_original = reinterpret_cast<CullFn>(t);
    std::memcpy(g_cull_off, reinterpret_cast<const void*>(0x593E40), 8);
    if (!write_jump(0x593E40, reinterpret_cast<const void*>(&cull_box))) {
        g_cull_original = reinterpret_cast<CullFn>(0x593E40);
        return false;
    }
    return true;
}

// The original first 8 bytes of every site written (for rendermath_set: an A/B in one session).
struct Saved {
    uintptr_t at;
    uint8_t orig[8];
    const void* target;
    int table;  // 0 core, 1 effects
    int index;  // in its table
};
Saved g_saved[96];
int g_saved_n = 0;

// All or nothing for one table: every site is checked before any is written.
int install_table(const char* what, const Site* sites, size_t count, int table) {
    for (size_t i = 0; i < count; ++i) {
        if (!matches(sites[i])) {
            LOG_ERROR("%s: the %s at 0x%X does not match this build. Not patching.", what, sites[i].name,
                      static_cast<unsigned>(sites[i].address));
            return 0;
        }
    }
    int done = 0;
    for (size_t i = 0; i < count; ++i) {
        if (g_saved_n < 96 && (sites[i].address & 7) == 0) {
            g_saved[g_saved_n].at = sites[i].address;
            std::memcpy(g_saved[g_saved_n].orig, reinterpret_cast<const void*>(sites[i].address), 8);
            g_saved[g_saved_n].target = sites[i].replacement;
            g_saved[g_saved_n].table = table;
            g_saved[g_saved_n].index = static_cast<int>(i);
            ++g_saved_n;
        }
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
        const int n = install_table("FastRenderMath", kCoreSites, sizeof(kCoreSites) / sizeof(kCoreSites[0]), 0);
        ok = ok && n == static_cast<int>(sizeof(kCoreSites) / sizeof(kCoreSites[0]));
        if (n) {
            LOG_INFO("FastRenderMath: %d of the engine's vector and matrix helpers (four-float add, subtract, multiply, "
                     "multiply-add, not-equal and saturate, 3x4 matrix add and multiply, 4x4 multiply and transpose) run as "
                     "SSE with the same results",
                     n);
        }
        if (install_cull()) {
            g_cull_installed = true;
            LOG_INFO("FastRenderMath: the view culling test (bounding box against the six view planes, 0x593E40) runs "
                     "four planes at a time in SSE with the same results (the original code for unusual inputs)");
        } else {
            LOG_ERROR("FastRenderMath: the view culling test at 0x593E40 does not match this build. Left as it is.");
            ok = false;
        }
    }
    if (effects) {
        const int n = install_table("FastEffects", kFfxSites, sizeof(kFfxSites) / sizeof(kFfxSites[0]), 1);
        ok = ok && n == static_cast<int>(sizeof(kFfxSites) / sizeof(kFfxSites[0]));
        if (n) {
            LOG_INFO("FastEffects: %d of the effects library's vector helpers (set, splat, subtract, float-to-integer, lerp, parameter evaluation, stream reads and forwarding wrappers) replaced "
                     "with direct code (same results)",
                     n);
        }
    }
    return ok;
}

// RemoteControl `cull 0|1`: the view culling replacement off (the original code) or on again, for an A/B in one session.
// The first 8 bytes (16-aligned) are swapped in one atomic write, so the render thread sees either version whole.
bool rendermath_set_cull(bool on) {
    if (!g_cull_installed) return false;
    uint64_t now = 0;
    std::memcpy(&now, reinterpret_cast<const void*>(0x593E40), 8);
    uint64_t want = 0;
    if (on) {
        uint8_t b[8];
        std::memcpy(b, &now, 8);
        b[0] = 0xE9;
        const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&cull_box) - (0x593E40 + 5));
        std::memcpy(b + 1, &rel, 4);
        std::memcpy(b + 5, g_cull_off + 5, 3);
        std::memcpy(&want, b, 8);
    } else {
        std::memcpy(&want, g_cull_off, 8);
    }
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(0x593E40), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(0x593E40), static_cast<LONG64>(want),
                                 static_cast<LONG64>(now));
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(0x593E40), 8, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(0x593E40), 8);
    return true;
}

// RemoteControl `fastmath core|effects 0|1`: every 8-aligned site of that table back to the original code or on again,
// each in one atomic 8-byte write (diagnostic A/B; a thread inside a site's first instructions at that moment would be
// cut, so this is for testing only). Returns how many sites were switched.
int rendermath_set_table(int table, bool on) {
    int n = 0;
    for (int i = 0; i < g_saved_n; ++i) {
        const Saved& sv = g_saved[i];
        // table 2: the effects wrappers added 2026-11-13 (index 18 on)
        if (table == 2 ? (sv.table != 1 || sv.index < 18) : sv.table != table) continue;
        uint8_t b[8];
        std::memcpy(b, sv.orig, 8);
        if (on) {
            b[0] = 0xE9;
            const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(sv.target) - (sv.at + 5));
            std::memcpy(b + 1, &rel, 4);
        }
        uint64_t want = 0, now = 0;
        std::memcpy(&want, b, 8);
        std::memcpy(&now, reinterpret_cast<const void*>(sv.at), 8);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(sv.at), 8, PAGE_EXECUTE_READWRITE, &old)) continue;
        InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(sv.at), static_cast<LONG64>(want),
                                     static_cast<LONG64>(now));
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(sv.at), 8, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(sv.at), 8);
        ++n;
    }
    return n;
}
