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

// 0x419890 reciprocal square root (cdecl: out, in), the engine's scalar SSE loop: per lane +inf for +-0, 0x7FFFFFFF
// for x < 0 (also -inf), +0 for +inf, otherwise (float)(1.0 / (double)(float)sqrt((double)x)); a NaN lane goes the
// last way and comes out quieted. A float sqrt or quotient rounded through double first rounds the same as directly
// (53 >= 2 * 24 + 2), so sqrtps and divps give the same bits; zero and negative lanes take 1.0 into them instead, so no
// divide-by-zero or invalid flag is raised that the original's branches would not raise. All lanes are read before out
// is written, as there (its results go to a temporary first).
float* __cdecl vec4_rsqrt(float* out, const float* in) {
    const __m128 x = _mm_loadu_ps(in);
    const __m128 zero = _mm_setzero_ps();
    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 is0 = _mm_cmpeq_ps(x, zero);
    const __m128 neg = _mm_cmplt_ps(x, zero);
    const __m128 skip = _mm_or_ps(is0, neg);
    const __m128 xs = _mm_or_ps(_mm_andnot_ps(skip, x), _mm_and_ps(skip, one));
    const __m128 r = _mm_div_ps(one, _mm_sqrt_ps(xs));
    const __m128 special = _mm_or_ps(_mm_and_ps(is0, _mm_castsi128_ps(_mm_set1_epi32(0x7F800000))),
                                     _mm_and_ps(neg, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))));
    _mm_storeu_ps(out, _mm_or_ps(_mm_andnot_ps(skip, r), special));
    return out;
}

// 0x41FB20, 0x422110, 0x4221B0: set 3x4 matrix `index` of a shader-constant block if it changed (bone palettes and
// the like). eax = index, [esp+4] = block, [esp+8] = matrix, callee pops both. The block's rows at
// block + (index*3+K2)*16 are compared with the matrix's row by row through the four-float not-equal (0x407F50: as
// doubles, a NaN lane differs, -0 equals +0), stopping at the first that differs; if one does, the 48 bytes are copied 8
// at a time in order and, when block+0x10 is set, a setter (0xF652B0, 0xF65640, 0xF655F0; callee pops, the index in edi)
// passes them on to the device's shader constants: it marks registers index*3+K1 .. +2 dirty through 0x425670 (bit
// 0x80000000 >> (r & 31) of word r >> 5 at [block+0x10] + 0x10; that function shifts through the runtime's 64-bit shift
// helper) and copies the 48 bytes there with memcpy, to [block+0x10] + 0x50 + (index*3+K1)*16, returning memcpy's
// destination. K2/K1 are 6/8 for the first two (vertex shader registers c8 on) and 0x78/0x80 for the third (c128 on).
// A skinned model sets every bone of every draw this way: about 20,000 per frame where many animated models are in view
// (Quelaag's Domain). Here the compares as cmpeqps on the same rows in the same order, the copy as before, and the
// setter's three bits and 48 bytes inline, with no calls. eax comes back as the original's (some callers return it): 0
// when nothing changed, matrix + 32 when copied without a setter, the destination (memcpy's return) after one. ecx, ebx,
// esi, edi and ebp are kept like the original; edx is scratch, as are xmm0 and xmm1. Each entry pushes ecx and loads
// K2 | K1 << 16 into it for the shared body.
__declspec(naked) void mat_set_core() {
    __asm {
        // [esp] = the caller's ecx, [esp+4] = return, [esp+8] = block, [esp+12] = matrix; eax = index, ecx = K2 | K1 << 16
        push esi
        push edi
        push ebx
        mov edi, eax
        mov ebx, ecx
        mov edx, [esp + 20]
        mov ecx, [esp + 24]
        lea esi, [edi + edi * 2]
        movzx eax, bx
        add eax, esi
        shl eax, 4
        lea esi, [eax + edx]
        movups xmm0, [esi]
        movups xmm1, [ecx]
        cmpeqps xmm0, xmm1
        movmskps eax, xmm0
        cmp eax, 15
        jne copy
        movups xmm0, [esi + 16]
        movups xmm1, [ecx + 16]
        cmpeqps xmm0, xmm1
        movmskps eax, xmm0
        cmp eax, 15
        jne copy
        movups xmm0, [esi + 32]
        movups xmm1, [ecx + 32]
        cmpeqps xmm0, xmm1
        movmskps eax, xmm0
        cmp eax, 15
        jne copy
        xor eax, eax  // as the original: the last not-equal's false
        jmp done
    copy:
        movq xmm0, qword ptr [ecx]
        movq qword ptr [esi], xmm0
        movq xmm0, qword ptr [ecx + 8]
        movq qword ptr [esi + 8], xmm0
        movq xmm0, qword ptr [ecx + 16]
        movq qword ptr [esi + 16], xmm0
        movq xmm0, qword ptr [ecx + 24]
        movq qword ptr [esi + 24], xmm0
        movq xmm0, qword ptr [ecx + 32]
        movq qword ptr [esi + 32], xmm0
        movq xmm0, qword ptr [ecx + 40]
        movq qword ptr [esi + 40], xmm0
        lea eax, [ecx + 32]  // as the original without a setter
        mov edx, [edx + 0x10]
        test edx, edx
        jz done
        // The setter: the dirty words at shadow + 0x10, register f = index*3 + K1 and the two after it.
        add edx, 0x10
        shr ebx, 16
        lea edi, [edi + edi * 2]
        add edi, ebx
        mov ecx, edi
        mov eax, edi
        shr eax, 5
        and ecx, 31
        mov ebx, 0x80000000
        shr ebx, cl
        or [edx + eax * 4], ebx
        lea ecx, [edi + 1]
        mov eax, ecx
        shr eax, 5
        and ecx, 31
        mov ebx, 0x80000000
        shr ebx, cl
        or [edx + eax * 4], ebx
        lea ecx, [edi + 2]
        mov eax, ecx
        shr eax, 5
        and ecx, 31
        mov ebx, 0x80000000
        shr ebx, cl
        or [edx + eax * 4], ebx
        // The 48 bytes from the block's rows to the registers (dirty words + 0x40 + f * 16); eax = the destination,
        // as the setter's memcpy returns it.
        shl edi, 4
        lea eax, [edx + edi + 0x40]
        movups xmm0, [esi]
        movups [eax], xmm0
        movups xmm0, [esi + 16]
        movups [eax + 16], xmm0
        movups xmm0, [esi + 32]
        movups [eax + 32], xmm0
    done:
        pop ebx
        pop edi
        pop esi
        pop ecx
        ret 8
    }
}

// 0x41FB20 and 0x422110 (registers c8 on)
__declspec(naked) void mat_set_if_changed() {
    __asm {
        push ecx
        mov ecx, 0x00080006
        jmp mat_set_core
    }
}

__declspec(naked) void mat_set_if_changed_4221B0() {
    __asm {
        push ecx
        mov ecx, 0x00800078
        jmp mat_set_core
    }
}

// 0x431C30 and 0x431D40: pass a constant block's dirty registers on to the device, as runs. stdcall (dirty, ctx): the
// dirty words at dirty + 0 (eight, 256 registers, register r = bit 0x80000000 >> (r & 31) of word r >> 5), a mask at
// dirty + 0x20, the register data at dirty + 0x40 + r * 16; the device at [ctx + 0x10]. For each word, the masked bits:
// none ends a run carried in from the word before (flushed); otherwise the word loses them (written back as
// dirty & ~mask) and they are walked bit by bit, register order, a clear bit ending the run so far: the device's
// SetVertexShaderConstantF (vtable + 0x178, the first) or SetPixelShaderConstantF (+0x1B4, the second) gets (device,
// first register, its data, count). A run that reaches the end of a word goes on into the next; one still open after
// the eighth word is flushed last. Called for every draw (about 1000 a frame in Quelaag's Domain), most of the time
// with a few short runs: the walk tests all 32 bits of every word with something in it. Here each run is found with
// two bit scans instead: the same calls with the same arguments in the same order, the dirty words written at the
// same points with the same values (and words with nothing masked not written, as before). eax returns as the
// original's: the last call's result when a run was open at the end, else 8 (with ecx = dirty + 0x20, edx = 0).
// ebx, esi, edi and ebp are kept. Each entry loads its vtable offset into eax (not an argument) for the shared body.
__declspec(naked) void flush_run() {
    __asm {
        // eax = vtable offset, ecx = dirty, edx = ctx, esi = first register, ebx = count; eax/ecx/edx as the call leaves
        push edi
        mov edi, esi
        shl edi, 4
        lea edi, [ecx + edi + 0x40]
        mov ecx, [edx + 0x10]
        mov edx, [ecx]
        push ebx
        push edi
        push esi
        push ecx
        call dword ptr [edx + eax]
        pop edi
        ret
    }
}

__declspec(naked) void flush_core() {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        push eax  // [esp] = vtable offset, [esp+24] = dirty, [esp+28] = ctx
        xor ebx, ebx  // count of the open run
        xor esi, esi  // its first register
        xor edi, edi  // word
    word_loop:
        mov ecx, [esp + 24]
        mov eax, [ecx + edi * 4]
        mov edx, [ecx + edi * 4 + 0x20]
        mov ebp, eax
        and ebp, edx
        jnz has_bits
        test ebx, ebx
        jz next_word
        mov edx, [esp + 28]
        mov eax, [esp]
        call flush_run
        xor ebx, ebx
        jmp next_word
    has_bits:
        not edx
        and eax, edx
        mov [ecx + edi * 4], eax
        test ebx, ebx
        jz find
        test ebp, 0x80000000
        jz carried_end
        mov ecx, 31  // the run carried in goes on from this word's first register
        jmp count
    carried_end:
        mov ecx, [esp + 24]
        mov edx, [esp + 28]
        mov eax, [esp]
        call flush_run
        xor ebx, ebx
    find:
        test ebp, ebp
        jz next_word
        bsr ecx, ebp  // p: the bit of the lowest register left
        mov esi, edi
        shl esi, 5
        add esi, 31
        sub esi, ecx
    count:
        // the clear bits below p; none: the run goes to the end of the word (p + 1 registers)
        mov eax, 1
        shl eax, cl
        dec eax
        mov edx, ebp
        not edx
        and edx, eax
        jnz has_end
        lea ebx, [ebx + ecx + 1]
        jmp next_word
    has_end:
        bsr edx, edx  // q: the clear bit that ends the run (p - q registers)
        add ebx, ecx
        sub ebx, edx
        mov ecx, edx
        mov eax, 1
        shl eax, cl
        dec eax
        and ebp, eax
        mov ecx, [esp + 24]
        mov edx, [esp + 28]
        mov eax, [esp]
        call flush_run
        xor ebx, ebx
        jmp find
    next_word:
        inc edi
        cmp edi, 8
        jl word_loop
        test ebx, ebx
        jz no_last
        mov ecx, [esp + 24]
        mov edx, [esp + 28]
        mov eax, [esp]
        call flush_run
        jmp leave_
    no_last:
        mov eax, 8
        mov ecx, [esp + 24]
        add ecx, 0x20
        xor edx, edx
    leave_:
        add esp, 4
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret 8
    }
}

__declspec(naked) void flush_vs_constants() {
    __asm {
        mov eax, 0x178
        jmp flush_core
    }
}

__declspec(naked) void flush_ps_constants() {
    __asm {
        mov eax, 0x1B4
        jmp flush_core
    }
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

// The effects library's keyframe curves: 0x982C30 (four lanes) and 0x9817B0 (one float), cdecl (out, t, stream). The
// stream holds a count n, n key times and n values; it is moved past them first. For each lane, the time (through the
// x87 stack: a signaling NaN comes out quiet) is looked up from the last key down: the first key at or below it (a NaN
// compare goes on looking), key 0 at the latest (with n below 2 the search does not run: key n - 1). The segment's
// times and values are read through the x87 stack too; past the last key the end is the time itself and the same
// value. Then out = lerp(v0, v1, (t - t0) * r) with r = 1 / (t1 - t0), 0 where that is infinite. The four-lane one
// takes t - t0 from the time as given (not through the x87 stack) and works through the engine's helpers: subtract
// (0x417420), its reciprocal 0x430AC0 (in double; an infinite input gives a zero of its sign, a zero gives -infinity),
// the infinity mask 0x4193E0, and-not, multiply (0x5537E0) and the lerp 0x983C40 (subtract, then multiply-add
// 0x553700). The one-float one subtracts in double (0x4AD4E0) and does the reciprocal, multiply and multiply-add on the
// x87 stack (0x9846C0, 0x984500, 0x984560), so at the thread's x87 precision. Debug builds: every step was a call with
// a debug frame and its result copied through two or three stack temporaries; the particle updates call these for
// most of their parameters. Here the same operations in the same order, the four-lane ones through this file's
// replacements of those helpers, the one-float ones as the same x87 instructions. Returned: eax = out, edx = lane 2 of
// the result (four lanes) or out (one float), as the originals leave them.
namespace {
inline uint32_t x87_pass(uint32_t v) {
    // a signaling NaN (exponent all ones, quiet bit clear, the rest not all zero) gets its quiet bit
    return (v & 0x7FC00000u) == 0x7F800000u && (v & 0x003FFFFFu) ? (v | 0x00400000u) : v;
}
inline float bits_f(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

struct CurveKeys {
    const uint32_t* keys;
    const uint32_t* vals;
    int n;
};

inline CurveKeys curve_open(uintptr_t* stream) {
    const uintptr_t base = *stream;
    const int n = *reinterpret_cast<const int*>(base);
    const uintptr_t keys = base + 4;
    const uintptr_t vals = keys + 4u * static_cast<uint32_t>(n);
    *stream = vals + 4u * static_cast<uint32_t>(n);
    return {reinterpret_cast<const uint32_t*>(keys), reinterpret_cast<const uint32_t*>(vals), n};
}

// One lane's segment: t0, t1, v0, v1 (each through the x87 stack) for the time t as given.
inline void curve_segment(const CurveKeys& c, uint32_t t, uint32_t& t0, uint32_t& t1, uint32_t& v0, uint32_t& v1) {
    const uint32_t tq = x87_pass(t);
    const float tf = bits_f(tq);
    int j = c.n - 1;
    while (j > 0 && !(bits_f(c.keys[j]) <= tf)) --j;
    v0 = x87_pass(c.vals[j]);
    t0 = x87_pass(c.keys[j]);
    if (j + 1 < c.n) {
        v1 = x87_pass(c.vals[j + 1]);
        t1 = x87_pass(c.keys[j + 1]);
    } else {
        v1 = x87_pass(c.vals[j]);
        t1 = tq;
    }
}

// 0x430AC0 for one lane
inline uint32_t recip_430ac0(uint32_t v) {
    const float x = bits_f(v);
    if ((v & 0x7FFFFFFFu) == 0x7F800000u) return 0.0f > x ? 0x80000000u : 0u;
    if (x == 0.0f) return 0xFF800000u;  // +0 and -0 both (compared with -0)
    const __m128 r = _mm_cvtsd_ss(_mm_setzero_ps(), _mm_div_sd(_mm_set_sd(1.0), _mm_cvtss_sd(_mm_setzero_pd(), _mm_set_ss(x))));
    uint32_t out;
    _mm_store_ss(reinterpret_cast<float*>(&out), r);
    return out;
}

inline uint32_t inf_mask(uint32_t v) {
    return (v & 0x7FFFFFFFu) == 0x7F800000u ? 0xFFFFFFFFu : 0u;
}

// 0x4AD4E0: a - b in double, rounded to float
inline uint32_t sub_dbl(uint32_t a, uint32_t b) {
    const __m128d d = _mm_sub_sd(_mm_cvtss_sd(_mm_setzero_pd(), _mm_set_ss(bits_f(a))),
                                 _mm_cvtss_sd(_mm_setzero_pd(), _mm_set_ss(bits_f(b))));
    uint32_t out;
    _mm_store_ss(reinterpret_cast<float*>(&out), _mm_cvtsd_ss(_mm_setzero_ps(), d));
    return out;
}
}  // namespace

uint64_t __cdecl ffx_curve4(uint32_t* out, const uint32_t* t, uintptr_t* stream) {
    const CurveKeys c = curve_open(stream);
    alignas(16) uint32_t tl[4], t0[4], t1[4], v0[4], v1[4];
    std::memcpy(tl, t, 16);
    for (int i = 0; i < 4; ++i) curve_segment(c, tl[i], t0[i], t1[i], v0[i], v1[i]);
    alignas(16) uint32_t tr[4], d[4], dt[4], z[4], w[4], s[4], res[4];
    std::memcpy(tr, t, 16);
    vec4_sub(reinterpret_cast<float*>(d), reinterpret_cast<const float*>(tr), reinterpret_cast<const float*>(t0));
    vec4_sub(reinterpret_cast<float*>(dt), reinterpret_cast<const float*>(t1), reinterpret_cast<const float*>(t0));
    for (int i = 0; i < 4; ++i) {
        const uint32_t r = recip_430ac0(dt[i]);
        z[i] = r & ~inf_mask(r);
    }
    vec4_mul(reinterpret_cast<float*>(w), reinterpret_cast<const float*>(d), reinterpret_cast<const float*>(z));
    vec4_sub(reinterpret_cast<float*>(s), reinterpret_cast<const float*>(v1), reinterpret_cast<const float*>(v0));
    vec4_madd(reinterpret_cast<float*>(res), reinterpret_cast<const float*>(s), reinterpret_cast<const float*>(w),
              reinterpret_cast<const float*>(v0));
    std::memcpy(out, res, 16);
    return reinterpret_cast<uintptr_t>(out) | static_cast<uint64_t>(res[2]) << 32;
}

uint64_t __cdecl ffx_curve1(uint32_t* out, const uint32_t* t, uintptr_t* stream) {
    const CurveKeys c = curve_open(stream);
    uint32_t t0, t1, v0, v1;
    curve_segment(c, *t, t0, t1, v0, v1);
    const uint32_t d = sub_dbl(*t, t0);
    const uint32_t dt = sub_dbl(t1, t0);
    uint32_t r, tmp, w, s, res;
    __asm {
        fld dword ptr [dt]   // 0x984650: through the x87 stack onto the stack, 0x9846C0: 1 / x, stored as a float
        fstp dword ptr [tmp]
        fld dword ptr [tmp]
        fld1
        _emit 0xDE           // fdivrp st(1), st: st(1) = 1 / x
        _emit 0xF1
        fstp dword ptr [r]
    }
    const uint32_t z = r & ~inf_mask(r);
    s = sub_dbl(v1, v0);
    __asm {
        fld dword ptr [d]    // 0x984500: d * z
        fmul dword ptr [z]
        fstp dword ptr [w]
        fld dword ptr [s]    // 0x984560: s * w + v0
        fmul dword ptr [w]
        fadd dword ptr [v0]
        fstp dword ptr [res]
    }
    *out = res;
    return reinterpret_cast<uintptr_t>(out) | static_cast<uint64_t>(reinterpret_cast<uintptr_t>(out)) << 32;
}

// More of the effects library's debug-build glue, each the same helper calls in the same order with the same inputs,
// without the debug frame and the two or three stack copies of every intermediate result:
//  0x9AF7F0 (out0, out1, out2, a, b, c, m): three rows of a*m[k] + b*m[k+3] + c*m[k+6] + m[k+9] (rows of 16 bytes),
//    each a chain of three multiply-adds (0x553700); outputs written at the end in order; eax = out2[3], edx = out2[2].
//  0x9B5DC0 (s, c, x): the sine and cosine polynomials, powers through the four-float multiply 0x407840 (as 0x5537E0),
//    terms through the multiply-add 0x4157C0 (as 0x553700) with the coefficients splatted from 0x10D5C50/0x10D5C60
//    lanes 1-3, the cosine starting from 0x10D5280; eax = c[2], edx = c.
//  0x9B18A0 (out, r, g, b, a): four vectors saturated (0x407720), times the constant at 0x11E7E60 (splatted through
//    the x87 stack), to integers at x1, x256, x65536 and x2^24, merged by the bit select 0x403B10 under 0xFF, 0xFFFFFF
//    and 0xFFFF; eax = out, edx = out[3].
//  0x9B4C60 (base, offset, stride, r0, r1, r2, r3): the four rows read, transposed (moves only) and written to
//    base + offset + k * stride in order; eax = base (edx was a pointer into its own frame).
namespace {
using Fn2 = void*(__cdecl*)(void*, const void*);
using Fn4 = void*(__cdecl*)(void*, const void*, const void*, const void*);
inline void splat_u32(uint32_t* out, uint32_t v) {
    out[0] = out[1] = out[2] = out[3] = v;
}
inline uint32_t u32_at(const void* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
}  // namespace

uint64_t __cdecl ffx_mat3_madd(uint32_t* out0, uint32_t* out1, uint32_t* out2, const float* a, const float* b,
                               const float* c, const float* m) {
    alignas(16) float r0[4], r1[4], r2[4];
    vec4_madd(r0, a, m, m + 36);
    vec4_madd(r0, b, m + 12, r0);
    vec4_madd(r0, c, m + 24, r0);
    vec4_madd(r1, a, m + 4, m + 40);
    vec4_madd(r1, b, m + 16, r1);
    vec4_madd(r1, c, m + 28, r1);
    vec4_madd(r2, a, m + 8, m + 44);
    vec4_madd(r2, b, m + 20, r2);
    vec4_madd(r2, c, m + 32, r2);
    std::memcpy(out0, r0, 16);
    std::memcpy(out1, r1, 16);
    std::memcpy(out2, r2, 16);
    return u32_at(r2 + 3) | static_cast<uint64_t>(u32_at(r2 + 2)) << 32;
}

uint64_t __cdecl ffx_sincos_poly(uint32_t* out_s, uint32_t* out_c, const float* x) {
    alignas(16) float x2[4], x3[4], x4[4], x5[4], x6[4], x7[4], k[4], sn[4], cs[4];
    vec4_mul(x2, x, x);
    vec4_mul(x3, x2, x);
    vec4_mul(x4, x2, x2);
    vec4_mul(x5, x3, x2);
    vec4_mul(x6, x3, x3);
    vec4_mul(x7, x4, x3);
    const auto* kc = reinterpret_cast<const uint32_t*>(0x10D5C50);
    const auto* kd = reinterpret_cast<const uint32_t*>(0x10D5C60);
    const auto* one = reinterpret_cast<const float*>(0x10D5280);
    auto* ku = reinterpret_cast<uint32_t*>(k);
    splat_u32(ku, kc[1]);
    vec4_madd(sn, k, x3, x);
    splat_u32(ku, kc[2]);
    vec4_madd(sn, k, x5, sn);
    splat_u32(ku, kc[3]);
    vec4_madd(sn, k, x7, sn);
    splat_u32(ku, kd[1]);
    vec4_madd(cs, k, x2, one);
    splat_u32(ku, kd[2]);
    vec4_madd(cs, k, x4, cs);
    splat_u32(ku, kd[3]);
    vec4_madd(cs, k, x6, cs);
    std::memcpy(out_s, sn, 16);
    std::memcpy(out_c, cs, 16);
    return u32_at(cs + 2) | static_cast<uint64_t>(reinterpret_cast<uintptr_t>(out_c)) << 32;
}

uint64_t __cdecl ffx_pack_colors(uint32_t* out, const uint32_t* p2, const uint32_t* p3, const uint32_t* p4,
                                 const uint32_t* p5) {
    alignas(16) uint32_t a[4], b[4], c[4], d[4];
    std::memcpy(a, p4, 16);
    std::memcpy(b, p3, 16);
    std::memcpy(c, p2, 16);
    std::memcpy(d, p5, 16);
    alignas(16) uint32_t scale[4], m8[4], m24[4], m16[4];
    float sv;
    std::memcpy(&sv, reinterpret_cast<const void*>(0x11E7E60), 4);
    ffx_splat_x87(reinterpret_cast<float*>(scale), sv);
    splat_u32(m8, 0xFF);
    splat_u32(m24, 0xFFFFFF);
    splat_u32(m16, 0xFFFF);
    const auto sat = reinterpret_cast<Fn2>(0x407720);
    alignas(16) uint32_t sa[4], sb[4], sc[4], sd[4];
    sat(sa, a);
    sat(sb, b);
    sat(sc, c);
    sat(sd, d);
    alignas(16) float ma[4], mb[4], mc[4], md[4];
    vec4_mul(ma, reinterpret_cast<const float*>(sa), reinterpret_cast<const float*>(scale));
    vec4_mul(mb, reinterpret_cast<const float*>(sb), reinterpret_cast<const float*>(scale));
    vec4_mul(mc, reinterpret_cast<const float*>(sc), reinterpret_cast<const float*>(scale));
    vec4_mul(md, reinterpret_cast<const float*>(sd), reinterpret_cast<const float*>(scale));
    alignas(16) uint32_t ia[4], ib[4], ic[4], id[4];
    ffx_to_int_1(ia, ma);
    ffx_to_int_256(ib, mb);
    ffx_to_int_65536(ic, mc);
    ffx_to_int_2p24(id, md);
    const auto sel = reinterpret_cast<Fn4>(0x403B10);
    alignas(16) uint32_t q1[4], q2[4], r[4];
    sel(q1, ib, ia, m8);
    sel(q2, id, ic, m24);
    sel(r, q2, q1, m16);
    std::memcpy(out, r, 16);
    return reinterpret_cast<uintptr_t>(out) | static_cast<uint64_t>(r[3]) << 32;
}

uint64_t __cdecl ffx_rows_transposed(uint8_t* base, int offset, int stride, const uint32_t* r0, const uint32_t* r1,
                                     const uint32_t* r2, const uint32_t* r3) {
    uint32_t m[4][4];
    std::memcpy(m[0], r0, 16);
    std::memcpy(m[1], r1, 16);
    std::memcpy(m[2], r2, 16);
    std::memcpy(m[3], r3, 16);
    const int at[4] = {offset, offset + stride, offset + stride * 2, stride * 3 + offset};
    for (int k = 0; k < 4; ++k) {
        const uint32_t row[4] = {m[0][k], m[1][k], m[2][k], m[3][k]};
        std::memcpy(base + at[k], row, 16);
    }
    return reinterpret_cast<uintptr_t>(base) | static_cast<uint64_t>(reinterpret_cast<uintptr_t>(base)) << 32;
}

// 0x446D30 (thiscall: this = a box, min at +0 and max at +0x10; out, m; pops 8): the box through the 4x4 matrix m
// (rows of four) as a box again, the engine's port of a vector routine, in scalar SSE through double. With P = the min
// corner (lanes by the mask at 0x10D54F0, the rest from 0x10D5280: x y z 1), e = max - min, and the zero vector Z at
// 0x10D5260: the products R0 = e.x * row 0 (lane 0 as row * e, the others e * row: which NaN wins), R1 = row 1 * e.y,
// R2 = row 2 * e.z; the corner Pt = row 0 * P.x + (row 1 * P.y + (row 2 * P.z + row 3 * P.w)), each step a
// multiply-add rounded once from double; per product a sign mask N = (R < Z, or unordered), min = ((LO0 + LO1) + LO2)
// + Pt and max = ((HI0 + HI1) + HI2) + Pt with LO = N ? R : Z and HI = N ? Z : R; then min is clamped to Q = Pt with
// the lanes of the mask at 0x10D5420 from 0x10D5280 where it lies above Q, max where it lies below (ordered compares:
// a NaN sum stays), and both written through that mask again. The original also writes Q to both halves first; every
// read is done by then and both halves are written again, so only the final values are stored here. Every operation
// as the original's (products and sums rounded to float each, operand order kept); eax = out as before.
__declspec(naked) void aabb_transform() {
    __asm {
        push ebp
        mov ebp, esp
        and esp, -16
        sub esp, 0x40
        mov eax, [ebp + 12]
        movups xmm7, [ecx]
        movups xmm6, [ecx + 16]
        subps xmm6, xmm7
        mov edx, 0x10D54F0
        movups xmm5, [edx]
        andps xmm7, xmm5
        mov edx, 0x10D5280
        movups xmm4, [edx]
        andnps xmm5, xmm4
        orps xmm7, xmm5
        // products: row 0 lane 0 M first, lanes 1-3 e0 first; rows 1 and 2 M first
        movaps xmm0, xmm6
        shufps xmm0, xmm0, 0x00
        movups xmm1, [eax]
        movaps xmm2, xmm1
        mulps xmm2, xmm0
        mulps xmm0, xmm1
        movss xmm0, xmm2
        movaps [esp + 16], xmm0
        movaps xmm0, xmm6
        shufps xmm0, xmm0, 0x55
        movups xmm1, [eax + 16]
        mulps xmm1, xmm0
        movaps [esp + 32], xmm1
        movaps xmm0, xmm6
        shufps xmm0, xmm0, 0xAA
        movups xmm1, [eax + 32]
        mulps xmm1, xmm0
        movaps [esp + 48], xmm1
        // the corner: M row 3 * p3, then rows 2, 1, 0 multiply-added
        movaps xmm0, xmm7
        shufps xmm0, xmm0, 0xFF
        movups xmm2, [eax + 48]
        mulps xmm2, xmm0
        movups xmm0, [eax + 32]
        movaps xmm1, xmm7
        shufps xmm1, xmm1, 0xAA
        movaps xmm4, xmm2
        cvtps2pd xmm2, xmm0
        cvtps2pd xmm3, xmm1
        mulpd xmm2, xmm3
        movhlps xmm0, xmm0
        movhlps xmm1, xmm1
        cvtps2pd xmm0, xmm0
        cvtps2pd xmm1, xmm1
        mulpd xmm0, xmm1
        cvtps2pd xmm3, xmm4
        addpd xmm2, xmm3
        movhlps xmm4, xmm4
        cvtps2pd xmm4, xmm4
        addpd xmm0, xmm4
        cvtpd2ps xmm2, xmm2
        cvtpd2ps xmm0, xmm0
        movlhps xmm2, xmm0
        movups xmm0, [eax + 16]
        movaps xmm1, xmm7
        shufps xmm1, xmm1, 0x55
        movaps xmm4, xmm2
        cvtps2pd xmm2, xmm0
        cvtps2pd xmm3, xmm1
        mulpd xmm2, xmm3
        movhlps xmm0, xmm0
        movhlps xmm1, xmm1
        cvtps2pd xmm0, xmm0
        cvtps2pd xmm1, xmm1
        mulpd xmm0, xmm1
        cvtps2pd xmm3, xmm4
        addpd xmm2, xmm3
        movhlps xmm4, xmm4
        cvtps2pd xmm4, xmm4
        addpd xmm0, xmm4
        cvtpd2ps xmm2, xmm2
        cvtpd2ps xmm0, xmm0
        movlhps xmm2, xmm0
        movups xmm0, [eax]
        movaps xmm1, xmm7
        shufps xmm1, xmm1, 0x00
        movaps xmm4, xmm2
        cvtps2pd xmm2, xmm0
        cvtps2pd xmm3, xmm1
        mulpd xmm2, xmm3
        movhlps xmm0, xmm0
        movhlps xmm1, xmm1
        cvtps2pd xmm0, xmm0
        cvtps2pd xmm1, xmm1
        mulpd xmm0, xmm1
        cvtps2pd xmm3, xmm4
        addpd xmm2, xmm3
        movhlps xmm4, xmm4
        cvtps2pd xmm4, xmm4
        addpd xmm0, xmm4
        cvtpd2ps xmm2, xmm2
        cvtpd2ps xmm0, xmm0
        movlhps xmm2, xmm0
        movaps [esp], xmm2
        mov edx, 0x10D5260
        movups xmm7, [edx]
        movaps xmm0, [esp + 16]
        movaps xmm4, xmm7
        cmpnleps xmm4, xmm0
        movaps xmm1, xmm4
        andps xmm1, xmm0
        movaps xmm3, xmm4
        andnps xmm3, xmm7
        orps xmm1, xmm3
        movaps xmm3, xmm4
        andps xmm3, xmm7
        andnps xmm4, xmm0
        orps xmm3, xmm4
        movaps xmm5, xmm1
        movaps xmm6, xmm3
        movaps xmm0, [esp + 32]
        movaps xmm4, xmm7
        cmpnleps xmm4, xmm0
        movaps xmm1, xmm4
        andps xmm1, xmm0
        movaps xmm3, xmm4
        andnps xmm3, xmm7
        orps xmm1, xmm3
        movaps xmm3, xmm4
        andps xmm3, xmm7
        andnps xmm4, xmm0
        orps xmm3, xmm4
        addps xmm5, xmm1
        addps xmm6, xmm3
        movaps xmm0, [esp + 48]
        movaps xmm4, xmm7
        cmpnleps xmm4, xmm0
        movaps xmm1, xmm4
        andps xmm1, xmm0
        movaps xmm3, xmm4
        andnps xmm3, xmm7
        orps xmm1, xmm3
        movaps xmm3, xmm4
        andps xmm3, xmm7
        andnps xmm4, xmm0
        orps xmm3, xmm4
        addps xmm5, xmm1
        addps xmm6, xmm3
        movaps xmm0, [esp]
        addps xmm5, xmm0
        addps xmm6, xmm0
        // Q = (Pt & ~S) | (ONE & S)
        mov edx, 0x10D5420
        movups xmm1, [edx]
        mov edx, 0x10D5280
        movups xmm2, [edx]
        andps xmm2, xmm1
        movaps xmm3, xmm1
        andnps xmm3, xmm0
        orps xmm3, xmm2
        // min: Q where the sum is above it (ordered); max: Q where the sum is below it
        movaps xmm4, xmm3
        cmpltps xmm4, xmm5
        movaps xmm0, xmm4
        andps xmm0, xmm3
        andnps xmm4, xmm5
        orps xmm4, xmm0
        movaps xmm7, xmm6
        cmpltps xmm7, xmm3
        movaps xmm0, xmm7
        andps xmm0, xmm3
        andnps xmm7, xmm6
        orps xmm7, xmm0
        movaps xmm0, xmm1
        andnps xmm0, xmm4
        orps xmm0, xmm2
        movaps xmm5, xmm1
        andnps xmm5, xmm7
        orps xmm5, xmm2
        mov eax, [ebp + 8]
        movups [eax], xmm0
        movups [eax + 16], xmm5
        mov esp, ebp
        pop ebp
        ret 8
    }
}

// 0x994AE0 (n, a, b, c): up to three arrays of vectors (16 bytes each) passed through 0x44C6A0 in place (through its
// effects-library wrapper 0x9B6D00), four floats' worth of n at a time ((n + 3) / 4 vectors), from the last to the
// first; a missing array (null) is a zero vector on the stack, not moved on. Nothing when n is 0 or all three are
// null. Each step reads the three vectors, passes the three through 0x44C6A0 in order and writes the three back in
// order, as the original; only the debug frame, the copy helpers 0x5538B0 / 0x553880 and the wrapper's copies are
// gone. The callers do not use the return value (eax = the last array written, as the original leaves it).
uint32_t __cdecl ffx_arrays_44c6a0(uint32_t n, uint8_t* a, uint8_t* b, uint8_t* c) {
    if ((!a && !b && !c) || !n) return 0;
    const uint32_t sa = a ? 16 : 0, sb = b ? 16 : 0, sc = c ? 16 : 0;
    alignas(16) uint32_t zero[4] = {0, 0, 0, 0};
    if (!a) a = reinterpret_cast<uint8_t*>(zero);
    if (!b) b = reinterpret_cast<uint8_t*>(zero);
    if (!c) c = reinterpret_cast<uint8_t*>(zero);
    uint32_t off = ((n + 3) >> 2) << 4;
    uint32_t oa = sa ? off : 0, ob = sb ? off : 0, oc = sc ? off : 0;
    const auto f = reinterpret_cast<void*(__cdecl*)(void*, const void*)>(0x44C6A0);
    do {
        off -= 16;
        oa -= sa;
        ob -= sb;
        oc -= sc;
        alignas(16) uint32_t va[4], vb[4], vc[4], ra[4], rb[4], rc[4];
        std::memcpy(va, a + oa, 16);
        std::memcpy(vb, b + ob, 16);
        std::memcpy(vc, c + oc, 16);
        f(ra, va);
        f(rb, vb);
        f(rc, vc);
        std::memcpy(a + oa, ra, 16);
        std::memcpy(b + ob, rb, 16);
        std::memcpy(c + oc, rc, 16);
    } while (off != 0);
    return reinterpret_cast<uintptr_t>(c);
}

// 0x995C60 (min, max, n, x, y, z): the bounds of n points given as three float arrays. Running minimum and maximum
// per axis start at the floats at 0x11E7ECC / 0x11E822C; the points past the last whole four go first, one at a
// time through 0x440400 (b > a ? a : b) and 0x410D90 (a > b ? a : b), each result through the x87 stack (a signaling
// NaN comes out quiet); with no whole four the outputs are (min x, y, z, 1) and (max x, y, z, 1). Otherwise the
// running values are splatted (through the x87 stack) and the fours taken from the last to the first through the
// four-float 0x407580 / 0x4074F0 (the same compares per lane), then each set of three with the vector at 0x10D5280
// transposed and reduced pairwise ((0, 1), (2, 3), then the two). The same compares in the same order (as SSE
// compares, the same results on float values); the debug frame, the exception frame and the copies are gone. The
// callers do not use the return value.
namespace {
// 0x407580 per lane: b > a ? a : b
inline __m128 vmin_407580(__m128 a, __m128 b) {
    const __m128 m = _mm_cmplt_ps(a, b);
    return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
}
// 0x4074F0 per lane: a > b ? a : b
inline __m128 vmax_4074f0(__m128 a, __m128 b) {
    const __m128 m = _mm_cmplt_ps(b, a);
    return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
}
inline __m128 reduce4(__m128 x, __m128 y, __m128 z, __m128 w, bool max) {
    _MM_TRANSPOSE4_PS(x, y, z, w);
    if (max) return vmax_4074f0(vmax_4074f0(x, y), vmax_4074f0(z, w));
    return vmin_407580(vmin_407580(x, y), vmin_407580(z, w));
}
}  // namespace

uint32_t __cdecl ffx_bounds3(uint32_t* out_min, uint32_t* out_max, uint32_t n, const uint32_t* xs, const uint32_t* ys,
                             const uint32_t* zs) {
    const uint32_t groups = n >> 2;
    const uint32_t* arr[3] = {xs, ys, zs};
    uint32_t mn[3], mx[3];
    for (int k = 0; k < 3; ++k) {
        mn[k] = x87_pass(*reinterpret_cast<const uint32_t*>(0x11E7ECC));
        mx[k] = x87_pass(*reinterpret_cast<const uint32_t*>(0x11E822C));
    }
    for (uint32_t i = groups * 4; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = arr[k][i];
            mn[k] = x87_pass(bits_f(v) > bits_f(mn[k]) ? mn[k] : v);
        }
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = arr[k][i];
            mx[k] = x87_pass(bits_f(mx[k]) > bits_f(v) ? mx[k] : v);
        }
    }
    if (!groups) {
        const uint32_t lo[4] = {mn[0], mn[1], mn[2], 0x3F800000u};
        const uint32_t hi[4] = {mx[0], mx[1], mx[2], 0x3F800000u};
        std::memcpy(out_min, lo, 16);
        std::memcpy(out_max, hi, 16);
        return 0;
    }
    __m128 bmin[3], bmax[3];
    for (int k = 0; k < 3; ++k) {
        bmin[k] = _mm_set1_ps(bits_f(mn[k]));
        bmax[k] = _mm_set1_ps(bits_f(mx[k]));
    }
    uint32_t off = groups * 16;
    do {
        off -= 16;
        __m128 v[3];
        for (int k = 0; k < 3; ++k) v[k] = _mm_loadu_ps(reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(arr[k]) + off));
        for (int k = 0; k < 3; ++k) bmin[k] = vmin_407580(bmin[k], v[k]);
        for (int k = 0; k < 3; ++k) bmax[k] = vmax_4074f0(bmax[k], v[k]);
    } while (off != 0);
    const __m128 one = _mm_loadu_ps(reinterpret_cast<const float*>(0x10D5280));
    _mm_storeu_ps(reinterpret_cast<float*>(out_min), reduce4(bmin[0], bmin[1], bmin[2], one, false));
    _mm_storeu_ps(reinterpret_cast<float*>(out_max), reduce4(bmax[0], bmax[1], bmax[2], one, true));
    return 0;
}

// 0x99C540, the effects library's particle quad builder, spends its time in a loop over the particles
// (0x99E9B0..0x99F1EA; counter at ebp-0x1484, n = the third argument, particles of 72 bytes from the second, output
// entries of 48 bytes from the first). Per particle: three splats (through the x87 stack) of floats 0, 4, 8 into the
// three-row multiply-add 0x9AF7F0 with the matrix at ebp-0x12C0; splats of 0xC and 0x10 into the multiply-adds
// 0x553700 with the rows at ebp-0x4E0 / -0x890 and -0x500 / -0x8B0; splats of 0x38..0x44 times the rows at
// ebp-0x8F0, -0x910, -0x930, -0x8D0 (0x5537E0) into the colour packing 0x9B18A0; then three transposed-row writes
// (0x9B4C60) to the output at offsets 0, 16 and 32 with the stride at ebp-0x668 (with the rows at ebp-0x540 and
// ebp-0x1430). The body is straight-line and reads no frame cell it writes (no state carried between particles), so
// here the first n - 1 particles are done with the same operations in the same order on the same values without the
// debug copies, and the last one is left to the game's code (the counter set to n - 1 and the loop entered at its
// test): every frame cell and register the function goes on with is as before.
volatile long g_ploop_calls = 0, g_ploop_iters = 0;

uint32_t __cdecl ffx_particle_loop(uint8_t* ebp, const uint8_t* ebx) {
    const uint32_t n = *reinterpret_cast<const uint32_t*>(ebx + 0x10);
    ++g_ploop_calls;
    g_ploop_iters += n;
    if (n < 2) return 0;
    const uint32_t parts = *reinterpret_cast<const uint32_t*>(ebx + 0xC);
    const uint32_t outs = *reinterpret_cast<const uint32_t*>(ebx + 8);
    const auto F = [ebp](uint32_t off) { return reinterpret_cast<const float*>(ebp - off); };
    const auto U = [ebp](uint32_t off) { return reinterpret_cast<const uint32_t*>(ebp - off); };
    for (uint32_t i = 0; i + 1 < n; ++i) {
        const auto* p = reinterpret_cast<const uint32_t*>(parts + 72 * i);
        alignas(16) uint32_t r1[4], r2[4], r3[4], r4[12], r5[4], r6[4], r9[4], r10[4], r11[4], r12[4];
        alignas(16) float r7[4], r8[4], r13[4], r14[4], r15[4], r16[4];
        alignas(16) uint32_t r17[4];
        splat_u32(r1, x87_pass(p[0]));
        splat_u32(r2, x87_pass(p[1]));
        splat_u32(r3, x87_pass(p[2]));
        ffx_mat3_madd(r4, r4 + 4, r4 + 8, reinterpret_cast<const float*>(r1), reinterpret_cast<const float*>(r2),
                      reinterpret_cast<const float*>(r3), F(0x12C0));
        splat_u32(r5, x87_pass(p[3]));
        splat_u32(r6, x87_pass(p[4]));
        vec4_madd(r7, reinterpret_cast<const float*>(r5), F(0x4E0), F(0x890));
        vec4_madd(r8, reinterpret_cast<const float*>(r6), F(0x500), F(0x8B0));
        splat_u32(r9, x87_pass(p[14]));
        splat_u32(r10, x87_pass(p[15]));
        splat_u32(r11, x87_pass(p[16]));
        splat_u32(r12, x87_pass(p[17]));
        vec4_mul(r13, reinterpret_cast<const float*>(r9), F(0x8F0));
        vec4_mul(r14, reinterpret_cast<const float*>(r10), F(0x910));
        vec4_mul(r15, reinterpret_cast<const float*>(r11), F(0x930));
        vec4_mul(r16, reinterpret_cast<const float*>(r12), F(0x8D0));
        ffx_pack_colors(r17, reinterpret_cast<const uint32_t*>(r13), reinterpret_cast<const uint32_t*>(r14),
                        reinterpret_cast<const uint32_t*>(r15), reinterpret_cast<const uint32_t*>(r16));
        auto* o = reinterpret_cast<uint8_t*>(outs + 48 * i);
        const int stride = *reinterpret_cast<const int*>(ebp - 0x668);
        ffx_rows_transposed(o, 0, stride, r4, r4 + 4, r4 + 8, U(0x540));
        ffx_rows_transposed(o, 16, stride, U(0x540), U(0x540), r17, reinterpret_cast<const uint32_t*>(r7));
        ffx_rows_transposed(o, 32, stride, reinterpret_cast<const uint32_t*>(r8), U(0x540), U(0x1430), U(0x540));
    }
    return n - 1;
}

// Entered by a jump at 0x99E9B0 (the loop's counter = 0): the particles up to the last, then the counter, eax, ecx
// and edx as the game's code has them there, and on to the loop's test at 0x99E9CB.
__declspec(naked) void ffx_particle_loop_entry() {
    __asm {
        push ebx
        push ebp
        call ffx_particle_loop
        add esp, 8
        mov dword ptr [ebp - 0x1484], eax
        mov eax, dword ptr [ebp - 0x1474]
        mov ecx, dword ptr [ebp - 0x147C]
        mov edx, dword ptr [ebp - 0x1478]
        push 0x99E9CB
        ret
    }
}

// 0xA4D420, the effects library's particle update, spends its time in a loop over its particle arrays four at a time
// (0xA4DF22..0xA4FB9A): the byte offset at ebp-0x730 counts down by 16 from [ebx+0x20] * 16; each pass reads 16 bytes
// at that offset from the arrays at [ebx+8] .. [ebx+0x28], runs a fixed chain of the library's helpers (the part
// from 0xA4E658 only when [ebx+0x4C] is set; the parameter evaluations 0x9B4650 on the objects at [ebx+0x44] and
// [ebx+0x48]) and writes six results back at the offset. The two paths below are generated from the game's code
// (tools/test_rendermath/ffxloop, a symbolic trace of the loop body): the same helper calls at the same addresses, in
// the same order, with the same inputs, and the same six writes, without the debug copies. Only the offset is carried
// from pass to pass, so the passes down to offset 16 are done here and the last (offset 0) is left to the game's
// code, entered at the loop's top with the counter at 16: every frame cell the function goes on with is as before.
static void a4_iter_skip(uint8_t* ebp, const uint8_t* ebx, uint32_t off) {
    alignas(16) uint32_t r1[4];  // 00A4DF43
    std::memcpy(r1, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x8)) + off, 16);
    alignas(16) uint32_t r2[4];  // 00A4DFB0
    std::memcpy(r2, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0xc)) + off, 16);
    alignas(16) uint32_t r3[4];  // 00A4E01D
    std::memcpy(r3, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x10)) + off, 16);
    alignas(16) uint32_t r4[4];  // 00A4E08A
    std::memcpy(r4, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x14)) + off, 16);
    alignas(16) uint32_t r5[4];  // 00A4E0F7
    std::memcpy(r5, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x18)) + off, 16);
    alignas(16) uint32_t r6[4];  // 00A4E164
    std::memcpy(r6, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x1c)) + off, 16);
    alignas(16) uint32_t r7[4];  // 00A4E1D1
    std::memcpy(r7, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x24)) + off, 16);
    alignas(16) uint32_t r8[4];  // 00A4E23E
    std::memcpy(r8, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x28)) + off, 16);
    alignas(16) uint32_t r9[4];  // 00A4E2AE
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984B60)(r9, reinterpret_cast<const uint32_t*>(ebp - 0xd0), r7);
    alignas(16) uint32_t r10[4];  // 00A4E31E
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x9B5990)(r10, r4, reinterpret_cast<const uint32_t*>(ebp - 0x370));
    alignas(16) uint32_t r11[4];  // 00A4E38E
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x9B5990)(r11, r5, reinterpret_cast<const uint32_t*>(ebp - 0x390));
    alignas(16) uint32_t r12[4];  // 00A4E3FE
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x9B5990)(r12, r6, reinterpret_cast<const uint32_t*>(ebp - 0x3b0));
    alignas(16) uint32_t r13[4];  // 00A4E475
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r13, r10, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r1);
    alignas(16) uint32_t r14[4];  // 00A4E4EC
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r14, r11, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r2);
    alignas(16) uint32_t r15[4];  // 00A4E563
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r15, r12, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r3);
    alignas(16) uint32_t r16[4];  // 00A4F07C
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r16, reinterpret_cast<const uint32_t*>(ebp - 0x4d0), reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r4);
    alignas(16) uint32_t r17[4];  // 00A4F0F3
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r17, reinterpret_cast<const uint32_t*>(ebp - 0x4f0), reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r5);
    alignas(16) uint32_t r18[4];  // 00A4F16A
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r18, reinterpret_cast<const uint32_t*>(ebp - 0x510), reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r6);
    alignas(16) uint32_t r19[4];  // 00A4F1E1
    reinterpret_cast<void*(__fastcall*)(uint32_t, uint32_t, void*, const void*, uint32_t, const void*)>(0x9B4650)(*reinterpret_cast<const uint32_t*>(ebx + 0x44), 0, r19, r9, *reinterpret_cast<const uint32_t*>(ebx + 0x2c), r8);
    alignas(16) uint32_t r20[4];  // 00A4F225
    reinterpret_cast<void*(__fastcall*)(uint32_t, uint32_t, void*, const void*, uint32_t, const void*)>(0x9B4650)(*reinterpret_cast<const uint32_t*>(ebx + 0x48), 0, r20, r7, *reinterpret_cast<const uint32_t*>(ebx + 0x2c), r8);
    alignas(16) uint32_t r21[4];  // 00A4F23F
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r21, r19, reinterpret_cast<const uint32_t*>(ebp - 0x2f0));
    alignas(16) uint32_t r22[4];  // 00A4F2AF
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r22, r21, r20);
    alignas(16) uint32_t r23[4];  // 00A4F31F
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984B60)(r23, reinterpret_cast<const uint32_t*>(ebp - 0x330), r22);
    alignas(16) uint32_t r24[4];  // 00A4F38F
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r24, r23, r23);
    alignas(16) uint32_t r25[4];  // 00A4F406
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x9B2EC0)(r25, r16, r17, r18);
    alignas(16) uint32_t r26[4];  // 00A4F46F
    reinterpret_cast<void*(__cdecl*)(void*, const void*)>(0x9B5CE0)(r26, r25);
    alignas(16) uint32_t r27[4];  // 00A4F4D8
    reinterpret_cast<void*(__cdecl*)(void*, const void*)>(0x985500)(r27, r26);
    alignas(16) uint32_t r28[4];  // 00A4F541
    reinterpret_cast<void*(__cdecl*)(void*, const void*)>(0xA520C0)(r28, r26);
    alignas(16) uint32_t r29[4];  // 00A4F5B1
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0xA51E80)(r29, r27, r28);
    alignas(16) uint32_t r30[4];  // 00A4F621
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0xA51F40)(r30, r25, r24);
    alignas(16) uint32_t r31[4];  // 00A4F691
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r31, r26, r29);
    alignas(16) uint32_t r32[4];  // 00A4F701
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r32, r16, r31);
    alignas(16) uint32_t r33[4];  // 00A4F771
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r33, r17, r31);
    alignas(16) uint32_t r34[4];  // 00A4F7E1
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r34, r18, r31);
    alignas(16) uint32_t r35[4];  // 00A4F858
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r35, r32, r22, r16);
    alignas(16) uint32_t r36[4];  // 00A4F8CF
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r36, r33, r22, r17);
    alignas(16) uint32_t r37[4];  // 00A4F946
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r37, r34, r22, r18);
    alignas(16) uint32_t r38[4];  // 00A4F9B6
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r38, r35, r30);
    alignas(16) uint32_t r39[4];  // 00A4FA26
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r39, r36, r30);
    alignas(16) uint32_t r40[4];  // 00A4FA96
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r40, r37, r30);
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x8)) + off, r13, 16);  // 00A4FB03
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0xc)) + off, r14, 16);  // 00A4FB1D
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x10)) + off, r15, 16);  // 00A4FB37
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x14)) + off, r38, 16);  // 00A4FB51
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x18)) + off, r39, 16);  // 00A4FB6B
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x1c)) + off, r40, 16);  // 00A4FB85
}

static void a4_iter_full(uint8_t* ebp, const uint8_t* ebx, uint32_t off) {
    alignas(16) uint32_t r1[4];  // 00A4DF43
    std::memcpy(r1, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x8)) + off, 16);
    alignas(16) uint32_t r2[4];  // 00A4DFB0
    std::memcpy(r2, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0xc)) + off, 16);
    alignas(16) uint32_t r3[4];  // 00A4E01D
    std::memcpy(r3, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x10)) + off, 16);
    alignas(16) uint32_t r4[4];  // 00A4E08A
    std::memcpy(r4, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x14)) + off, 16);
    alignas(16) uint32_t r5[4];  // 00A4E0F7
    std::memcpy(r5, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x18)) + off, 16);
    alignas(16) uint32_t r6[4];  // 00A4E164
    std::memcpy(r6, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x1c)) + off, 16);
    alignas(16) uint32_t r7[4];  // 00A4E1D1
    std::memcpy(r7, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x24)) + off, 16);
    alignas(16) uint32_t r8[4];  // 00A4E23E
    std::memcpy(r8, reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x28)) + off, 16);
    alignas(16) uint32_t r9[4];  // 00A4E2AE
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984B60)(r9, reinterpret_cast<const uint32_t*>(ebp - 0xd0), r7);
    alignas(16) uint32_t r10[4];  // 00A4E31E
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x9B5990)(r10, r4, reinterpret_cast<const uint32_t*>(ebp - 0x370));
    alignas(16) uint32_t r11[4];  // 00A4E38E
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x9B5990)(r11, r5, reinterpret_cast<const uint32_t*>(ebp - 0x390));
    alignas(16) uint32_t r12[4];  // 00A4E3FE
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x9B5990)(r12, r6, reinterpret_cast<const uint32_t*>(ebp - 0x3b0));
    alignas(16) uint32_t r13[4];  // 00A4E475
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r13, r10, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r1);
    alignas(16) uint32_t r14[4];  // 00A4E4EC
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r14, r11, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r2);
    alignas(16) uint32_t r15[4];  // 00A4E563
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r15, r12, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r3);
    alignas(16) uint32_t r16[4];  // 00A4E669
    reinterpret_cast<void*(__cdecl*)(void*, uint32_t)>(0x984920)(r16, x87_pass(*reinterpret_cast<const uint32_t*>(0x110b414)));
    alignas(16) uint32_t r17[4];  // 00A4E6D4
    std::memcpy(r17, ebp - 0x640 + 0x0u, 16);
    alignas(16) uint32_t r18[4];  // 00A4E73F
    std::memcpy(r18, ebp - 0x660 + 0x0u, 16);
    alignas(16) uint32_t r19[4];  // 00A4E7AA
    std::memcpy(r19, ebp - 0x680 + 0x0u, 16);
    alignas(16) uint32_t r20[4];  // 00A4E815
    std::memcpy(r20, ebp - 0x6a0 + 0x0u, 16);
    alignas(16) uint32_t r21[4];  // 00A4E880
    std::memcpy(r21, ebp - 0x6c0 + 0x0u, 16);
    alignas(16) uint32_t r22[4];  // 00A4E8F0
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984B60)(r22, r17, r13);
    alignas(16) uint32_t r23[4];  // 00A4E960
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984B60)(r23, r18, r14);
    alignas(16) uint32_t r24[4];  // 00A4E9D0
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984B60)(r24, r19, r15);
    alignas(16) uint32_t r25[4];  // 00A4EA47
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x9B2EC0)(r25, r22, r23, r24);
    alignas(16) uint32_t r26[4];  // 00A4EAB7
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0xA52000)(r26, r25, r20);
    alignas(16) uint32_t r27[4];  // 00A4EB27
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0xA52000)(r27, r16, r25);
    alignas(16) uint32_t r28[4];  // 00A4EB97
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r28, r26, r27);
    alignas(16) uint32_t r29[4];  // 00A4EC00
    reinterpret_cast<void*(__cdecl*)(void*, const void*)>(0x9B5CE0)(r29, r25);
    alignas(16) uint32_t r30[4];  // 00A4EC70
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r30, r22, r29);
    alignas(16) uint32_t r31[4];  // 00A4ECE0
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r31, r23, r29);
    alignas(16) uint32_t r32[4];  // 00A4ED50
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r32, r24, r29);
    alignas(16) uint32_t r33[4];  // 00A4EDC0
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r33, r29, r29);
    alignas(16) uint32_t r34[4];  // 00A4EE30
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r34, r21, r33);
    alignas(16) uint32_t r35[4];  // 00A4EEA0
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x9B66B0)(r35, r34, r28);
    alignas(16) uint32_t r36[4];  // 00A4EF17
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r36, r35, r30, reinterpret_cast<const uint32_t*>(ebp - 0x4d0));
    alignas(16) uint32_t r37[4];  // 00A4EF8E
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r37, r35, r31, reinterpret_cast<const uint32_t*>(ebp - 0x4f0));
    alignas(16) uint32_t r38[4];  // 00A4F005
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r38, r35, r32, reinterpret_cast<const uint32_t*>(ebp - 0x510));
    alignas(16) uint32_t r39[4];  // 00A4F07C
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r39, r36, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r4);
    alignas(16) uint32_t r40[4];  // 00A4F0F3
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r40, r37, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r5);
    alignas(16) uint32_t r41[4];  // 00A4F16A
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r41, r38, reinterpret_cast<const uint32_t*>(ebp - 0x2f0), r6);
    alignas(16) uint32_t r42[4];  // 00A4F1E1
    reinterpret_cast<void*(__fastcall*)(uint32_t, uint32_t, void*, const void*, uint32_t, const void*)>(0x9B4650)(*reinterpret_cast<const uint32_t*>(ebx + 0x44), 0, r42, r9, *reinterpret_cast<const uint32_t*>(ebx + 0x2c), r8);
    alignas(16) uint32_t r43[4];  // 00A4F225
    reinterpret_cast<void*(__fastcall*)(uint32_t, uint32_t, void*, const void*, uint32_t, const void*)>(0x9B4650)(*reinterpret_cast<const uint32_t*>(ebx + 0x48), 0, r43, r7, *reinterpret_cast<const uint32_t*>(ebx + 0x2c), r8);
    alignas(16) uint32_t r44[4];  // 00A4F23F
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r44, r42, reinterpret_cast<const uint32_t*>(ebp - 0x2f0));
    alignas(16) uint32_t r45[4];  // 00A4F2AF
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r45, r44, r43);
    alignas(16) uint32_t r46[4];  // 00A4F31F
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984B60)(r46, reinterpret_cast<const uint32_t*>(ebp - 0x330), r45);
    alignas(16) uint32_t r47[4];  // 00A4F38F
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r47, r46, r46);
    alignas(16) uint32_t r48[4];  // 00A4F406
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x9B2EC0)(r48, r39, r40, r41);
    alignas(16) uint32_t r49[4];  // 00A4F46F
    reinterpret_cast<void*(__cdecl*)(void*, const void*)>(0x9B5CE0)(r49, r48);
    alignas(16) uint32_t r50[4];  // 00A4F4D8
    reinterpret_cast<void*(__cdecl*)(void*, const void*)>(0x985500)(r50, r49);
    alignas(16) uint32_t r51[4];  // 00A4F541
    reinterpret_cast<void*(__cdecl*)(void*, const void*)>(0xA520C0)(r51, r49);
    alignas(16) uint32_t r52[4];  // 00A4F5B1
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0xA51E80)(r52, r50, r51);
    alignas(16) uint32_t r53[4];  // 00A4F621
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0xA51F40)(r53, r48, r47);
    alignas(16) uint32_t r54[4];  // 00A4F691
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r54, r49, r52);
    alignas(16) uint32_t r55[4];  // 00A4F701
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r55, r39, r54);
    alignas(16) uint32_t r56[4];  // 00A4F771
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r56, r40, r54);
    alignas(16) uint32_t r57[4];  // 00A4F7E1
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x5537E0)(r57, r41, r54);
    alignas(16) uint32_t r58[4];  // 00A4F858
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r58, r55, r45, r39);
    alignas(16) uint32_t r59[4];  // 00A4F8CF
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r59, r56, r45, r40);
    alignas(16) uint32_t r60[4];  // 00A4F946
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*, const void*)>(0x553700)(r60, r57, r45, r41);
    alignas(16) uint32_t r61[4];  // 00A4F9B6
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r61, r58, r53);
    alignas(16) uint32_t r62[4];  // 00A4FA26
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r62, r59, r53);
    alignas(16) uint32_t r63[4];  // 00A4FA96
    reinterpret_cast<void*(__cdecl*)(void*, const void*, const void*)>(0x984F60)(r63, r60, r53);
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x8)) + off, r13, 16);  // 00A4FB03
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0xc)) + off, r14, 16);  // 00A4FB1D
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x10)) + off, r15, 16);  // 00A4FB37
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x14)) + off, r61, 16);  // 00A4FB51
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x18)) + off, r62, 16);  // 00A4FB6B
    std::memcpy(reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(ebx + 0x1c)) + off, r63, 16);  // 00A4FB85
}

volatile long g_a4_calls = 0, g_a4_iters = 0;

uint32_t __cdecl ffx_update_loop(uint8_t* ebp, const uint8_t* ebx) {
    const uint32_t count = *reinterpret_cast<const uint32_t*>(ebx + 0x20);
    ++g_a4_calls;
    g_a4_iters += count;
    uint32_t off = count << 4;
    if (count < 2) return off;
    while (off > 16) {
        off -= 16;
        if (*reinterpret_cast<const uint32_t*>(ebx + 0x4C)) {
            a4_iter_full(ebp, ebx, off);
        } else {
            a4_iter_skip(ebp, ebx, off);
        }
    }
    return 16;
}

// Entered by a jump at 0xA4DEE8 (dl = [ebx+0x28] != 0): the game's code from there to the loop (the array at
// [ebx+0x28] defaults to the one at [ebx+8]), the passes but the last, the counter, eax and ecx as the game's code
// has them, and on to the loop's top at 0xA4DF22.
__declspec(naked) void ffx_update_loop_entry() {
    __asm {
        mov byte ptr [ebp - 0x729], dl
        movzx eax, byte ptr [ebp - 0x729]
        test eax, eax
        je no_second
        mov ecx, dword ptr [ebx + 0x28]
        mov dword ptr [ebp - 0x18C0], ecx
        jmp have_second
    no_second:
        mov edx, dword ptr [ebx + 8]
        mov dword ptr [ebp - 0x18C0], edx
    have_second:
        mov eax, dword ptr [ebp - 0x18C0]
        mov dword ptr [ebx + 0x28], eax
        push ebx
        push ebp
        call ffx_update_loop
        add esp, 8
        mov dword ptr [ebp - 0x730], eax
        mov eax, dword ptr [ebp - 0x18C0]
        mov ecx, dword ptr [ebx + 0x20]
        shl ecx, 4
        push 0xA4DF22
        ret
    }
}

// The runtime's x87 square root _CIsqrt (0xAC1800: argument and result in st(0)), behind the engine's sqrtf (0x419980)
// and called directly by much of the engine (bounds, physics, the effects' eased curves), on both threads. It stores
// the argument as a double to classify it; for a positive finite one it loads the control word 0x7F | (the thread's
// precision bits) (round to nearest, every exception masked) unless the word is 0x27F, and runs fsqrt. On the way out,
// with a word other than 0x27F and the precision exception masked, a set precision flag (sticky: in practice always
// set) sends it through the runtime's floating-point error handling (__startOneArgErrorHandling, __87except: for an
// inexact result the value stored as a double and loaded back, the word restored, errno untouched) - on Direct3D's
// render thread (word 0x07F) for every square root. With the thread's word of the form 0x7F | precision, the same
// here directly: fsqrt, and for a word other than 0x27F with the flag set the store to a double and the load back.
// Zero, negative (the domain error: errno and the runtime's handling), infinite and NaN arguments, and any other
// control word, go on in the original code, entered after its first two instructions (done the same way here).
__declspec(naked) void crt_cisqrt() {
    __asm {
        sub esp, 12
        fst qword ptr [esp]
        mov eax, [esp + 4]
        test eax, eax
        js slow
        cmp eax, 0x7FF00000
        jae slow
        or eax, [esp]
        jz slow
        fnstcw word ptr [esp + 8]
        movzx eax, word ptr [esp + 8]
        and eax, 0xCFF
        cmp eax, 0x7F
        jne slow
        fsqrt
        cmp word ptr [esp + 8], 0x27F
        je done
        fnstsw ax
        test al, 0x20
        jz done
        fstp qword ptr [esp]
        fld qword ptr [esp]
    done:
        add esp, 12
        ret
    slow:
        mov eax, 0xAC1806
        jmp eax
    }
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
    // optional: FNV-1a of hash_size bytes at address + hash_at (long debug-build bodies)
    uint32_t hash = 0;
    size_t hash_at = 0;
    size_t hash_size = 0;
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
// 0x41FB20 whole (its calls and the dirty-mark call included)
const uint8_t kMatSetHead[] = {0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x55, 0x8B, 0x6C, 0x24, 0x10, 0x56, 0x57, 0x8B, 0xF8, 0x8D, 0x44, 0x7F, 0x06, 0xC1, 0xE0, 0x04, 0x8D, 0x34, 0x28, 0x56, 0x8B, 0xC3, 0xE8, 0x0F, 0x84, 0xFE, 0xFF};
const uint8_t kMatSetRest[] = {0x84, 0xC0, 0x75, 0x20, 0x8D, 0x4E, 0x10, 0x8D, 0x43, 0x10, 0x51, 0xE8, 0xFF, 0x83, 0xFE, 0xFF, 0x84, 0xC0, 0x75, 0x10, 0x8D, 0x56, 0x20, 0x8D, 0x43, 0x20, 0x52, 0xE8, 0xEF, 0x83, 0xFE, 0xFF, 0x84, 0xC0, 0x74, 0x4B, 0xF3, 0x0F, 0x7E, 0x03, 0x66, 0x0F, 0xD6, 0x06, 0xF3, 0x0F, 0x7E, 0x43, 0x08, 0x66, 0x0F, 0xD6, 0x46, 0x08, 0xF3, 0x0F, 0x7E, 0x43, 0x10, 0x66, 0x0F, 0xD6, 0x46, 0x10, 0xF3, 0x0F, 0x7E, 0x43, 0x18, 0x66, 0x0F, 0xD6, 0x46, 0x18, 0xF3, 0x0F, 0x7E, 0x43, 0x20, 0x8D, 0x43, 0x20, 0x8D, 0x4E, 0x20, 0x66, 0x0F, 0xD6, 0x01, 0xF3, 0x0F, 0x7E, 0x40, 0x08, 0x66, 0x0F, 0xD6, 0x41, 0x08, 0x83, 0x7D, 0x10, 0x00, 0x74, 0x06, 0x55, 0xE8, 0x00, 0x57, 0xB4, 0x00, 0x5F, 0x5E, 0x5D, 0x5B, 0x59, 0xC2, 0x08, 0x00};
// 0x422110 and 0x4221B0 whole, as 0x41FB20 (their call offsets and the row offset 0x78 of the second differ)
const uint8_t kMatSetBHead[] = {0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x55, 0x8B, 0x6C, 0x24, 0x10, 0x56, 0x57, 0x8B, 0xF8, 0x8D, 0x44, 0x7F, 0x06, 0xC1, 0xE0, 0x04, 0x8D, 0x34, 0x28, 0x56, 0x8B, 0xC3, 0xE8, 0x1F, 0x5E, 0xFE, 0xFF};
const uint8_t kMatSetBRest[] = {0x84, 0xC0, 0x75, 0x20, 0x8D, 0x4E, 0x10, 0x8D, 0x43, 0x10, 0x51, 0xE8, 0x0F, 0x5E, 0xFE, 0xFF, 0x84, 0xC0, 0x75, 0x10, 0x8D, 0x56, 0x20, 0x8D, 0x43, 0x20, 0x52, 0xE8, 0xFF, 0x5D, 0xFE, 0xFF, 0x84, 0xC0, 0x74, 0x4B, 0xF3, 0x0F, 0x7E, 0x03, 0x66, 0x0F, 0xD6, 0x06, 0xF3, 0x0F, 0x7E, 0x43, 0x08, 0x66, 0x0F, 0xD6, 0x46, 0x08, 0xF3, 0x0F, 0x7E, 0x43, 0x10, 0x66, 0x0F, 0xD6, 0x46, 0x10, 0xF3, 0x0F, 0x7E, 0x43, 0x18, 0x66, 0x0F, 0xD6, 0x46, 0x18, 0xF3, 0x0F, 0x7E, 0x43, 0x20, 0x8D, 0x43, 0x20, 0x8D, 0x4E, 0x20, 0x66, 0x0F, 0xD6, 0x01, 0xF3, 0x0F, 0x7E, 0x40, 0x08, 0x66, 0x0F, 0xD6, 0x41, 0x08, 0x83, 0x7D, 0x10, 0x00, 0x74, 0x06, 0x55, 0xE8, 0xA0, 0x34, 0xB4, 0x00, 0x5F, 0x5E, 0x5D, 0x5B, 0x59, 0xC2, 0x08, 0x00};
const uint8_t kMatSetCHead[] = {0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x55, 0x8B, 0x6C, 0x24, 0x10, 0x56, 0x57, 0x8B, 0xF8, 0x8D, 0x44, 0x7F, 0x78, 0xC1, 0xE0, 0x04, 0x8D, 0x34, 0x28, 0x56, 0x8B, 0xC3, 0xE8, 0x7F, 0x5D, 0xFE, 0xFF};
const uint8_t kMatSetCRest[] = {0x84, 0xC0, 0x75, 0x20, 0x8D, 0x4E, 0x10, 0x8D, 0x43, 0x10, 0x51, 0xE8, 0x6F, 0x5D, 0xFE, 0xFF, 0x84, 0xC0, 0x75, 0x10, 0x8D, 0x56, 0x20, 0x8D, 0x43, 0x20, 0x52, 0xE8, 0x5F, 0x5D, 0xFE, 0xFF, 0x84, 0xC0, 0x74, 0x4B, 0xF3, 0x0F, 0x7E, 0x03, 0x66, 0x0F, 0xD6, 0x06, 0xF3, 0x0F, 0x7E, 0x43, 0x08, 0x66, 0x0F, 0xD6, 0x46, 0x08, 0xF3, 0x0F, 0x7E, 0x43, 0x10, 0x66, 0x0F, 0xD6, 0x46, 0x10, 0xF3, 0x0F, 0x7E, 0x43, 0x18, 0x66, 0x0F, 0xD6, 0x46, 0x18, 0xF3, 0x0F, 0x7E, 0x43, 0x20, 0x8D, 0x43, 0x20, 0x8D, 0x4E, 0x20, 0x66, 0x0F, 0xD6, 0x01, 0xF3, 0x0F, 0x7E, 0x40, 0x08, 0x66, 0x0F, 0xD6, 0x41, 0x08, 0x83, 0x7D, 0x10, 0x00, 0x74, 0x06, 0x55, 0xE8, 0xB0, 0x33, 0xB4, 0x00, 0x5F, 0x5E, 0x5D, 0x5B, 0x59, 0xC2, 0x08, 0x00};
// The three setters these call (0x425670 dirty mark, then memcpy), checked so the inline copy matches what they do
const uint8_t kBoneSetter8[] = {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x56, 0x8B, 0x75, 0x10, 0x6A, 0x03, 0x8D, 0x5C, 0x7F, 0x08, 0x83, 0xC6, 0x10, 0x56, 0x8B, 0xC3};
const uint8_t kBoneSetter8Tail[] = {0x8D, 0x44, 0x7F, 0x06, 0xC1, 0xE0, 0x04, 0x83, 0xC3, 0x04, 0x6A, 0x30, 0x03, 0xC5, 0xC1, 0xE3, 0x04, 0x50, 0x03, 0xDE, 0x53};
const uint8_t kBoneSetter128[] = {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x56, 0x8B, 0x75, 0x10, 0x6A, 0x03, 0x8D, 0x9C, 0x7F, 0x80, 0x00, 0x00, 0x00, 0x83, 0xC6, 0x10, 0x56, 0x8B, 0xC3};
const uint8_t kBoneSetter128Tail[] = {0x8D, 0x44, 0x7F, 0x78, 0xC1, 0xE0, 0x04, 0x83, 0xC3, 0x04, 0x6A, 0x30, 0x03, 0xC5, 0xC1, 0xE3, 0x04, 0x50, 0x03, 0xDE, 0x53};
// 0x431C30 and 0x431D40 whole (no calls but through the device's vtable; they differ in its offset)
const uint8_t kFlushVs[] = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x28, 0x33, 0xD2, 0x8D, 0x47, 0x20, 0x33, 0xDB, 0x8B, 0xCF, 0x2B, 0xC7, 0x89, 0x54, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x14, 0x89, 0x44, 0x24, 0x18, 0xEB, 0x04, 0x8B, 0x44, 0x24, 0x18, 0x8B, 0x31, 0x8B, 0x04, 0x08, 0x8B, 0xEE, 0x23, 0xE8, 0x89, 0x6C, 0x24, 0x1C, 0x74, 0x6A, 0x8B, 0x7C, 0x24, 0x10, 0xF7, 0xD0, 0x23, 0xC6, 0xC1, 0xE7, 0x05, 0x8B, 0xF3, 0x89, 0x7C, 0x24, 0x20, 0x33, 0xED, 0xC1, 0xE6, 0x04, 0x89, 0x01, 0xC1, 0xE7, 0x04, 0x8B, 0xCD, 0xB8, 0x00, 0x00, 0x00, 0x80, 0xD3, 0xE8, 0x8B, 0x4C, 0x24, 0x1C, 0x85, 0xC1, 0x74, 0x10, 0x85, 0xD2, 0x75, 0x09, 0x8B, 0x44, 0x24, 0x20, 0x8D, 0x1C, 0x28, 0x8B, 0xF7, 0x42, 0xEB, 0x23, 0x85, 0xD2, 0x74, 0x1F, 0x8B, 0x4C, 0x24, 0x2C, 0x8B, 0x41, 0x10, 0x8B, 0x08, 0x52, 0x8B, 0x54, 0x24, 0x2C, 0x8D, 0x54, 0x16, 0x40, 0x52, 0x53, 0x50, 0x8B, 0x81, 0x78, 0x01, 0x00, 0x00, 0xFF, 0xD0, 0x33, 0xD2, 0x45, 0x83, 0xC7, 0x10, 0x83, 0xFD, 0x20, 0x7C, 0xB3, 0xEB, 0x23, 0x85, 0xD2, 0x74, 0x27, 0x8B, 0x4C, 0x24, 0x2C, 0x8B, 0x41, 0x10, 0x8B, 0x08, 0x52, 0x8D, 0x53, 0x04, 0xC1, 0xE2, 0x04, 0x03, 0xD7, 0x52, 0x53, 0x50, 0x8B, 0x81, 0x78, 0x01, 0x00, 0x00, 0xFF, 0xD0, 0x33, 0xD2, 0x8B, 0x7C, 0x24, 0x28, 0x8B, 0x4C, 0x24, 0x14, 0x8B, 0x44, 0x24, 0x10, 0x40, 0x83, 0xC1, 0x04, 0x83, 0xF8, 0x08, 0x89, 0x44, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x14, 0x0F, 0x8C, 0x3F, 0xFF, 0xFF, 0xFF, 0x85, 0xD2, 0x74, 0x1D, 0x8B, 0x4C, 0x24, 0x2C, 0x8B, 0x41, 0x10, 0x8B, 0x08, 0x52, 0x8D, 0x53, 0x04, 0xC1, 0xE2, 0x04, 0x03, 0xD7, 0x52, 0x53, 0x50, 0x8B, 0x81, 0x78, 0x01, 0x00, 0x00, 0xFF, 0xD0, 0x5F, 0x5E, 0x5D, 0x5B, 0x83, 0xC4, 0x14, 0xC2, 0x08, 0x00};
const uint8_t kFlushPs[] = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x28, 0x33, 0xD2, 0x8D, 0x47, 0x20, 0x33, 0xDB, 0x8B, 0xCF, 0x2B, 0xC7, 0x89, 0x54, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x14, 0x89, 0x44, 0x24, 0x18, 0xEB, 0x04, 0x8B, 0x44, 0x24, 0x18, 0x8B, 0x31, 0x8B, 0x04, 0x08, 0x8B, 0xEE, 0x23, 0xE8, 0x89, 0x6C, 0x24, 0x1C, 0x74, 0x6A, 0x8B, 0x7C, 0x24, 0x10, 0xF7, 0xD0, 0x23, 0xC6, 0xC1, 0xE7, 0x05, 0x8B, 0xF3, 0x89, 0x7C, 0x24, 0x20, 0x33, 0xED, 0xC1, 0xE6, 0x04, 0x89, 0x01, 0xC1, 0xE7, 0x04, 0x8B, 0xCD, 0xB8, 0x00, 0x00, 0x00, 0x80, 0xD3, 0xE8, 0x8B, 0x4C, 0x24, 0x1C, 0x85, 0xC1, 0x74, 0x10, 0x85, 0xD2, 0x75, 0x09, 0x8B, 0x44, 0x24, 0x20, 0x8D, 0x1C, 0x28, 0x8B, 0xF7, 0x42, 0xEB, 0x23, 0x85, 0xD2, 0x74, 0x1F, 0x8B, 0x4C, 0x24, 0x2C, 0x8B, 0x41, 0x10, 0x8B, 0x08, 0x52, 0x8B, 0x54, 0x24, 0x2C, 0x8D, 0x54, 0x16, 0x40, 0x52, 0x53, 0x50, 0x8B, 0x81, 0xB4, 0x01, 0x00, 0x00, 0xFF, 0xD0, 0x33, 0xD2, 0x45, 0x83, 0xC7, 0x10, 0x83, 0xFD, 0x20, 0x7C, 0xB3, 0xEB, 0x23, 0x85, 0xD2, 0x74, 0x27, 0x8B, 0x4C, 0x24, 0x2C, 0x8B, 0x41, 0x10, 0x8B, 0x08, 0x52, 0x8D, 0x53, 0x04, 0xC1, 0xE2, 0x04, 0x03, 0xD7, 0x52, 0x53, 0x50, 0x8B, 0x81, 0xB4, 0x01, 0x00, 0x00, 0xFF, 0xD0, 0x33, 0xD2, 0x8B, 0x7C, 0x24, 0x28, 0x8B, 0x4C, 0x24, 0x14, 0x8B, 0x44, 0x24, 0x10, 0x40, 0x83, 0xC1, 0x04, 0x83, 0xF8, 0x08, 0x89, 0x44, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x14, 0x0F, 0x8C, 0x3F, 0xFF, 0xFF, 0xFF, 0x85, 0xD2, 0x74, 0x1D, 0x8B, 0x4C, 0x24, 0x2C, 0x8B, 0x41, 0x10, 0x8B, 0x08, 0x52, 0x8D, 0x53, 0x04, 0xC1, 0xE2, 0x04, 0x03, 0xD7, 0x52, 0x53, 0x50, 0x8B, 0x81, 0xB4, 0x01, 0x00, 0x00, 0xFF, 0xD0, 0x5F, 0x5E, 0x5D, 0x5B, 0x83, 0xC4, 0x14, 0xC2, 0x08, 0x00};
// 0x982C30 and 0x9817B0 up to their debug fill's count (FastEffects zeroes it); the rest by hash
const uint8_t kCurve4Head[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC, 0xC0, 0x03, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0x40, 0xFC, 0xFF, 0xFF, 0xB9};
const uint8_t kCurve1Head[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0xFC, 0xFE, 0xFF, 0xFF, 0xB9};
// 0x9AF7F0, 0x9B5DC0, 0x9B18A0 and 0x9B4C60 up to their debug fill's count; the rest by hash
const uint8_t kMatMaddHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0x70, 0xFE, 0xFF, 0xFF, 0xB9};
const uint8_t kSinCosHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC, 0x10, 0x04, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0xF0, 0xFB, 0xFF, 0xFF, 0xB9};
const uint8_t kPackHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC, 0x90, 0x05, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0x70, 0xFA, 0xFF, 0xFF, 0xB9};
const uint8_t kRowsHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x36, 0x2B, 0x04, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x53, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0xC0, 0xFE, 0xFF, 0xFF, 0xB9};
// 0x446D30's first 16 bytes; the rest by hash
const uint8_t kAabbHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x8B, 0x45, 0x0C, 0x8B};
// 0x994AE0 up to its debug fill's count; the rest by hash
const uint8_t kArraysHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC, 0x30, 0x02, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0xD0, 0xFD, 0xFF, 0xFF, 0xB9};
// 0x995C60 up to its debug fill's count; the rest by hash
const uint8_t kBoundsHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xFC, 0x2A, 0x04, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x53, 0x81, 0xEC, 0x40, 0x07, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0xB0, 0xF8, 0xFF, 0xFF, 0xB9};
// 0x99C540 up to its debug fill's count (the rest by hash), and its particle loop's start
const uint8_t kQuadsHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0xB8, 0xC8, 0x1F, 0x00, 0x00, 0xE8, 0x90, 0x54, 0x12, 0x00, 0x56, 0x57, 0x8D, 0xBD, 0x38, 0xE0, 0xFF, 0xFF, 0xB9};
const uint8_t kQuadLoop[] = {0xC7, 0x85, 0x7C, 0xEB, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x0F};
// 0xA4D420 up to its debug fill's count (the rest by hash), and the code from 0xA4DEE8 to its particle loop
const uint8_t kUpdateHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x83, 0x80, 0x04, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x53, 0xB8, 0xB8, 0x18, 0x00, 0x00, 0xE8, 0xA1, 0x45, 0x07, 0x00, 0x56, 0x57, 0x8D, 0xBD, 0x38, 0xE7, 0xFF, 0xFF, 0xB9};
const uint8_t kUpdatePreLoop[] = {0x88, 0x95, 0xD7, 0xF8, 0xFF, 0xFF, 0x0F, 0xB6, 0x85, 0xD7, 0xF8, 0xFF, 0xFF, 0x85, 0xC0, 0x74, 0x0B, 0x8B, 0x4B, 0x28, 0x89, 0x8D, 0x40, 0xE7, 0xFF, 0xFF, 0xEB, 0x09, 0x8B, 0x53, 0x08, 0x89, 0x95, 0x40, 0xE7, 0xFF, 0xFF, 0x8B, 0x85, 0x40, 0xE7, 0xFF, 0xFF, 0x89, 0x43, 0x28, 0x8B, 0x4B, 0x20, 0xC1, 0xE1, 0x04, 0x89, 0x8D, 0xD0, 0xF8, 0xFF, 0xFF};
// _CIsqrt (0xAC1800) and the start of its body (the rest by hash)
const uint8_t kCiSqrtHead[] = {0x83, 0xEC, 0x0C, 0xDD, 0x14, 0x24, 0xE8, 0x2D, 0xE3, 0x00, 0x00, 0xE8, 0x0D, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x0C, 0xC3, 0x8D, 0x54, 0x24, 0x04, 0xE8, 0xD8, 0xE2, 0x00, 0x00, 0x52, 0x9B, 0xD9};
// 0x419890 through its constants (-0.0f at 0x11E7FF0, 1.0 at 0x11E7C90) to the loop's first load
const uint8_t kRsqrtHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x55, 0x0C, 0x0F, 0x57, 0xC9, 0xF3, 0x0F, 0x10,
                              0x15, 0xF0, 0x7F, 0x1E, 0x01, 0xF2, 0x0F, 0x10, 0x1D, 0x90, 0x7C, 0x1E, 0x01, 0x8D, 0x44,
                              0x24, 0xF0, 0x83, 0xEC, 0x20, 0x33, 0xC9, 0x2B, 0xD0};
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
    {"four-float reciprocal square root", 0x419890, kRsqrtHead, sizeof(kRsqrtHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&vec4_rsqrt)},
    {"box transform", 0x446D30, kAabbHead, sizeof(kAabbHead), nullptr, 0, 0, reinterpret_cast<const void*>(&aabb_transform),
     0x7ECAA53Cu, 16, 3495},
    {"runtime square root", 0xAC1800, kCiSqrtHead, sizeof(kCiSqrtHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&crt_cisqrt), 0x1AB0EB6Eu, 0x20, 154},
};

// Bone palettes (table 3, installed with the core sites; RemoteControl `fastmath bone 0|1`): 0x41FB20 and its two
// siblings with their setter calls inlined (see mat_set_core), and the flushes that pass the dirty registers on to the
// device (see flush_core). The setters are only checked here (their bytes must be
// what mat_set_core does inline); they are not patched themselves.
const Site kBoneSites[] = {
    {"set matrix if changed", 0x41FB20, kMatSetHead, sizeof(kMatSetHead), kMatSetRest, 0x21, sizeof(kMatSetRest),
     reinterpret_cast<const void*>(&mat_set_if_changed)},
    {"bone setter c8", 0xF652B0, kBoneSetter8, sizeof(kBoneSetter8), kBoneSetter8Tail, 0x1B, sizeof(kBoneSetter8Tail),
     nullptr},
    {"bone setter c8 (second)", 0xF65640, kBoneSetter8, sizeof(kBoneSetter8), kBoneSetter8Tail, 0x1B,
     sizeof(kBoneSetter8Tail), nullptr},
    {"bone setter c128", 0xF655F0, kBoneSetter128, sizeof(kBoneSetter128), kBoneSetter128Tail, 0x1E,
     sizeof(kBoneSetter128Tail), nullptr},
    {"set matrix if changed (second)", 0x422110, kMatSetBHead, sizeof(kMatSetBHead), kMatSetBRest, 0x21,
     sizeof(kMatSetBRest), reinterpret_cast<const void*>(&mat_set_if_changed)},
    {"set matrix if changed (c128)", 0x4221B0, kMatSetCHead, sizeof(kMatSetCHead), kMatSetCRest, 0x21,
     sizeof(kMatSetCRest), reinterpret_cast<const void*>(&mat_set_if_changed_4221B0)},
    {"vertex shader constant flush", 0x431C30, kFlushVs, sizeof(kFlushVs), nullptr, 0, 0,
     reinterpret_cast<const void*>(&flush_vs_constants)},
    {"pixel shader constant flush", 0x431D40, kFlushPs, sizeof(kFlushPs), nullptr, 0, 0,
     reinterpret_cast<const void*>(&flush_ps_constants)},
};


// The effects library's integer vector operations (ported console vector code), cdecl (out, a, b), each lane read
// through a one-lane getter (0x51FF80 x, 0x402BC0 y, 0xB0E260 z, 0x403B00 w), the four results built into a temporary
// (by the set helper 0x984890, or through lane-address getters) and then copied to out, out returned:
//   0x9852A0 shift left, 0x985430 shift right (logical): a << (b & 31), a >> (b & 31) as x86 shl / shr take counts;
//   0x985020 and-not: a & ~b.
// Here the four lanes directly (all read before any is written, as there).
uint32_t* __cdecl ffx_shl4(uint32_t* out, const uint32_t* a, const uint32_t* b) {
    const uint32_t r0 = a[0] << (b[0] & 31), r1 = a[1] << (b[1] & 31), r2 = a[2] << (b[2] & 31), r3 = a[3] << (b[3] & 31);
    out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3;
    return out;
}
uint32_t* __cdecl ffx_shr4(uint32_t* out, const uint32_t* a, const uint32_t* b) {
    const uint32_t r0 = a[0] >> (b[0] & 31), r1 = a[1] >> (b[1] & 31), r2 = a[2] >> (b[2] & 31), r3 = a[3] >> (b[3] & 31);
    out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3;
    return out;
}
uint32_t* __cdecl ffx_andnot4(uint32_t* out, const uint32_t* a, const uint32_t* b) {
    const __m128i r = _mm_andnot_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b)),
                                       _mm_loadu_si128(reinterpret_cast<const __m128i*>(a)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), r);
    return out;
}
const uint8_t kFfxIntHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0x56, 0x57, 0x8D, 0x7D};  // through lea edi (the 0xCC fill after it is FastEffects' to patch)
const uint8_t kShl4Body[] = {0x8B, 0x4B, 0x0C, 0xE8, 0x2E, 0xE8, 0xA7, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0x24, 0xE8, 0xA7, 0xFF, 0x8B, 0xC8, 0xD3, 0xE6, 0x56, 0x8B, 0x4B, 0x0C, 0xE8, 0x77, 0x8F, 0x18, 0x00, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0x6D, 0x8F, 0x18, 0x00, 0x8B, 0xC8, 0xD3, 0xE6, 0x56, 0x8B, 0x4B, 0x0C, 0xE8, 0xC0, 0xD8, 0xA7, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0xB6, 0xD8, 0xA7, 0xFF, 0x8B, 0xC8, 0xD3, 0xE6, 0x56, 0x8B, 0x4B, 0x0C, 0xE8, 0x69, 0xAC, 0xB9, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0x5F, 0xAC, 0xB9, 0xFF, 0x8B, 0xC8, 0xD3, 0xE6, 0x56, 0x8D, 0x45, 0xE0, 0x50, 0xE8, 0x61, 0xF5, 0xFF, 0xFF};
const uint8_t kShr4Body[] = {0x8B, 0x4B, 0x0C, 0xE8, 0x9E, 0xE6, 0xA7, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0x94, 0xE6, 0xA7, 0xFF, 0x8B, 0xC8, 0xD3, 0xEE, 0x56, 0x8B, 0x4B, 0x0C, 0xE8, 0xE7, 0x8D, 0x18, 0x00, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0xDD, 0x8D, 0x18, 0x00, 0x8B, 0xC8, 0xD3, 0xEE, 0x56, 0x8B, 0x4B, 0x0C, 0xE8, 0x30, 0xD7, 0xA7, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0x26, 0xD7, 0xA7, 0xFF, 0x8B, 0xC8, 0xD3, 0xEE, 0x56, 0x8B, 0x4B, 0x0C, 0xE8, 0xD9, 0xAA, 0xB9, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0xCF, 0xAA, 0xB9, 0xFF, 0x8B, 0xC8, 0xD3, 0xEE, 0x56, 0x8D, 0x45, 0xE0, 0x50, 0xE8, 0xD1, 0xF3, 0xFF, 0xFF};
const uint8_t kAndNot4Body[] = {0x8B, 0x4B, 0x0C, 0xE8, 0x2E, 0xAF, 0xB9, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0x24, 0xAF, 0xB9, 0xFF, 0xF7, 0xD0, 0x23, 0xF0, 0x8D, 0x4D, 0xE0, 0xE8, 0xF8, 0xCE, 0xB5, 0xFF, 0x89, 0x30, 0x8B, 0x4B, 0x0C, 0xE8, 0x4E, 0xDB, 0xA7, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0x44, 0xDB, 0xA7, 0xFF, 0xF7, 0xD0, 0x23, 0xF0, 0x8D, 0x4D, 0xE0, 0xE8, 0x98, 0x47, 0xD5, 0xFF, 0x89, 0x30, 0x8B, 0x4B, 0x0C, 0xE8, 0xCE, 0x91, 0x18, 0x00, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0xC4, 0x91, 0x18, 0x00, 0xF7, 0xD0, 0x23, 0xF0, 0x8D, 0x4D, 0xE0, 0xE8, 0x68, 0xE1, 0xAD, 0xFF, 0x89, 0x30, 0x8B, 0x4B, 0x0C, 0xE8, 0x4E, 0xEA, 0xA7, 0xFF, 0x8B, 0xF0, 0x8B, 0x4B, 0x10, 0xE8, 0x44, 0xEA, 0xA7, 0xFF, 0xF7, 0xD0, 0x23, 0xF0, 0x8D, 0x4D, 0xE0, 0xE8, 0x28, 0xEA, 0xA7, 0xFF, 0x89, 0x30};


// The effects library's random numbers (particle spawning), three nested debug functions built from the helpers above
// through stack temporaries:
//   0x983F90 (out, state): xorshift32 on each lane of the state (x ^= x << 13; x ^= x >> 17; x ^= x << 5, through
//     the splat, shift and xor wrappers); the state and out both get the new value (state first).
//   0x983D30 (out, state): a float in [0, 1) per lane: the mantissa bits of a new 0x983F90 value put into 1.0 (the
//     select helper with 0x7FFFFF), then 1.0 subtracted (exact).
//   0x9824F0 (out, scale, -, range, -, state): scale * madd(random, splat(range->hi), splat(range->lo)), range a
//     pointer to a pointer to {lo, hi}; the splats load through the x87 stack, the multiply-add rounds once from double
//     and the multiply rounds the float product, as the replacements above do (the same functions are called here).
// Here the same steps in the same order, without the temporaries and debug frames.
uint32_t* __cdecl ffx_xorshift4(uint32_t* out, uint32_t* state) {
    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state));
    x = _mm_xor_si128(x, _mm_slli_epi32(x, 13));
    x = _mm_xor_si128(x, _mm_srli_epi32(x, 17));
    x = _mm_xor_si128(x, _mm_slli_epi32(x, 5));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), x);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), x);
    return out;
}
float* __cdecl ffx_rand01(float* out, uint32_t* state) {
    alignas(16) uint32_t bits[4];
    ffx_xorshift4(bits, state);
    const __m128i m = _mm_or_si128(_mm_and_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(bits)),
                                                 _mm_set1_epi32(0x007FFFFF)),
                                   _mm_set1_epi32(0x3F800000));
    _mm_storeu_ps(out, _mm_sub_ps(_mm_castsi128_ps(m), _mm_set1_ps(1.0f)));
    return out;
}
float* __cdecl ffx_rand_range(float* out, float* scale, uint32_t, const uint32_t* const* range, uint32_t,
                              uint32_t* state) {
    const uint32_t* lohi = *range;
    alignas(16) float lo[4], hi[4], r[4], d[4];
    ffx_splat_x87(lo, *reinterpret_cast<const float*>(&lohi[0]));
    ffx_splat_x87(hi, *reinterpret_cast<const float*>(&lohi[1]));
    ffx_rand01(r, state);
    vec4_madd(d, r, hi, lo);
    alignas(16) float e[4];
    vec4_mul(e, scale, d);
    std::memcpy(out, e, 16);
    return out;
}
const uint8_t kXorshiftHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC, 0xD0, 0x02, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0x30, 0xFD, 0xFF, 0xFF};
const uint8_t kXorshiftBody[] = {0x6A, 0x05, 0x8D, 0x85, 0xB0, 0xFD, 0xFF, 0xFF, 0x50, 0xE8, 0xF3, 0x07, 0x00, 0x00, 0x83, 0xC4, 0x08, 0x8B, 0x08, 0x89, 0x4D, 0xC0, 0x8B, 0x50, 0x04, 0x89, 0x55, 0xC4, 0x8B, 0x48, 0x08, 0x89, 0x4D, 0xC8, 0x8B, 0x50, 0x0C, 0x89, 0x55, 0xCC, 0x8B, 0x45, 0xC0, 0x89, 0x45, 0xE0, 0x8B, 0x4D, 0xC4, 0x89, 0x4D, 0xE4, 0x8B, 0x55, 0xC8, 0x89, 0x55, 0xE8, 0x8B, 0x45, 0xCC, 0x89, 0x45, 0xEC, 0x6A, 0x0D, 0x8D, 0x8D, 0xA0, 0xFD, 0xFF, 0xFF, 0x51, 0xE8, 0xB3, 0x07, 0x00, 0x00, 0x83, 0xC4, 0x08, 0x8B, 0x10, 0x89, 0x55, 0x80, 0x8B, 0x48, 0x04, 0x89, 0x4D, 0x84, 0x8B, 0x50, 0x08, 0x89};
const uint8_t kRand01Head[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0xA0, 0xFE, 0xFF, 0xFF};
const uint8_t kRand01Body[] = {0x68, 0xFF, 0xFF, 0x7F, 0x00, 0x8D, 0x85, 0xE0, 0xFE, 0xFF, 0xFF, 0x50, 0xE8, 0x50, 0x0A, 0x00, 0x00, 0x83, 0xC4, 0x08, 0x8B, 0x08, 0x89, 0x4D, 0xC0, 0x8B, 0x50, 0x04, 0x89, 0x55, 0xC4, 0x8B, 0x48, 0x08, 0x89, 0x4D, 0xC8, 0x8B, 0x50, 0x0C, 0x89, 0x55, 0xCC, 0x8B, 0x45, 0xC0, 0x89, 0x45, 0xE0, 0x8B, 0x4D, 0xC4, 0x89, 0x4D, 0xE4, 0x8B, 0x55, 0xC8, 0x89, 0x55, 0xE8, 0x8B, 0x45, 0xCC, 0x89, 0x45, 0xEC, 0x8D, 0x8D, 0xD0, 0xFE, 0xFF, 0xFF, 0x51, 0xE8, 0xF2, 0x0C, 0x00, 0x00, 0x83, 0xC4, 0x04, 0x8B, 0x10, 0x89, 0x55, 0x80, 0x8B, 0x48, 0x04, 0x89, 0x4D, 0x84, 0x8B, 0x50, 0x08};
const uint8_t kRandRangeHead[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00, 0x57, 0x8D, 0xBD, 0xA0, 0xFE, 0xFF, 0xFF};
const uint8_t kRandRangeBody[] = {0x8B, 0x43, 0x14, 0x8B, 0x08, 0x89, 0x4D, 0xFC, 0x8B, 0x55, 0xFC, 0xD9, 0x02, 0xD9, 0x5D, 0xF8, 0x8B, 0x45, 0xFC, 0xD9, 0x40, 0x04, 0xD9, 0x5D, 0xF4, 0x51, 0xD9, 0x45, 0xF8, 0xD9, 0x1C, 0x24, 0x8D, 0x8D, 0xE0, 0xFE, 0xFF, 0xFF, 0x51, 0xE8, 0xD5, 0x23, 0x00, 0x00, 0x83, 0xC4, 0x08, 0x8B, 0x10, 0x89, 0x55, 0xC0, 0x8B, 0x48, 0x04, 0x89, 0x4D, 0xC4, 0x8B, 0x50, 0x08, 0x89, 0x55, 0xC8, 0x8B, 0x40, 0x0C, 0x89, 0x45, 0xCC, 0x8B, 0x4D, 0xC0, 0x89, 0x4D, 0xE0, 0x8B, 0x55, 0xC4, 0x89, 0x55, 0xE4, 0x8B, 0x45, 0xC8, 0x89, 0x45, 0xE8, 0x8B, 0x4D, 0xCC, 0x89, 0x4D, 0xEC, 0x51, 0xD9};

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
    {"integer shift left", 0x9852A0, kFfxIntHead, sizeof(kFfxIntHead), kShl4Body, 0x2A, sizeof(kShl4Body),
     reinterpret_cast<const void*>(&ffx_shl4)},
    {"integer shift right", 0x985430, kFfxIntHead, sizeof(kFfxIntHead), kShr4Body, 0x2A, sizeof(kShr4Body),
     reinterpret_cast<const void*>(&ffx_shr4)},
    {"integer and-not", 0x985020, kFfxIntHead, sizeof(kFfxIntHead), kAndNot4Body, 0x2A, sizeof(kAndNot4Body),
     reinterpret_cast<const void*>(&ffx_andnot4)},
    {"random: xorshift4", 0x983F90, kXorshiftHead, sizeof(kXorshiftHead), kXorshiftBody, 0x2F, sizeof(kXorshiftBody),
     reinterpret_cast<const void*>(&ffx_xorshift4)},
    {"random: rand01", 0x983D30, kRand01Head, sizeof(kRand01Head), kRand01Body, 0x2F, sizeof(kRand01Body),
     reinterpret_cast<const void*>(&ffx_rand01)},
    {"random: rand_range", 0x9824F0, kRandRangeHead, sizeof(kRandRangeHead), kRandRangeBody, 0x2F, sizeof(kRandRangeBody),
     reinterpret_cast<const void*>(&ffx_rand_range)},
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
    {"keyframe curve (four lanes)", 0x982C30, kCurve4Head, sizeof(kCurve4Head), nullptr, 0, 0,
     reinterpret_cast<const void*>(&ffx_curve4), 0x266987A4u, 40, 1627},
    {"keyframe curve (one float)", 0x9817B0, kCurve1Head, sizeof(kCurve1Head), nullptr, 0, 0,
     reinterpret_cast<const void*>(&ffx_curve1), 0x4C113B3Fu, 21, 779},
    {"three-row multiply-add", 0x9AF7F0, kMatMaddHead, sizeof(kMatMaddHead), nullptr, 0, 0, reinterpret_cast<const void*>(&ffx_mat3_madd),
     0x0CC0CF07u, 40, 1011},
    {"sine and cosine polynomials", 0x9B5DC0, kSinCosHead, sizeof(kSinCosHead), nullptr, 0, 0, reinterpret_cast<const void*>(&ffx_sincos_poly),
     0x305BC54Eu, 40, 2011},
    {"colour packing", 0x9B18A0, kPackHead, sizeof(kPackHead), nullptr, 0, 0, reinterpret_cast<const void*>(&ffx_pack_colors),
     0x94FB03DFu, 40, 1911},
    {"transposed rows", 0x9B4C60, kRowsHead, sizeof(kRowsHead), nullptr, 0, 0, reinterpret_cast<const void*>(&ffx_rows_transposed),
     0xAC6ECBD8u, 55, 449},
    {"vector arrays through 0x44C6A0", 0x994AE0, kArraysHead, sizeof(kArraysHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&ffx_arrays_44c6a0), 0x073CE9FAu, 40, 1007},
    {"bounds of three arrays", 0x995C60, kBoundsHead, sizeof(kBoundsHead), nullptr, 0, 0,
     reinterpret_cast<const void*>(&ffx_bounds3), 0x15FD9D02u, 55, 3225},
    {"particle quad builder", 0x99C540, kQuadsHead, sizeof(kQuadsHead), nullptr, 0, 0, nullptr, 0x94826511u, 45, 11428},
    {"particle quad loop", 0x99E9B0, kQuadLoop, sizeof(kQuadLoop), nullptr, 0, 0,
     reinterpret_cast<const void*>(&ffx_particle_loop_entry)},
    {"particle update", 0xA4D420, kUpdateHead, sizeof(kUpdateHead), nullptr, 0, 0, nullptr, 0x79E35490u, 60, 10098},
    {"particle update loop", 0xA4DEE8, kUpdatePreLoop, sizeof(kUpdatePreLoop), nullptr, 0, 0,
     reinterpret_cast<const void*>(&ffx_update_loop_entry)},
};

bool matches(const Site& s) {
    if (std::memcmp(reinterpret_cast<const void*>(s.address), s.expect, s.expect_size) != 0) return false;
    if (s.hash_size) {
        uint32_t h = 0x811C9DC5u;
        const auto* p = reinterpret_cast<const uint8_t*>(s.address + s.hash_at);
        for (size_t i = 0; i < s.hash_size; ++i) h = (h ^ p[i]) * 0x01000193u;
        if (h != s.hash) return false;
    }
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
    int table;  // 0 core, 1 effects, 3 bone palettes
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
        if (!sites[i].replacement) continue;  // checked only
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
                     "multiply-add, not-equal, saturate and reciprocal square root, 3x4 matrix add and multiply, 4x4 "
                     "multiply and transpose, box transform, the runtime square root's common case) run with the "
                     "same results",
                     n);
        }
        const int nb = install_table("FastRenderMath", kBoneSites, sizeof(kBoneSites) / sizeof(kBoneSites[0]), 3);
        ok = ok && nb == 5;
        if (nb) {
            LOG_INFO("FastRenderMath: bone palettes: %d sites (the three set-matrix-if-changed routines compare in SSE "
                     "and write the shader constants and their dirty bits inline, the two shader constant flushes find "
                     "the dirty runs by bit scans) with the same results and device calls",
                     nb);
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
            LOG_INFO("FastEffects: %d of the effects library's routines (vector helpers, forwarding wrappers, the "
                     "keyframe curves, colour packing, the sine/cosine polynomial and other particle glue) replaced "
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

// RemoteControl `fastmath core|effects|new|bone 0|1`: every 8-aligned site of that table back to the original code or on again,
// each in one atomic 8-byte write (diagnostic A/B; a thread inside a site's first instructions at that moment would be
// cut, so this is for testing only). Returns how many sites were switched.
namespace {
// One site back to the original code (on false or no target) or to target, in one atomic 8-byte write.
bool switch_site(const Saved& sv, const void* target, bool on) {
    uint8_t b[8];
    std::memcpy(b, sv.orig, 8);
    if (on && target) {
        b[0] = 0xE9;
        const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (sv.at + 5));
        std::memcpy(b + 1, &rel, 4);
    }
    uint64_t want = 0, now = 0;
    std::memcpy(&want, b, 8);
    std::memcpy(&now, reinterpret_cast<const void*>(sv.at), 8);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(sv.at), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(sv.at), static_cast<LONG64>(want),
                                 static_cast<LONG64>(now));
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(sv.at), 8, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(sv.at), 8);
    return true;
}
}  // namespace

// RemoteControl `fastmath stats`: calls and particles through the particle quad loop since the last call.
void rendermath_stats(long* calls, long* particles, long* update_calls, long* update_groups) {
    *calls = g_ploop_calls;
    *particles = g_ploop_iters;
    *update_calls = g_a4_calls;
    *update_groups = g_a4_iters;
    g_ploop_calls = g_ploop_iters = g_a4_calls = g_a4_iters = 0;
}

// RemoteControl `fastmath at ADDR 0|1`: the one site at ADDR, as rendermath_set_table.
int rendermath_set_site(uintptr_t at, bool on) {
    int n = 0;
    for (int i = 0; i < g_saved_n; ++i) {
        if (g_saved[i].at == at && switch_site(g_saved[i], g_saved[i].target, on)) ++n;
    }
    return n;
}

int rendermath_set_table(int table, bool on) {
    int n = 0;
    for (int i = 0; i < g_saved_n; ++i) {
        const Saved& sv = g_saved[i];
        // table 2: the effects sites added 2026-11-13 (integer ops and random numbers at 0-5, the wrappers from 24 on)
        if (table == 2 ? (sv.table != 1 || (sv.index >= 6 && sv.index < 24)) : sv.table != table) continue;
        if (switch_site(sv, sv.target, on)) ++n;
    }
    return n;
}
