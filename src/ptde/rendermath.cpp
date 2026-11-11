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
    {"to-integer x1", 0x9B21A0, kFfxHead110, sizeof(kFfxHead110), kToInt1Body, 0x2F, sizeof(kToInt1Body),
     reinterpret_cast<const void*>(&ffx_to_int_1)},
    {"to-integer x65536", 0x9B32A0, kFfxHead110, sizeof(kFfxHead110), kToInt65536Body, 0x2F, sizeof(kToInt65536Body),
     reinterpret_cast<const void*>(&ffx_to_int_65536)},
    {"to-integer x256", 0x9B3030, kFfxHead110, sizeof(kFfxHead110), kToInt256Body, 0x2F, sizeof(kToInt256Body),
     reinterpret_cast<const void*>(&ffx_to_int_256)},
    {"to-integer x2^24", 0x9B3510, kFfxHead110, sizeof(kFfxHead110), kToInt2p24Body, 0x2F, sizeof(kToInt2p24Body),
     reinterpret_cast<const void*>(&ffx_to_int_2p24)},
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
            LOG_INFO("FastRenderMath: %d of the engine's vector and matrix helpers (four-float add, subtract, multiply, "
                     "multiply-add and not-equal, 3x4 matrix add and multiply, 4x4 multiply and transpose) run as "
                     "SSE with the same results",
                     n);
        }
    }
    if (effects) {
        const int n = install_table("FastEffects", kFfxSites, sizeof(kFfxSites) / sizeof(kFfxSites[0]));
        ok = ok && n == static_cast<int>(sizeof(kFfxSites) / sizeof(kFfxSites[0]));
        if (n) {
            LOG_INFO("FastEffects: %d of the effects library's vector helpers (set, splat, subtract, float-to-integer) replaced "
                     "with direct code (same results)",
                     n);
        }
    }
    return ok;
}
