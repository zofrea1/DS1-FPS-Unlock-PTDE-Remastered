// Runs PTDE's original vector/matrix helpers (the game's code mapped at 0x400000) and FastRenderMath's
// replacements on the same random inputs, and compares every output bit, the return values, aliasing (out = an
// input) and the registers the custom-convention originals keep.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <float.h>
#include <cstdio>
#include <cstring>
#include <random>

#include "rendermath.cpp"

static std::mt19937 rng(12345);
static int g_fail = 0;

static bool g_nan_heavy = false;
static uint32_t rnd_bits() {
    if (g_nan_heavy && (rng() & 1)) return 0x7F800001u | (rng() & 0x803FFFFFu) | ((rng() & 1u) << 22);
    switch (rng() % 8) {
    case 0: return rng();  // anything: NaN, inf, denormals
    case 1: { float f = static_cast<float>(static_cast<int>(rng() % 2001) - 1000) / 7.0f; uint32_t u; std::memcpy(&u, &f, 4); return u; }
    case 2: return 0x80000000u * (rng() & 1);  // +-0
    case 3: return (rng() & 0x807FFFFFu);      // denormal
    default: { std::normal_distribution<float> n(0.0f, 100.0f); float f = n(rng); uint32_t u; std::memcpy(&u, &f, 4); return u; }
    }
}
static void fill(uint32_t* p, int n) { for (int i = 0; i < n; ++i) p[i] = rnd_bits(); }

static bool same(const void* a, const void* b, size_t n, const char* what, int iter) {
    if (std::memcmp(a, b, n) == 0) return true;
    if (g_fail < 20) std::printf("MISMATCH %s (iteration %d)\n", what, iter);
    ++g_fail;
    return false;
}

using F3 = void*(__cdecl*)(void*, const void*, const void*);
using F4 = void*(__cdecl*)(void*, const void*, const void*, const void*);
using F2 = void*(__cdecl*)(void*, const void*);
using FSet = void*(__cdecl*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
using FSplat = void*(__cdecl*)(void*, uint32_t);

static uint32_t g_ecx_after = 0;
// ecx = out, eax = A, push B, callee pops it.
__declspec(naked) void* __cdecl call_mat34add(void* fn, void* out, const void* a, const void* b) {
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov ecx, [ebp + 12]
        mov eax, [ebp + 16]
        push dword ptr [ebp + 20]
        call dword ptr [ebp + 8]
        mov g_ecx_after, ecx
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
// eax = A, push B, callee pops it; result in eax (al).
__declspec(naked) uint32_t __cdecl call_neq(void* fn, const void* a, const void* b) {
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov eax, [ebp + 12]
        push dword ptr [ebp + 16]
        call dword ptr [ebp + 8]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

#include "probe_order.inc"

// Fake parameter evaluators planted in the game's table (0x112D618) for 0x9B4650: one writes into the temporary it
// gets, one returns its own buffer; both fold every argument in, so a wrong one shows.
static uint32_t g_fake_buf[4];
static const uint32_t* __cdecl fake_eval0(uint32_t* t, uint32_t a, uint32_t* obj, uint32_t b, uint32_t c) {
    for (int i = 0; i < 4; ++i) t[i] = a * 3u + b * 5u + c * 7u + obj[1] * 11u + static_cast<uint32_t>(i);
    return t;
}
static const uint32_t* __cdecl fake_eval1(uint32_t*, uint32_t a, uint32_t* obj, uint32_t b, uint32_t c) {
    for (int i = 0; i < 4; ++i) g_fake_buf[i] = a ^ (b << 1) ^ (c << 2) ^ obj[2] ^ (0x1000u * static_cast<uint32_t>(i));
    return g_fake_buf;
}
// thiscall: ecx = obj, push c, b, a, out; the callee pops 16 bytes.
__declspec(naked) void* __cdecl call_param_eval(void* fn, void* obj, void* out, uint32_t a, uint32_t b, uint32_t c) {
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov ecx, [ebp + 12]
        push dword ptr [ebp + 28]
        push dword ptr [ebp + 24]
        push dword ptr [ebp + 20]
        push dword ptr [ebp + 16]
        call dword ptr [ebp + 8]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

// The parent starts this program again suspended, maps the game's image at 0x400000 in it before Windows fills the low
// addresses (code page files, heaps), and resumes it with "child".
static uint8_t g_image[0x11C2000];
static int run_parent(const char* self) {
    FILE* f = std::fopen("ptde_image.bin", "rb");
    if (!f) { std::printf("no image\n"); return 2; }
    std::fread(g_image, 1, sizeof(g_image), f);
    std::fclose(f);
    char cmd[MAX_PATH + 16];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" child", self);
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        std::printf("CreateProcess failed %lu\n", GetLastError());
        return 2;
    }
    void* at = VirtualAllocEx(pi.hProcess, reinterpret_cast<void*>(0x400000), sizeof(g_image), MEM_RESERVE | MEM_COMMIT,
                              PAGE_EXECUTE_READWRITE);
    SIZE_T wrote = 0;
    if (at != reinterpret_cast<void*>(0x400000) || !WriteProcessMemory(pi.hProcess, at, g_image, sizeof(g_image), &wrote)) {
        std::printf("could not map the image in the child (%lu)\n", GetLastError());
        TerminateProcess(pi.hProcess, 2);
        return 2;
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    return static_cast<int>(code);
}


// View culling (0x593E40 vs cull_box): random planes and boxes, many built so that the near or far sum lands on or
// next to zero, some outside the replacement's range (it hands those to the original), at x87 24-bit precision (the
// render thread's) and at 53-bit (the replacement falls back to the original).
static float rf(float scale) { std::normal_distribution<float> n(0.0f, scale); return n(rng); }
static void test_cull() {
    int counts[3] = {}, fallbacks = 0;
    const unsigned cws[2] = {0x007F, 0x027F};
    for (int pass = 0; pass < 2; ++pass) {
        unsigned short cw = static_cast<unsigned short>(cws[pass]), saved;
        __asm fnstcw saved
        __asm fldcw cw
        const int N = pass == 0 ? 1000000 : 100000;
        for (int it = 0; it < N; ++it) {
            alignas(16) float pl[24];
            alignas(16) uint32_t box[8];
            float c0[4], c1[4];
            for (int i = 0; i < 3; ++i) { c0[i] = rf(50.0f); c1[i] = c0[i] + std::fabs(rf(10.0f)); }
            c0[3] = rf(1.0f); c1[3] = rf(1.0f);
            if (it % 3 == 0) std::swap(c0[it % 2], c1[it % 2]);
            for (int k = 0; k < 6; ++k) {
                float a = rf(1.0f), b = rf(1.0f), c = rf(1.0f);
                if (it % 11 == 0) a = 0.0f;
                if (it % 13 == 0) b = -0.0f;
                // d: random, or exactly cancelling the near (or far) sum computed in float
                const float nx = a >= 0 ? c0[0] : c1[0], ny = b >= 0 ? c0[1] : c1[1], nz = c >= 0 ? c0[2] : c1[2];
                const float fx = a >= 0 ? c1[0] : c0[0], fy = b >= 0 ? c1[1] : c0[1], fz = c >= 0 ? c1[2] : c0[2];
                float d;
                switch (rng() % 5) {
                case 0: d = -((ny * b + nx * a) + nz * c); break;
                case 1: d = -((fy * b + fx * a) + fz * c); break;
                case 2: { float e = -((ny * b + nx * a) + nz * c); uint32_t u; std::memcpy(&u, &e, 4); u += (rng() % 5) - 2; std::memcpy(&d, &u, 4); } break;
                case 3: { float e = -((fy * b + fx * a) + fz * c); uint32_t u; std::memcpy(&u, &e, 4); u += (rng() % 5) - 2; std::memcpy(&d, &u, 4); } break;
                default: d = rf(100.0f);
                }
                pl[k * 4] = a; pl[k * 4 + 1] = b; pl[k * 4 + 2] = c; pl[k * 4 + 3] = d;
            }
            std::memcpy(box, c0, 16); std::memcpy(box + 4, c1, 16);
            // Out of range now and then: huge, tiny, denormal, inf, NaN.
            if (it % 97 == 0) { uint32_t u = rnd_bits(); std::memcpy(&pl[rng() % 24], &u, 4); ++fallbacks; }
            if (it % 101 == 0) box[rng() % 8] = rnd_bits();
            const int r1 = reinterpret_cast<int(__cdecl*)(const float*, const uint32_t*)>(0x593E40)(pl, box) & 0xFF;
            const int r2 = cull_box(pl, box);
            if (r1 != r2) { if (g_fail < 20) std::printf("MISMATCH cull cw %04X it %d: %d vs %d\n", cw, it, r1, r2); ++g_fail; }
            if (pass == 0 && r1 >= 0 && r1 < 3) ++counts[r1];
        }
        __asm fldcw saved
    }
    std::printf("cull: outside %d, straddle %d, inside %d (24-bit pass), %d inputs pushed out of range; %d mismatches so far\n",
                counts[0], counts[1], counts[2], fallbacks, g_fail);
}


// The effects library's integer vector ops (0x9852A0 shl, 0x985430 shr, 0x985020 and-not) vs ffx_shl4/shr4/andnot4:
// random lanes and counts (incl. >= 32), separate and in place (out = a, out = b).
static void test_intops() {
    using F = uint32_t*(__cdecl*)(uint32_t*, const uint32_t*, const uint32_t*);
    struct { uintptr_t orig; F repl; const char* name; } ops[] = {
        {0x9852A0, &ffx_shl4, "ffx shl"}, {0x985430, &ffx_shr4, "ffx shr"}, {0x985020, &ffx_andnot4, "ffx andnot"}};
    for (int it = 0; it < 300000; ++it) {
        alignas(16) uint32_t a[4], b[4], o1[4], o2[4], x1[4], x2[4];
        for (int i = 0; i < 4; ++i) { a[i] = rng(); b[i] = (it & 1) ? rng() : (rng() % 40); }
        for (auto& op : ops) {
            fill(o1, 4); std::memcpy(o2, o1, 16);
            uint32_t* r1 = reinterpret_cast<F>(op.orig)(o1, a, b);
            uint32_t* r2 = op.repl(o2, a, b);
            same(o1, o2, 16, op.name, it);
            if (r1 != o1 || r2 != o2) { std::printf("return %s\n", op.name); ++g_fail; }
            std::memcpy(x1, a, 16); std::memcpy(x2, a, 16);
            reinterpret_cast<F>(op.orig)(x1, x1, b);
            op.repl(x2, x2, b);
            same(x1, x2, 16, op.name, it);
            std::memcpy(x1, b, 16); std::memcpy(x2, b, 16);
            reinterpret_cast<F>(op.orig)(x1, a, x1);
            op.repl(x2, a, x2);
            same(x1, x2, 16, op.name, it);
        }
    }
    std::printf("integer ops: 300000 iterations, %d mismatches so far\n", g_fail);
}


// The effects library's random numbers: 0x983F90 xorshift, 0x983D30 [0,1), 0x9824F0 range (vs ffx_xorshift4 / rand01 /
// rand_range): output and the state after, random states, ranges (incl. NaN/inf/denormal) and scales.
static void test_random() {
    for (int it = 0; it < 200000; ++it) {
        alignas(16) uint32_t s1[4], s2[4], o1[4], o2[4], sc[4];
        for (int i = 0; i < 4; ++i) s1[i] = s2[i] = rng() | 1u;
        fill(o1, 4); std::memcpy(o2, o1, 16);
        reinterpret_cast<void*(__cdecl*)(void*, void*)>(0x983F90)(o1, s1);
        ffx_xorshift4(o2, s2);
        same(o1, o2, 16, "xorshift out", it); same(s1, s2, 16, "xorshift state", it);
        reinterpret_cast<void*(__cdecl*)(void*, void*)>(0x983D30)(o1, s1);
        ffx_rand01(reinterpret_cast<float*>(o2), s2);
        same(o1, o2, 16, "rand01 out", it); same(s1, s2, 16, "rand01 state", it);
        uint32_t lohi[2] = {rnd_bits(), rnd_bits()};
        if (it & 1) { float lo = static_cast<float>(rng() % 1000) / 37.0f, hi = static_cast<float>(rng() % 1000) / 13.0f; std::memcpy(&lohi[0], &lo, 4); std::memcpy(&lohi[1], &hi, 4); }
        const uint32_t* plohi = lohi;
        fill(sc, 4);
        reinterpret_cast<void*(__cdecl*)(void*, void*, uint32_t, const void*, uint32_t, void*)>(0x9824F0)(o1, sc, 0, &plohi, 0, s1);
        ffx_rand_range(reinterpret_cast<float*>(o2), reinterpret_cast<float*>(sc), 0, &plohi, 0, s2);
        same(o1, o2, 16, "rand range out", it); same(s1, s2, 16, "rand range state", it);
    }
    std::printf("random: 200000 iterations, %d mismatches so far\n", g_fail);
}


// 0x41FB20, 0x422110 and 0x4221B0 set-matrix-if-changed (eax = index, block, matrix; callee pops) vs
// mat_set_if_changed and its two siblings: random blocks with rows equal, differing in one lane (also NaN, -0 vs +0) or
// random, with and without the dirty structure (block+0x10) that the setters (0xF652B0, 0xF65640, 0xF655F0) mark and
// copy into; the whole block and dirty structure compared after, and eax/ecx/ebx/esi/edi/ebp as they come back.
static uint32_t g_matset_fn, g_regs_out[6];
__declspec(naked) static void call_matset(uint32_t /*index*/, void* /*block*/, void* /*matrix*/) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        push dword ptr [esp + 28]
        push dword ptr [esp + 28]
        mov eax, [esp + 28]
        mov ecx, 0x11111111
        mov ebx, 0x22222222
        mov esi, 0x33333333
        mov edi, 0x44444444
        mov ebp, 0x55555555
        mov edx, 0x66666666
        call dword ptr [g_matset_fn]
        mov [g_regs_out], ecx
        mov [g_regs_out + 4], ebx
        mov [g_regs_out + 8], esi
        mov [g_regs_out + 12], edi
        mov [g_regs_out + 16], ebp
        mov [g_regs_out + 20], eax
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
static void test_matset_one(const char* what, uint32_t orig, const void* repl, uint32_t k2, uint32_t max_index) {
    static uint8_t blk1[4096], blk2[4096], d1[8192], d2[8192];
    for (int it = 0; it < 200000; ++it) {
        const uint32_t index = rng() % (max_index + 1);
        uint32_t m[12];
        fill(m, 12);
        for (size_t i = 0; i < sizeof(blk1) / 4; ++i) reinterpret_cast<uint32_t*>(blk1)[i] = rnd_bits();
        for (size_t i = 0; i < 20; ++i) reinterpret_cast<uint32_t*>(d1)[i] = (rng() & 1) ? rng() : 0;
        uint32_t* rows = reinterpret_cast<uint32_t*>(blk1 + (index * 3 + k2) * 16);
        switch (rng() % 4) {
        case 0: std::memcpy(rows, m, 48); break;
        case 1: std::memcpy(rows, m, 48); rows[rng() % 12] = rnd_bits(); break;
        case 2: std::memcpy(rows, m, 48); rows[rng() % 12] ^= 0x80000000u; break;
        default: break;
        }
        const bool dirty = (rng() & 1) != 0;
        std::memcpy(blk2, blk1, sizeof(blk1));
        std::memcpy(d2, d1, sizeof(d1));
        reinterpret_cast<uint32_t*>(blk1)[4] = dirty ? reinterpret_cast<uint32_t>(d1) : 0;
        reinterpret_cast<uint32_t*>(blk2)[4] = dirty ? reinterpret_cast<uint32_t>(d2) : 0;
        g_matset_fn = orig;
        call_matset(index, blk1, m);
        uint32_t r1[6];
        std::memcpy(r1, g_regs_out, 24);
        g_matset_fn = reinterpret_cast<uint32_t>(repl);
        call_matset(index, blk2, m);
        reinterpret_cast<uint32_t*>(blk2)[4] = reinterpret_cast<uint32_t*>(blk1)[4];
        same(blk1, blk2, sizeof(blk1), "matset block", it);
        same(d1, d2, sizeof(d1), "matset dirty", it);
        // eax: the destination after a setter is a pointer into d1 / d2; compare it as an offset there
        if (dirty && r1[5] >= reinterpret_cast<uint32_t>(d1) && r1[5] < reinterpret_cast<uint32_t>(d1) + sizeof(d1)) {
            r1[5] += reinterpret_cast<uint32_t>(d2) - reinterpret_cast<uint32_t>(d1);
        }
        if (std::memcmp(r1, g_regs_out, 24) != 0) {
            if (g_fail < 20) {
                std::printf("%s registers differ at %d (index %u): %08X %08X %08X %08X %08X %08X vs %08X %08X %08X %08X "
                            "%08X %08X\n",
                            what, it, index, r1[0], r1[1], r1[2], r1[3], r1[4], r1[5], g_regs_out[0], g_regs_out[1],
                            g_regs_out[2], g_regs_out[3], g_regs_out[4], g_regs_out[5]);
            }
            ++g_fail;
        }
    }
    std::printf("%s: 200000 iterations, %d mismatches so far\n", what, g_fail);
}
static void test_matset() {
    test_matset_one("set matrix if changed 0x41FB20", 0x41FB20, reinterpret_cast<const void*>(&mat_set_if_changed), 6, 82);
    test_matset_one("set matrix if changed 0x422110", 0x422110,
                    reinterpret_cast<const void*>(&mat_set_if_changed), 6, 82);
    test_matset_one("set matrix if changed 0x4221B0", 0x4221B0,
                    reinterpret_cast<const void*>(&mat_set_if_changed_4221B0), 0x78, 41);
}

// 0x431C30 / 0x431D40 dirty-run flushes vs flush_vs_constants / flush_ps_constants: a fake device whose vtable slots
// 0x178 and 0x1B4 log every call (device, first register, data offset, count, and the eight dirty words at that moment)
// and return a changing value; random dirty and mask words (empty, full, sparse, dense, runs across words); the call
// logs, the dirty words after, and eax/ecx/edx/ebx/esi/edi/ebp compared.
struct FlushCall {
    uint32_t dev, start, data_off, count, slot, words[8];
};
static FlushCall g_fcalls[300];
static int g_nfcalls;
static uint8_t* g_fbase;
static uint32_t g_fret;
static HRESULT __stdcall fake_set_const(uint32_t slot, void* dev, uint32_t start, const float* data, uint32_t count) {
    if (g_nfcalls < 300) {
        FlushCall& c = g_fcalls[g_nfcalls];
        c.dev = reinterpret_cast<uint32_t>(dev);
        c.start = start;
        c.data_off = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(data) - g_fbase);
        c.count = count;
        c.slot = slot;
        std::memcpy(c.words, g_fbase, 32);
    }
    ++g_nfcalls;
    return static_cast<HRESULT>(g_fret += 0x01010101u);
}
static HRESULT __stdcall fake_vs(void* dev, uint32_t start, const float* data, uint32_t count) {
    return fake_set_const(0x178, dev, start, data, count);
}
static HRESULT __stdcall fake_ps(void* dev, uint32_t start, const float* data, uint32_t count) {
    return fake_set_const(0x1B4, dev, start, data, count);
}
static uint32_t g_flush_fn, g_fregs[7];
__declspec(naked) static void call_flush(void* /*dirty*/, void* /*ctx*/) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        push dword ptr [esp + 24]
        push dword ptr [esp + 24]
        mov eax, 0x77777777
        mov ecx, 0x11111111
        mov ebx, 0x22222222
        mov esi, 0x33333333
        mov edi, 0x44444444
        mov ebp, 0x55555555
        mov edx, 0x66666666
        call dword ptr [g_flush_fn]
        mov [g_fregs], ecx
        mov [g_fregs + 4], ebx
        mov [g_fregs + 8], esi
        mov [g_fregs + 12], edi
        mov [g_fregs + 16], ebp
        mov [g_fregs + 20], eax
        mov [g_fregs + 24], edx
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
static uint32_t rand_word() {
    switch (rng() % 8) {
    case 0: case 1: return 0;
    case 2: return 0xFFFFFFFFu;
    case 3: return rng() & rng() & rng();  // sparse
    case 4: return rng() | rng();          // dense
    case 5: {                              // one run
        const uint32_t a = rng() % 32, n = 1 + rng() % 32;
        const uint64_t m = ((1ull << n) - 1) << a;
        return static_cast<uint32_t>(m);
    }
    case 6: return 0x80000000u >> (rng() % 32) | 1u << (rng() % 32);
    default: return rng();
    }
}
static void test_flush() {
    static uint8_t base1[0x40 + 256 * 16], base2[sizeof(base1)];
    static void* vtbl[128];
    for (int i = 0; i < 128; ++i) vtbl[i] = nullptr;
    vtbl[0x178 / 4] = reinterpret_cast<void*>(&fake_vs);
    vtbl[0x1B4 / 4] = reinterpret_cast<void*>(&fake_ps);
    void* device[4] = {vtbl, nullptr, nullptr, nullptr};
    void* ctx[8] = {};
    ctx[4] = device;  // ctx + 0x10
    static FlushCall log1[300];
    long calls = 0;
    for (int it = 0; it < 400000; ++it) {
        const bool ps = (it & 1) != 0;
        uint32_t w[16];
        for (int i = 0; i < 8; ++i) w[i] = rand_word();
        const int mm = rng() % 4;
        for (int i = 0; i < 8; ++i) w[8 + i] = mm == 0 ? 0xFFFFFFFFu : mm == 1 ? rand_word() : (rng() & 1) ? 0xFFFFFFFFu : 0;
        std::memcpy(base1, w, 64);
        std::memcpy(base2, w, 64);
        g_fbase = base1;
        g_nfcalls = 0;
        g_fret = 0x10000000u;
        g_flush_fn = ps ? 0x431D40 : 0x431C30;
        call_flush(base1, ctx);
        const int n1 = g_nfcalls;
        std::memcpy(log1, g_fcalls, sizeof(g_fcalls));
        uint32_t r1[7];
        std::memcpy(r1, g_fregs, 28);
        g_fbase = base2;
        g_nfcalls = 0;
        g_fret = 0x10000000u;
        g_flush_fn = reinterpret_cast<uint32_t>(ps ? &flush_ps_constants : &flush_vs_constants);
        call_flush(base2, ctx);
        calls += n1;
        // ecx is a pointer into base1 / base2 (dirty + 0x20 with no last call, the fake's g_fbase after one): compare
        // it as an offset
        for (int k : {0, 6}) {
            if (r1[k] >= reinterpret_cast<uint32_t>(base1) && r1[k] <= reinterpret_cast<uint32_t>(base1) + sizeof(base1)) {
                r1[k] += reinterpret_cast<uint32_t>(base2) - reinterpret_cast<uint32_t>(base1);
            }
        }
        if (n1 != g_nfcalls || std::memcmp(log1, g_fcalls, sizeof(FlushCall) * (n1 < 300 ? n1 : 300)) != 0) {
            if (g_fail < 20) std::printf("MISMATCH flush calls (iteration %d): %d vs %d calls\n", it, n1, g_nfcalls);
            ++g_fail;
        }
        same(base1, base2, 64, "flush words", it);
        if (std::memcmp(r1, g_fregs, 28) != 0) {
            if (g_fail < 20) {
                std::printf("flush registers differ at %d: %08X %08X %08X %08X %08X %08X %08X vs %08X %08X %08X %08X %08X "
                            "%08X %08X\n",
                            it, r1[0], r1[1], r1[2], r1[3], r1[4], r1[5], r1[6], g_fregs[0], g_fregs[1], g_fregs[2],
                            g_fregs[3], g_fregs[4], g_fregs[5], g_fregs[6]);
            }
            ++g_fail;
        }
    }
    std::printf("constant flushes: 400000 iterations, %ld device calls, %d mismatches so far\n", calls, g_fail);
}

// 0x982C30 / 0x9817B0 keyframe curves vs ffx_curve4 / ffx_curve1: random streams (n 0..8; keys sorted, with
// repeats, or random, NaN/inf/zero/denormal mixed in; values anything), times on keys, between, outside or anything;
// out, the stream pointer after, and eax/edx/ebx/esi/edi/ebp compared; under x87 precision 24, 53 and 64 bits.
static uint32_t g_curve_fn, g_cregs[6];
__declspec(naked) static void call_curve(void* /*out*/, const void* /*t*/, void* /*stream*/) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        push dword ptr [esp + 28]
        push dword ptr [esp + 28]
        push dword ptr [esp + 28]
        mov ebx, 0x22222222
        mov esi, 0x33333333
        mov edi, 0x44444444
        mov ebp, 0x55555555
        call dword ptr [g_curve_fn]
        add esp, 12
        mov [g_cregs], eax
        mov [g_cregs + 4], edx
        mov [g_cregs + 8], ebx
        mov [g_cregs + 12], esi
        mov [g_cregs + 16], edi
        mov [g_cregs + 20], ebp
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
static float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static void test_curves() {
    static uint32_t buf1[64], buf2[64];
    const unsigned pcs[3] = {_PC_24, _PC_53, _PC_64};
    long cases = 0;
    for (int pc = 0; pc < 3; ++pc) {
        unsigned prev = 0;
        _controlfp_s(&prev, pcs[pc], _MCW_PC);
        for (int it = 0; it < 150000; ++it) {
            const bool four = (it & 1) != 0;
            const int n = static_cast<int>(rng() % 9);
            uint32_t* keys = buf1 + 9;  // buf1[8] = n
            const int mode = rng() % 4;
            float x = static_cast<float>(static_cast<int>(rng() % 200) - 100) / 8.0f;
            for (int k = 0; k < n; ++k) {
                if (mode == 0) { keys[k] = rnd_bits(); continue; }
                if (mode == 1 && (rng() % 3) == 0) { keys[k] = k ? keys[k - 1] : f2u(x); continue; }
                x += static_cast<float>(rng() % 64) / 16.0f;
                keys[k] = f2u(x);
                if (mode == 2 && (rng() % 6) == 0) keys[k] = rnd_bits();
            }
            for (int k = 0; k < n; ++k) keys[n + k] = rnd_bits();
            for (int k = 0; k < 8; ++k) buf1[k] = rnd_bits();
            buf1[8] = static_cast<uint32_t>(n);
            uint32_t t[4];
            for (int i = 0; i < 4; ++i) {
                switch (rng() % 5) {
                case 0: t[i] = n ? keys[rng() % n] : rnd_bits(); break;
                case 1: t[i] = n ? f2u(u2f(keys[rng() % n]) + static_cast<float>(rng() % 100) / 50.0f - 1.0f) : rnd_bits(); break;
                case 2: t[i] = f2u(static_cast<float>(static_cast<int>(rng() % 400) - 200) / 4.0f); break;
                default: t[i] = rnd_bits(); break;
                }
            }
            std::memcpy(buf2, buf1, sizeof(buf1));
            uint32_t out1[4] = {1, 2, 3, 4}, out2[4] = {1, 2, 3, 4};
            uint32_t* stream1 = buf1 + 8;
            uint32_t* stream2 = buf2 + 8;
            g_curve_fn = four ? 0x982C30 : 0x9817B0;
            call_curve(out1, t, &stream1);
            uint32_t r1[6];
            std::memcpy(r1, g_cregs, 24);
            g_curve_fn = reinterpret_cast<uint32_t>(four ? reinterpret_cast<const void*>(&ffx_curve4)
                                                         : reinterpret_cast<const void*>(&ffx_curve1));
            call_curve(out2, t, &stream2);
            ++cases;
            // eax (and edx for one float) point at out1 / out2: compare as offsets
            if (r1[0] == reinterpret_cast<uint32_t>(out1)) r1[0] = reinterpret_cast<uint32_t>(out2);
            if (!four && r1[1] == reinterpret_cast<uint32_t>(out1)) r1[1] = reinterpret_cast<uint32_t>(out2);
            same(out1, out2, four ? 16 : 4, four ? "curve4 out" : "curve1 out", it);
            if ((stream1 - buf1) != (stream2 - buf2)) {
                if (g_fail < 20) std::printf("MISMATCH curve stream (iteration %d)\n", it);
                ++g_fail;
            }
            if (std::memcmp(r1, g_cregs, 24) != 0) {
                if (g_fail < 20) {
                    std::printf("curve%d registers differ at %d (pc %d): %08X %08X vs %08X %08X\n", four ? 4 : 1, it, pc,
                                r1[0], r1[1], g_cregs[0], g_cregs[1]);
                }
                ++g_fail;
            }
        }
        _controlfp_s(&prev, _PC_53, _MCW_PC);
        std::printf("keyframe curves, x87 precision %s: %d mismatches so far\n", pc == 0 ? "24" : pc == 1 ? "53" : "64",
                    g_fail);
    }
    std::printf("keyframe curves: %ld cases\n", cases);
}

// 0x983400 (the eased keyframe curve) vs ffx_curve4_eased: streams of n 1..8 keys (sorted and finite: increasing,
// with repeats, sometimes +inf at the end, denormals and zeros of both signs among them), any values, tangent angles
// below, at and above pi/4 and anything (NaN, inf); times on keys, between, outside, +-inf, denormal. (A NaN time or
// a -inf key makes the segment fraction NaN, and the original's sqrtf then goes through the C runtime's error handling,
// which needs the runtime set up; both versions call that same _CIsqrt with the same argument.) out, the stream after,
// eax/edx/ebx/esi/edi/ebp compared, under x87 precision 24, 53 and 64 bits.
static uint32_t eased_angle() {
    const float pi4f = 0.785398185253143310546875f;  // the double at 0x1115B88 is this float exactly
    switch (rng() % 6) {
    case 0: return f2u(pi4f);
    case 1: { const uint32_t u = f2u(pi4f); return (rng() & 1) ? u + 1 + rng() % 3 : u - 1 - rng() % 3; }
    case 2: return f2u(static_cast<float>(rng() % 1000) / 636.0f);         // 0 .. 1.57
    case 3: return f2u(-static_cast<float>(rng() % 1000) / 636.0f);
    default: return rnd_bits();
    }
}
static void test_eased() {
    static uint32_t buf1[64], buf2[64];
    const unsigned pcs[3] = {_PC_24, _PC_53, _PC_64};
    long cases = 0, eased = 0, fast_lanes = 0;
    for (int pc = 0; pc < 3; ++pc) {
        unsigned prev = 0;
        _controlfp_s(&prev, pcs[pc], _MCW_PC);
        for (int it = 0; it < 200000; ++it) {
            const int n = 1 + static_cast<int>(rng() % 8);
            uint32_t* keys = buf1 + 9;  // buf1[8] = n
            float x = static_cast<float>(static_cast<int>(rng() % 200) - 100) / 8.0f;
            if (rng() % 8 == 0) x = u2f(rng() & 0x807FFFFFu);  // a denormal or zero to start
            for (int k = 0; k < n; ++k) {
                if (k && rng() % 4 == 0) { keys[k] = keys[k - 1]; continue; }
                const int step = static_cast<int>(rng() % 4);
                x += step == 0 ? u2f(rng() & 0x007FFFFFu) : step == 1 ? static_cast<float>(rng() % 64) / 1024.0f
                                                                      : static_cast<float>(rng() % 64) / 16.0f;
                keys[k] = f2u(x);
            }
            if (n > 1 && rng() % 10 == 0) keys[n - 1] = 0x7F800000u;
            for (int k = 0; k < n; ++k) keys[n + k] = rnd_bits();
            for (int k = 0; k < 2 * n; ++k) keys[2 * n + k] = eased_angle();
            for (int k = 0; k < 8; ++k) buf1[k] = rnd_bits();
            buf1[8] = static_cast<uint32_t>(n);
            uint32_t t[4];
            for (int i = 0; i < 4; ++i) {
                const int a = static_cast<int>(rng() % n);
                switch (rng() % 6) {
                case 0: t[i] = keys[a]; break;
                case 1:
                case 2: {
                    const int b = a + 1 < n ? a + 1 : a;
                    const float lo = u2f(keys[a]), hi = u2f(keys[b]);
                    const float f = static_cast<float>(rng() % 1001) / 1000.0f;
                    t[i] = f2u(hi == u2f(0x7F800000u) ? lo + f : lo + (hi - lo) * f);
                    break;
                }
                case 3: t[i] = f2u(u2f(keys[a]) + static_cast<float>(static_cast<int>(rng() % 200) - 100) / 64.0f); break;
                case 4: t[i] = (rng() & 1) ? 0xFF800000u : 0x7F800000u; break;
                default: t[i] = rng() % 3 ? f2u(static_cast<float>(static_cast<int>(rng() % 400) - 200) / 4.0f)
                                          : (rng() & 0x807FFFFFu);
                }
                if ((t[i] & 0x7F800000u) == 0x7F800000u && (t[i] & 0x007FFFFFu)) t[i] = 0;  // no NaN times
                if (u2f(keys[0]) < u2f(t[i]) && u2f(t[i]) < u2f(keys[n - 1])) ++eased;
            }
            std::memcpy(buf2, buf1, sizeof(buf1));
            uint32_t out1[4] = {1, 2, 3, 4}, out2[4] = {1, 2, 3, 4};
            uint32_t* stream1 = buf1 + 8;
            uint32_t* stream2 = buf2 + 8;
            g_curve_fn = 0x983400;
            call_curve(out1, t, &stream1);
            uint32_t r1[6];
            std::memcpy(r1, g_cregs, 24);
            g_curve_fn = reinterpret_cast<uint32_t>(&ffx_curve4_eased);
            call_curve(out2, t, &stream2);
            if (pc == 0) {  // how many lanes the SSE path finishes (no fallback to the x87 code)
                const uint32_t* kk = keys;
                for (int i = 0; i < 4; ++i) {
                    const unsigned csr = _mm_getcsr();
                    _mm_setcsr(csr & ~0x3Fu);
                    uint32_t r = 0;
                    if (eased_lane_ss(kk, kk + n, kk + 2 * n, kk + 3 * n, n, t[i], *reinterpret_cast<const uint32_t*>(0x112D704),
                                      *reinterpret_cast<const uint32_t*>(0x11D92AC), &r) &&
                        (_mm_getcsr() & 0x1Du) == 0 && u2f(keys[0]) < u2f(t[i]) && u2f(t[i]) < u2f(keys[n - 1])) {
                        ++fast_lanes;
                    }
                    _mm_setcsr(csr);
                }
            }
            ++cases;
            if (r1[0] == reinterpret_cast<uint32_t>(out1)) r1[0] = reinterpret_cast<uint32_t>(out2);
            same(out1, out2, 16, "eased curve out", it);
            if ((stream1 - buf1) != (stream2 - buf2)) {
                if (g_fail < 20) std::printf("MISMATCH eased curve stream (iteration %d)\n", it);
                ++g_fail;
            }
            if (std::memcmp(r1, g_cregs, 24) != 0) {
                if (g_fail < 20) {
                    std::printf("eased curve registers differ at %d (pc %d): %08X %08X vs %08X %08X\n", it, pc, r1[0],
                                r1[1], g_cregs[0], g_cregs[1]);
                }
                ++g_fail;
            }
        }
        _controlfp_s(&prev, _PC_53, _MCW_PC);
        std::printf("eased curve, x87 precision %s: %d mismatches so far\n", pc == 0 ? "24" : pc == 1 ? "53" : "64",
                    g_fail);
    }
    std::printf("eased curve: %ld cases (%ld lanes inside the keys, eased; at precision 24, %ld of them finished in SSE)\n",
                cases, eased, fast_lanes);
}

// The eased curve at x87 precision 24 (the render thread's, where the SSE path runs) with the data the game has: finite
// keys spread or bunched (steps down to one ulp), finite values of any size, angles across -pi/2..pi/2 and next to
// pi/4, times between keys and right next to them (u near 0 and 1).
static void test_eased_real() {
    static uint32_t buf1[64], buf2[64];
    unsigned prev = 0;
    _controlfp_s(&prev, _PC_24, _MCW_PC);
    long cases = 0;
    const int before = g_fail;
    for (int it = 0; it < 1500000; ++it) {
        const int n = 2 + static_cast<int>(rng() % 7);
        uint32_t* keys = buf1 + 9;
        float x = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 64.0f;
        for (int k = 0; k < n; ++k) {
            switch (rng() % 4) {
            case 0: keys[k] = f2u(x) + 1 + rng() % 4; x = u2f(keys[k]); break;  // a few ulps on
            case 1: x += static_cast<float>(rng() % 100) / 1000.0f + 0.001f; keys[k] = f2u(x); break;
            default: x += static_cast<float>(rng() % 400) / 50.0f + 0.01f; keys[k] = f2u(x); break;
            }
        }
        for (int k = 0; k < n; ++k) {
            const float m = (rng() % 4) ? 1.0f : (rng() % 2 ? 1e-3f : 1e6f);
            keys[n + k] = f2u((static_cast<float>(static_cast<int>(rng() % 20001) - 10000) / 10000.0f) * m);
        }
        for (int k = 0; k < 2 * n; ++k) {
            const uint32_t q = f2u(0.785398185253143310546875f);
            switch (rng() % 3) {
            case 0: keys[2 * n + k] = q + static_cast<uint32_t>(static_cast<int>(rng() % 9) - 4); break;
            default: keys[2 * n + k] = f2u((static_cast<float>(static_cast<int>(rng() % 3141)) - 1570.0f) / 1000.0f); break;
            }
        }
        buf1[8] = static_cast<uint32_t>(n);
        uint32_t t[4];
        for (int i = 0; i < 4; ++i) {
            const int a = static_cast<int>(rng() % (n - 1));
            const uint32_t lo = keys[a], hi = keys[a + 1];
            switch (rng() % 4) {
            case 0: t[i] = lo + 1; break;   // just after a key
            case 1: t[i] = hi - 1; break;   // just before the next
            default: {
                const float f = static_cast<float>(rng() % 100001) / 100000.0f;
                t[i] = f2u(u2f(lo) + (u2f(hi) - u2f(lo)) * f);
            }
            }
            if ((t[i] & 0x7F800000u) == 0x7F800000u) t[i] = lo;  // one ulp off +-0 or the top: no NaN or inf times
        }
        std::memcpy(buf2, buf1, sizeof(buf1));
        uint32_t out1[4] = {1, 2, 3, 4}, out2[4] = {1, 2, 3, 4};
        uint32_t* stream1 = buf1 + 8;
        uint32_t* stream2 = buf2 + 8;
        g_curve_fn = 0x983400;
        call_curve(out1, t, &stream1);
        uint32_t r1[6];
        std::memcpy(r1, g_cregs, 24);
        g_curve_fn = reinterpret_cast<uint32_t>(&ffx_curve4_eased);
        call_curve(out2, t, &stream2);
        ++cases;
        if (r1[0] == reinterpret_cast<uint32_t>(out1)) r1[0] = reinterpret_cast<uint32_t>(out2);
        same(out1, out2, 16, "eased curve (game data) out", it);
        if (std::memcmp(r1, g_cregs, 24) != 0 || (stream1 - buf1) != (stream2 - buf2)) {
            if (g_fail < 20) std::printf("eased curve (game data): registers or stream differ at %d\n", it);
            ++g_fail;
        }
    }
    _controlfp_s(&prev, _PC_53, _MCW_PC);
    std::printf("eased curve, game data at x87 precision 24: %ld cases, %d mismatches\n", cases, g_fail - before);
}

// The effects glue rewrites (0x9AF7F0, 0x9B5DC0, 0x9B18A0, 0x9B4C60) vs ffx_mat3_madd, ffx_sincos_poly,
// ffx_pack_colors, ffx_rows_transposed: random inputs, every output byte and eax/edx (pointers as offsets) compared,
// and ebx/esi/edi/ebp kept.
static uint32_t g_gfn, g_gargs[8], g_gn, g_gregs[6];
__declspec(naked) static void call_glue() {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov ecx, [g_gn]
    push_loop:
        test ecx, ecx
        jz pushed
        push dword ptr [g_gargs + ecx * 4 - 4]
        dec ecx
        jmp push_loop
    pushed:
        mov ebx, 0x22222222
        mov esi, 0x33333333
        mov edi, 0x44444444
        mov ebp, 0x55555555
        call dword ptr [g_gfn]
        mov ecx, [g_gn]
        lea esp, [esp + ecx * 4]
        mov [g_gregs], eax
        mov [g_gregs + 4], edx
        mov [g_gregs + 8], ebx
        mov [g_gregs + 12], esi
        mov [g_gregs + 16], edi
        mov [g_gregs + 20], ebp
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
static void glue_compare(const char* what, int it, const uint8_t* o1, const uint8_t* o2, size_t n, const uint32_t* r1,
                         uint32_t lo1, uint32_t lo2, bool check_edx) {
    same(o1, o2, n, what, it);
    uint32_t a = r1[0], d = r1[1];
    if (a >= lo1 && a < lo1 + 0x400) a += lo2 - lo1;
    if (d >= lo1 && d < lo1 + 0x400) d += lo2 - lo1;
    if (a != g_gregs[0] || (check_edx && d != g_gregs[1]) || std::memcmp(r1 + 2, g_gregs + 2, 16) != 0) {
        if (g_fail < 20) std::printf("%s registers differ at %d: %08X %08X vs %08X %08X\n", what, it, a, d, g_gregs[0], g_gregs[1]);
        ++g_fail;
    }
}
static void test_glue() {
    static uint8_t in[0x400], o1[0x400], o2[0x400];
    for (int it = 0; it < 200000; ++it) {
        const int kind = it % 4;
        fill(reinterpret_cast<uint32_t*>(in), 0x100);
        if (kind == 2 && (rng() & 1)) {  // colours mostly in range
            for (int i = 0; i < 16; ++i) { float f = static_cast<float>(rng() % 1200) / 1000.0f - 0.1f; std::memcpy(in + i * 4, &f, 4); }
        }
        if (kind == 1 && (rng() & 1)) {  // angles mostly in the polynomial's range
            for (int i = 0; i < 4; ++i) { float f = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 600.0f; std::memcpy(in + i * 4, &f, 4); }
        }
        fill(reinterpret_cast<uint32_t*>(o1), 0x100);
        std::memcpy(o2, o1, sizeof(o1));
        uint32_t r1[6];
        size_t n = 0;
        const char* what = "";
        bool edx = true;
        for (int pass = 0; pass < 2; ++pass) {
            uint8_t* o = pass ? o2 : o1;
            const uint32_t I = reinterpret_cast<uint32_t>(in), O = reinterpret_cast<uint32_t>(o);
            switch (kind) {
            case 0:
                what = "glue 0x9AF7F0"; n = 48; g_gn = 7;
                g_gargs[0] = O; g_gargs[1] = O + 16; g_gargs[2] = O + 32; g_gargs[3] = I; g_gargs[4] = I + 16; g_gargs[5] = I + 32; g_gargs[6] = I + 64;
                g_gfn = pass ? reinterpret_cast<uint32_t>(&ffx_mat3_madd) : 0x9AF7F0;
                break;
            case 1:
                what = "glue 0x9B5DC0"; n = 32; g_gn = 3;
                g_gargs[0] = O; g_gargs[1] = O + 16; g_gargs[2] = I;
                g_gfn = pass ? reinterpret_cast<uint32_t>(&ffx_sincos_poly) : 0x9B5DC0;
                break;
            case 2:
                what = "glue 0x9B18A0"; n = 16; g_gn = 5;
                g_gargs[0] = O; g_gargs[1] = I; g_gargs[2] = I + 16; g_gargs[3] = I + 32; g_gargs[4] = I + 48;
                g_gfn = pass ? reinterpret_cast<uint32_t>(&ffx_pack_colors) : 0x9B18A0;
                break;
            default: {
                what = "glue 0x9B4C60"; n = 0x200; g_gn = 7; edx = false;
                static int off, stride;
                if (!pass) { off = static_cast<int>(rng() % 64) * 4; const int strides[5] = {16, 32, 64, 4, 0}; stride = strides[rng() % 5]; }
                g_gargs[0] = O; g_gargs[1] = static_cast<uint32_t>(off); g_gargs[2] = static_cast<uint32_t>(stride);
                g_gargs[3] = I; g_gargs[4] = I + 16; g_gargs[5] = I + 32; g_gargs[6] = I + 48;
                g_gfn = pass ? reinterpret_cast<uint32_t>(&ffx_rows_transposed) : 0x9B4C60;
                break;
            }
            }
            call_glue();
            if (!pass) std::memcpy(r1, g_gregs, 24);
        }
        glue_compare(what, it, o1, o2, n, r1, reinterpret_cast<uint32_t>(o1), reinterpret_cast<uint32_t>(o2), edx);
    }
    std::printf("effects glue: 200000 iterations, %d mismatches so far\n", g_fail);
}

// 0x446D30 box transform vs aabb_transform (thiscall: ecx = box, out, matrix): random boxes and matrices (also with
// NaN, infinities, zeros, denormals; min above max), out apart from or the same as the box; the 32 bytes out and eax
// compared, ebx/esi/edi/ebp kept.
static uint32_t g_box_fn, g_bregs[5];
__declspec(naked) static void call_box(void* /*box*/, void* /*out*/, void* /*m*/) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov ecx, [esp + 20]
        push dword ptr [esp + 28]
        push dword ptr [esp + 28]
        mov ebx, 0x22222222
        mov esi, 0x33333333
        mov edi, 0x44444444
        mov ebp, 0x55555555
        call dword ptr [g_box_fn]
        mov [g_bregs], eax
        mov [g_bregs + 4], ebx
        mov [g_bregs + 8], esi
        mov [g_bregs + 12], edi
        mov [g_bregs + 16], ebp
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
static void test_box() {
    alignas(16) static uint32_t box1[8], box2[8], out1[8], out2[8], m[16];
    for (int it = 0; it < 400000; ++it) {
        fill(m, 16);
        fill(box1, 8);
        if (rng() & 1) {  // a plain box and an affine matrix
            for (int i = 0; i < 3; ++i) {
                const float a = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 10.0f;
                const float b = a + static_cast<float>(rng() % 500) / 10.0f;
                std::memcpy(box1 + i, &a, 4);
                std::memcpy(box1 + 4 + i, &b, 4);
            }
            const float one = 1.0f, zero = 0.0f;
            std::memcpy(box1 + 3, &one, 4);
            std::memcpy(box1 + 7, &one, 4);
            for (int i = 0; i < 12; ++i) { float f = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 500.0f; std::memcpy(m + i, &f, 4); }
            std::memcpy(m + 3, &zero, 4); std::memcpy(m + 7, &zero, 4); std::memcpy(m + 11, &zero, 4); std::memcpy(m + 15, &one, 4);
        }
        std::memcpy(box2, box1, 32);
        const bool inplace = (rng() % 4) == 0;
        fill(out1, 8);
        std::memcpy(out2, out1, 32);
        g_box_fn = 0x446D30;
        call_box(box1, inplace ? box1 : out1, m);
        uint32_t r1[5];
        std::memcpy(r1, g_bregs, 20);
        g_box_fn = reinterpret_cast<uint32_t>(&aabb_transform);
        call_box(box2, inplace ? box2 : out2, m);
        if (inplace) {
            same(box1, box2, 32, "box transform (in place)", it);
            if (r1[0] == reinterpret_cast<uint32_t>(box1)) r1[0] = reinterpret_cast<uint32_t>(box2);
        } else {
            same(out1, out2, 32, "box transform", it);
            same(box1, box2, 32, "box transform input", it);
            if (r1[0] == reinterpret_cast<uint32_t>(out1)) r1[0] = reinterpret_cast<uint32_t>(out2);
        }
        if (std::memcmp(r1, g_bregs, 20) != 0) {
            if (g_fail < 20) std::printf("box transform registers differ at %d: %08X vs %08X\n", it, r1[0], g_bregs[0]);
            ++g_fail;
        }
    }
    std::printf("box transform: 400000 iterations, %d mismatches so far\n", g_fail);
}

// 0x994AE0 (arrays through 0x44C6A0) and 0x995C60 (bounds of three arrays) vs ffx_arrays_44c6a0 / ffx_bounds3:
// n 0..23, arrays missing (null) or present, every array byte and output compared.
static void test_arrays() {
    alignas(16) static uint32_t a1[3][28], a2[3][28], o1[8], o2[8];
    for (int it = 0; it < 200000; ++it) {
        const uint32_t n = rng() % 24;
        for (int k = 0; k < 3; ++k) fill(a1[k], 28);
        if (rng() & 1) {
            for (int k = 0; k < 3; ++k) for (int i = 0; i < 28; ++i) { float f = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 37.0f; std::memcpy(&a1[k][i], &f, 4); }
        }
        std::memcpy(a2, a1, sizeof(a1));
        if (it & 1) {
            const unsigned present = rng() % 8;
            g_gn = 4;
            for (int pass = 0; pass < 2; ++pass) {
                uint32_t (*a)[28] = pass ? a2 : a1;
                g_gargs[0] = n;
                for (int k = 0; k < 3; ++k) g_gargs[1 + k] = (present >> k) & 1 ? reinterpret_cast<uint32_t>(a[k]) : 0;
                g_gfn = pass ? reinterpret_cast<uint32_t>(&ffx_arrays_44c6a0) : 0x994AE0;
                call_glue();
            }
            same(a1, a2, sizeof(a1), "arrays 0x994AE0", it);
        } else {
            fill(o1, 8);
            std::memcpy(o2, o1, sizeof(o1));
            g_gn = 6;
            for (int pass = 0; pass < 2; ++pass) {
                uint32_t (*a)[28] = pass ? a2 : a1;
                uint32_t* o = pass ? o2 : o1;
                g_gargs[0] = reinterpret_cast<uint32_t>(o);
                g_gargs[1] = reinterpret_cast<uint32_t>(o + 4);
                g_gargs[2] = n;
                for (int k = 0; k < 3; ++k) g_gargs[3 + k] = reinterpret_cast<uint32_t>(a[k]);
                g_gfn = pass ? reinterpret_cast<uint32_t>(&ffx_bounds3) : 0x995C60;
                call_glue();
            }
            same(o1, o2, sizeof(o1), "bounds 0x995C60", it);
            same(a1, a2, sizeof(a1), "bounds 0x995C60 input", it);
        }
    }
    std::printf("arrays and bounds: 200000 iterations, %d mismatches so far\n", g_fail);
}

// 0x99C540's particle loop (0x99E980..0x99F1EA) run on a fake frame: the original, and with 0x99E9B0 jumping to
// ffx_particle_loop_entry; the loop's exit 0x99F1EA comes back here. The whole frame above esp, the particles, the
// output and eax/ecx/edx at the exit compared. n 0..9, strides 0, 16, 48, 48n.
static uint32_t g_loop_ebp, g_loop_ebx, g_loop_esp, g_loop_saved_esp, g_loop_regs[3], g_loop_entry = 0x99E980;
__declspec(naked) static void loop_back() {
    __asm {
        mov [g_loop_regs], eax
        mov [g_loop_regs + 4], ecx
        mov [g_loop_regs + 8], edx
        mov esp, [g_loop_saved_esp]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
__declspec(naked) static void run_loop() {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov [g_loop_saved_esp], esp
        mov ebx, [g_loop_ebx]
        mov esp, [g_loop_esp]
        mov ebp, [g_loop_ebp]
        mov eax, 0x11111111
        mov ecx, 0x22222222
        mov edx, 0x33333333
        push dword ptr [g_loop_entry]
        ret
    }
}
static void run_loop_at(uint32_t entry) {
    g_loop_entry = entry;
    run_loop();
}
static void put_jump(uint32_t at, const void* to) {
    uint8_t b[5] = {0xE9};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(to) - (at + 5));
    std::memcpy(b + 1, &rel, 4);
    std::memcpy(reinterpret_cast<void*>(at), b, 5);
}
static void test_particle_loop() {
    static uint8_t orig_site[12];
    std::memcpy(orig_site, reinterpret_cast<void*>(0x99E9B0), 12);
    put_jump(0x99F1EA, reinterpret_cast<const void*>(&loop_back));
    alignas(16) static uint8_t stack1[0x10000], stack2[0x10000], parts1[9 * 72], parts2[9 * 72], out1[0x10000], out2[0x10000];
    alignas(16) static uint32_t args1[8], args2[8];
    long iters = 0;
    for (int it = 0; it < 30000; ++it) {
        const uint32_t n = rng() % 10;
        fill(reinterpret_cast<uint32_t*>(stack1 + 0xE000 - 0x2400), 0x2400 / 4);
        fill(reinterpret_cast<uint32_t*>(parts1), sizeof(parts1) / 4);
        if (rng() & 1) {
            for (size_t i = 0; i < sizeof(parts1) / 4; ++i) { float f = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 64.0f; std::memcpy(parts1 + 4 * i, &f, 4); }
        }
        const int strides[4] = {0, 16, 48, static_cast<int>(48 * n)};
        const int stride = strides[rng() % 4];
        std::memcpy(stack1 + 0xE000 - 0x668, &stride, 4);
        std::memset(out1, 0xA5, sizeof(out1));
        std::memcpy(stack2, stack1, sizeof(stack1));
        std::memcpy(parts2, parts1, sizeof(parts1));
        std::memcpy(out2, out1, sizeof(out1));
        uint32_t r1[3];
        for (int pass = 0; pass < 2; ++pass) {
            uint8_t* st = pass ? stack2 : stack1;
            uint32_t* a = pass ? args2 : args1;
            a[2] = reinterpret_cast<uint32_t>(pass ? out2 : out1);
            a[3] = reinterpret_cast<uint32_t>(pass ? parts2 : parts1);
            a[4] = n;
            g_loop_ebx = reinterpret_cast<uint32_t>(a);
            g_loop_ebp = reinterpret_cast<uint32_t>(st + 0xE000);
            g_loop_esp = reinterpret_cast<uint32_t>(st + 0xE000 - 0x2400);
            if (pass) put_jump(0x99E9B0, reinterpret_cast<const void*>(&ffx_particle_loop_entry));
            else std::memcpy(reinterpret_cast<void*>(0x99E9B0), orig_site, 12);
            run_loop_at(0x99E980);
            if (!pass) std::memcpy(r1, g_loop_regs, 12);
        }
        std::memcpy(reinterpret_cast<void*>(0x99E9B0), orig_site, 12);
        iters += n;
        {   // the loop's particle pointer (ebp-0x1488) points into each run's own particles: compare as an offset
            uint32_t q;
            std::memcpy(&q, stack1 + 0xE000 - 0x1488, 4);
            const uint32_t b1 = reinterpret_cast<uint32_t>(parts1), b2 = reinterpret_cast<uint32_t>(parts2);
            if (q >= b1 && q < b1 + sizeof(parts1)) { q += b2 - b1; std::memcpy(stack1 + 0xE000 - 0x1488, &q, 4); }
        }
        if (std::memcmp(stack1 + 0xE000 - 0x2400, stack2 + 0xE000 - 0x2400, 0x2400) != 0 && g_fail < 20) {
            int shown = 0;
            for (int o = 0; o < 0x2400 && shown < 6; o += 4) {
                uint32_t x, y;
                std::memcpy(&x, stack1 + 0xE000 - 0x2400 + o, 4);
                std::memcpy(&y, stack2 + 0xE000 - 0x2400 + o, 4);
                if (x != y) { std::printf("  n %u frame ebp-%#x: %08X vs %08X\n", n, 0x2400 - o, x, y); ++shown; }
            }
        }
        same(stack1 + 0xE000 - 0x2400, stack2 + 0xE000 - 0x2400, 0x2400, "particle loop frame", it);
        same(parts1, parts2, sizeof(parts1), "particle loop particles", it);
        same(out1, out2, sizeof(out1), "particle loop output", it);
        // eax/ecx/edx: pointers into out1/out2 or the frames compare as offsets
        for (int k = 0; k < 3; ++k) {
            const uint32_t d1 = reinterpret_cast<uint32_t>(out1), d2 = reinterpret_cast<uint32_t>(out2);
            const uint32_t s1 = reinterpret_cast<uint32_t>(stack1), s2 = reinterpret_cast<uint32_t>(stack2);
            if (r1[k] >= d1 && r1[k] < d1 + sizeof(out1)) r1[k] += d2 - d1;
            else if (r1[k] >= s1 && r1[k] < s1 + sizeof(stack1)) r1[k] += s2 - s1;
        }
        if (std::memcmp(r1, g_loop_regs, 12) != 0) {
            if (g_fail < 20) std::printf("particle loop registers differ at %d (n %u): %08X %08X %08X vs %08X %08X %08X\n", it, n, r1[0], r1[1], r1[2], g_loop_regs[0], g_loop_regs[1], g_loop_regs[2]);
            ++g_fail;
        }
    }
    std::printf("particle loop: 30000 runs, %ld particles, %d mismatches so far\n", iters, g_fail);
}

// 0xA4D420's particle update loop (entered at 0xA4DEE1, the loop's exit 0xA4FB9A comes back here) on a fake frame:
// the original, and with 0xA4DEE8 jumping to ffx_update_loop_entry. Arrays of 1..8 groups of four; the branch flag
// [ebx+0x4C] and the second array [ebx+0x28] (zero: the first array) both ways; the parameter objects of type 4 (the
// four-lane curve, with a key stream) or a probe type (table slot 29) whose result mixes every input and counts its
// calls. The whole frame above esp, all arrays, the argument block and eax/ecx/edx at the exit compared.
static uint32_t g_probe_calls;
static void* __cdecl probe_eval(uint32_t* out, const uint32_t* a, const uint32_t* self, uint32_t b, const uint32_t* c) {
    ++g_probe_calls;
    for (int i = 0; i < 4; ++i) {
        out[i] = (a[i] * 0x9E3779B1u) ^ ((c[i] << 7) | (c[i] >> 25)) ^ (b * (i + 3)) ^ self[2] ^ (g_probe_calls * 0x85EBCA6Bu);
    }
    return out;
}
static void test_update_loop() {
    static uint8_t pre[58];
    std::memcpy(pre, reinterpret_cast<void*>(0xA4DEE8), sizeof(pre));
    put_jump(0xA4FB9A, reinterpret_cast<const void*>(&loop_back));
    *reinterpret_cast<uint32_t*>(0x112D618 + 29 * 4) = reinterpret_cast<uint32_t>(&probe_eval);
    alignas(16) static uint8_t stack1[0x10000], stack2[0x10000], arr1[9][8 * 16], arr2[9][8 * 16];
    alignas(16) static uint32_t args1[0x20], args2[0x20], pobj1[2][4], pobj2[2][4], stream[64];
    long groups = 0;
    for (int it = 0; it < 20000; ++it) {
        const uint32_t count = 1 + rng() % 8;
        fill(reinterpret_cast<uint32_t*>(stack1 + 0xE000 - 0x1A00), 0x1A00 / 4);
        for (int k = 0; k < 9; ++k) {
            fill(reinterpret_cast<uint32_t*>(arr1[k]), 8 * 4);
            if (rng() & 1) {
                for (int i = 0; i < 32; ++i) { float f = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 64.0f; std::memcpy(arr1[k] + 4 * i, &f, 4); }
            }
        }
        // a key stream for type 4: n, keys, values
        const uint32_t nk = 1 + rng() % 5;
        stream[0] = nk;
        float key = -2.0f;
        for (uint32_t i = 0; i < nk; ++i) { key += static_cast<float>(rng() % 40) / 10.0f; std::memcpy(&stream[1 + i], &key, 4); stream[1 + nk + i] = rnd_bits(); }
        for (int k = 0; k < 2; ++k) {
            pobj1[k][0] = (rng() & 1) ? 4u : 29u;
            pobj1[k][1] = reinterpret_cast<uint32_t>(stream);
            pobj1[k][2] = rng();
            pobj1[k][3] = 0;
        }
        fill(args1, 0x20);
        std::memcpy(stack2, stack1, sizeof(stack1));
        std::memcpy(arr2, arr1, sizeof(arr1));
        std::memcpy(pobj2, pobj1, sizeof(pobj1));
        const bool flag = (rng() & 1) != 0, second = (rng() & 1) != 0;
        uint32_t r1[3];
        uint32_t calls1 = 0;
        for (int pass = 0; pass < 2; ++pass) {
            uint8_t* st = pass ? stack2 : stack1;
            uint32_t* a = pass ? args2 : args1;
            auto& arr = pass ? arr2 : arr1;
            auto& po = pass ? pobj2 : pobj1;
            if (pass) std::memcpy(args2, args1, sizeof(args1));
            const int slots[8] = {2, 3, 4, 5, 6, 7, 9, 10};  // ebx + 8 .. 0x1C, 0x24, 0x28
            for (int k = 0; k < 8; ++k) a[slots[k]] = reinterpret_cast<uint32_t>(arr[k]);
            if (!second) a[10] = 0;
            a[8] = count;                                           // ebx + 0x20
            a[17] = reinterpret_cast<uint32_t>(po[0]);              // ebx + 0x44
            a[18] = reinterpret_cast<uint32_t>(po[1]);              // ebx + 0x48
            a[19] = flag ? 1u : 0u;                                 // ebx + 0x4C
            g_loop_ebx = reinterpret_cast<uint32_t>(a);
            g_loop_ebp = reinterpret_cast<uint32_t>(st + 0xE000);
            g_loop_esp = reinterpret_cast<uint32_t>(st + 0xE000 - 0x1A00);
            if (pass) put_jump(0xA4DEE8, reinterpret_cast<const void*>(&ffx_update_loop_entry));
            else std::memcpy(reinterpret_cast<void*>(0xA4DEE8), pre, sizeof(pre));
            g_probe_calls = 0;
            run_loop_at(0xA4DEE1);
            if (!pass) { std::memcpy(r1, g_loop_regs, 12); calls1 = g_probe_calls; }
        }
        std::memcpy(reinterpret_cast<void*>(0xA4DEE8), pre, sizeof(pre));
        groups += count;
        // pointers into each run's own buffers compare as offsets: frame cells, the argument block, registers
        const uint32_t lo1[4] = {reinterpret_cast<uint32_t>(stack1), reinterpret_cast<uint32_t>(arr1), reinterpret_cast<uint32_t>(pobj1), reinterpret_cast<uint32_t>(args1)};
        const uint32_t lo2[4] = {reinterpret_cast<uint32_t>(stack2), reinterpret_cast<uint32_t>(arr2), reinterpret_cast<uint32_t>(pobj2), reinterpret_cast<uint32_t>(args2)};
        const uint32_t len[4] = {sizeof(stack1), sizeof(arr1), sizeof(pobj1), sizeof(args1)};
        auto map = [&](uint32_t v) {
            for (int b = 0; b < 4; ++b) if (v >= lo1[b] && v < lo1[b] + len[b]) return v - lo1[b] + lo2[b];
            return v;
        };
        // a cell matches when equal, or equal once a pointer into run 1's buffers is moved to run 2's (a random
        // value can fall in those ranges too, so only cells that differ are mapped)
        for (int o = 0; o < 0x1A00; o += 4) {
            uint32_t v, w;
            std::memcpy(&v, stack1 + 0xE000 - 0x1A00 + o, 4);
            std::memcpy(&w, stack2 + 0xE000 - 0x1A00 + o, 4);
            if (v != w && map(v) == w) std::memcpy(stack1 + 0xE000 - 0x1A00 + o, &w, 4);
        }
        for (int k = 0; k < 0x20; ++k) if (args1[k] != args2[k] && map(args1[k]) == args2[k]) args1[k] = args2[k];
        for (int k = 0; k < 3; ++k) if (r1[k] != g_loop_regs[k] && map(r1[k]) == g_loop_regs[k]) r1[k] = g_loop_regs[k];
        if (std::memcmp(stack1 + 0xE000 - 0x1A00, stack2 + 0xE000 - 0x1A00, 0x1A00) != 0 && g_fail < 20) {
            int shown = 0;
            for (int o = 0; o < 0x1A00 && shown < 6; o += 4) {
                uint32_t x, y;
                std::memcpy(&x, stack1 + 0xE000 - 0x1A00 + o, 4);
                std::memcpy(&y, stack2 + 0xE000 - 0x1A00 + o, 4);
                if (x != y) { std::printf("  count %u flag %d frame ebp-%#x: %08X vs %08X\n", count, flag, 0x1A00 - o, x, y); ++shown; }
            }
        }
        same(stack1 + 0xE000 - 0x1A00, stack2 + 0xE000 - 0x1A00, 0x1A00, "update loop frame", it);
        same(arr1, arr2, sizeof(arr1), "update loop arrays", it);
        same(args1, args2, sizeof(args1), "update loop arguments", it);
        if (calls1 != g_probe_calls) { if (g_fail < 20) std::printf("update loop probe calls %u vs %u\n", calls1, g_probe_calls); ++g_fail; }
        if (std::memcmp(r1, g_loop_regs, 12) != 0) {
            if (g_fail < 20) std::printf("update loop registers differ at %d (count %u): %08X %08X %08X vs %08X %08X %08X\n", it, count, r1[0], r1[1], r1[2], g_loop_regs[0], g_loop_regs[1], g_loop_regs[2]);
            ++g_fail;
        }
    }
    std::printf("update loop: 20000 runs, %ld groups of four, %d mismatches so far\n", groups, g_fail);
}

// _CIsqrt (0xAC1800) vs crt_cisqrt: st(0) = a float, double or extended value (positive, zero, denormal, huge, tiny
// below the double range), under the control words 0x27F, 0x07F, 0x37F, 0x127F and the other rounding modes; the
// 80-bit result, the status word (cleared before) and the control word after compared. (Negative, infinite and NaN
// arguments take the same original code in both, through its error handling, which needs the runtime set up.)
static uint32_t g_sq_fn;
static uint16_t g_sq_cw, g_sq_sw, g_sq_cw_after;
static uint32_t g_sq_preset;
static const double g_sq_three = 3.0;
static uint8_t g_sq_in[10], g_sq_out[10];
__declspec(naked) static void call_sqrt() {
    __asm {
        fninit
        fldcw word ptr [g_sq_cw]
        fnclex
        cmp dword ptr [g_sq_preset], 0
        je no_preset
        fld1
        fdiv qword ptr [g_sq_three]
        fstp st(0)
    no_preset:
        fld tbyte ptr [g_sq_in]
        call dword ptr [g_sq_fn]
        fnstsw word ptr [g_sq_sw]
        fnstcw word ptr [g_sq_cw_after]
        fstp tbyte ptr [g_sq_out]
        fninit
        ret
    }
}
static void test_cisqrt() {
    const uint16_t cws[8] = {0x27F, 0x07F, 0x37F, 0x127F, 0x67F, 0xA7F, 0xE7F, 0x47F};
    long fast = 0;
    for (int it = 0; it < 400000; ++it) {
        uint8_t in[10] = {};
        // build the 80-bit value directly: sign 0, exponent and mantissa random within ranges
        uint64_t mant = (static_cast<uint64_t>(rng()) << 32) | rng();
        uint32_t ex;
        switch (rng() % 6) {
        case 0: ex = 0x3FFF - 126 + rng() % 254; mant &= 0xFFFFFF0000000000ull; break;   // a float's
        case 1: ex = 0x3FFF - 1022 + rng() % 2046; mant &= 0xFFFFFFFFFFFFF800ull; break; // a double's
        case 2: ex = 0x3FFF - 1100 + rng() % 2200; break;                              // extended, also past double
        case 3: ex = 0; mant = 0; break;                                                // +0
        case 4: ex = 0x3FFF - 1023 - rng() % 52; mant &= 0xFFFFFFFFFFFFF800ull; break;  // tiny (double denormal range)
        default: ex = 0x3FFF + rng() % 64 - 32; break;
        }
        if (ex) mant |= 0x8000000000000000ull;
        std::memcpy(in, &mant, 8);
        in[8] = static_cast<uint8_t>(ex & 0xFF);
        in[9] = static_cast<uint8_t>((ex >> 8) & 0x7F);
        const uint16_t cw = cws[rng() % 8];
        g_sq_preset = rng() & 1;
        uint8_t o1[10];
        uint16_t sw1, cwa1;
        for (int pass = 0; pass < 2; ++pass) {
            std::memcpy(g_sq_in, in, 10);
            g_sq_cw = cw;
            g_sq_fn = pass ? reinterpret_cast<uint32_t>(&crt_cisqrt) : 0xAC1800;
            call_sqrt();
            if (!pass) { std::memcpy(o1, g_sq_out, 10); sw1 = g_sq_sw; cwa1 = g_sq_cw_after; }
        }
        if ((cw & 0xCFF) == 0x7F && ex) ++fast;
        if (std::memcmp(o1, g_sq_out, 10) != 0 || sw1 != g_sq_sw || cwa1 != g_sq_cw_after) {
            if (g_fail < 20) {
                std::printf("MISMATCH sqrt (it %d cw %04X exp %04X): sw %04X/%04X cw %04X/%04X\n", it, cw, ex, sw1, g_sq_sw, cwa1, g_sq_cw_after);
                std::printf("   in ");
                for (int b = 9; b >= 0; --b) std::printf("%02X", in[b]);
                std::printf("  out ");
                for (int b = 9; b >= 0; --b) std::printf("%02X", o1[b]);
                std::printf(" / ");
                for (int b = 9; b >= 0; --b) std::printf("%02X", g_sq_out[b]);
                std::printf("\n");
            }
            ++g_fail;
        }
    }
    std::printf("runtime square root: 400000 cases (%ld on the fast path's terms), %d mismatches so far\n", fast, g_fail);
}

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "child") != 0) return run_parent(argv[0]);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("image mapped (child)\n");
    probe_orders(0);
    probe_orders(1);

    for (int pass = 0; pass < 2; ++pass) {
    g_nan_heavy = pass == 1;
    std::printf("pass %d (%s)\n", pass, g_nan_heavy ? "half the inputs NaN" : "mixed values");
    const int N = 200000;
    alignas(16) uint32_t a[16], b[16], c[16], o1[16], o2[16], x1[16], x2[16];
    for (int it = 0; it < N; ++it) {
        fill(a, 16); fill(b, 16); fill(c, 16);
        // four-float add / sub / mul / madd
        struct { uintptr_t orig; void* repl; const char* name; } v3[] = {
            {0x4078D0, (void*)&vec4_add, "add"}, {0x417420, (void*)&vec4_sub, "sub"}, {0x5537E0, (void*)&vec4_mul, "mul"},
            {0x984B60, (void*)&ffx_sub, "ffx sub"}};
        for (auto& t : v3) {
            fill(o1, 4); std::memcpy(o2, o1, 16);
            void* r1 = reinterpret_cast<F3>(t.orig)(o1, a, b);
            void* r2 = reinterpret_cast<F3>(t.repl)(o2, a, b);
            same(o1, o2, 16, t.name, it); if (r1 != o1 || r2 != o2) { std::printf("return %s\n", t.name); ++g_fail; }
        }
        fill(o1, 4); std::memcpy(o2, o1, 16);
        reinterpret_cast<F4>(0x553700)(o1, a, b, c);
        vec4_madd(reinterpret_cast<float*>(o2), reinterpret_cast<float*>(a), reinterpret_cast<float*>(b), reinterpret_cast<float*>(c));
        same(o1, o2, 16, "madd", it);
        // madd in place (out = c)
        std::memcpy(x1, c, 16); std::memcpy(x2, c, 16);
        reinterpret_cast<F4>(0x553700)(x1, a, b, x1);
        vec4_madd(reinterpret_cast<float*>(x2), reinterpret_cast<float*>(a), reinterpret_cast<float*>(b), reinterpret_cast<float*>(x2));
        same(x1, x2, 16, "madd in place", it);
        // 3x4 matrix add (custom convention), and ecx kept
        fill(o1, 12); std::memcpy(o2, o1, 48);
        call_mat34add(reinterpret_cast<void*>(0x421E70), o1, a, b);
        const uint32_t e1 = g_ecx_after;
        call_mat34add(reinterpret_cast<void*>(&mat34_add), o2, a, b);
        same(o1, o2, 48, "mat34 add", it);
        if (e1 != g_ecx_after || e1 != reinterpret_cast<uint32_t>(o1) - 0 && g_ecx_after != reinterpret_cast<uint32_t>(o2)) {}
        if (g_ecx_after != reinterpret_cast<uint32_t>(o2)) { std::printf("mat34 add ecx not kept\n"); ++g_fail; }
        // not-equal (custom convention); half the time equal or with +-0 / NaN lanes
        std::memcpy(x1, a, 16);
        if (it & 1) x1[rng() % 4] = rnd_bits();
        if ((it & 7) == 2) { x1[0] ^= 0x80000000u * ((a[0] & 0x7FFFFFFF) == 0); }
        const uint32_t n1 = call_neq(reinterpret_cast<void*>(0x407F50), a, x1);
        const uint32_t n2 = call_neq(reinterpret_cast<void*>(&vec4_not_equal), a, x1);
        if (n1 != n2) { if (g_fail < 20) std::printf("MISMATCH not-equal %u vs %u\n", n1, n2); ++g_fail; }
        // transpose, separate and in place
        fill(o1, 16); std::memcpy(o2, o1, 64);
        reinterpret_cast<F2>(0x423220)(o1, a);
        mat44_transpose(o2, a);
        same(o1, o2, 64, "transpose", it);
        std::memcpy(x1, a, 64); std::memcpy(x2, a, 64);
        reinterpret_cast<F2>(0x423220)(x1, x1);
        mat44_transpose(x2, x2);
        same(x1, x2, 64, "transpose in place", it);
        // 4x4 multiply: separate, out = A, out = B
        fill(o1, 16); std::memcpy(o2, o1, 64);
        reinterpret_cast<F3>(0x4144D0)(o1, a, b);
        mat44_mul(reinterpret_cast<float*>(o2), reinterpret_cast<float*>(a), reinterpret_cast<float*>(b));
        same(o1, o2, 64, "mat44 mul", it);
        std::memcpy(x1, a, 64); std::memcpy(x2, a, 64);
        reinterpret_cast<F3>(0x4144D0)(x1, x1, b);
        mat44_mul(reinterpret_cast<float*>(x2), reinterpret_cast<float*>(x2), reinterpret_cast<float*>(b));
        same(x1, x2, 64, "mat44 mul out=A", it);
        std::memcpy(x1, b, 64); std::memcpy(x2, b, 64);
        reinterpret_cast<F3>(0x4144D0)(x1, a, x1);
        mat44_mul(reinterpret_cast<float*>(x2), reinterpret_cast<float*>(a), reinterpret_cast<float*>(x2));
        if (!same(x1, x2, 64, "mat44 mul out=B", it) && g_fail <= 3) {
            for (int i = 0; i < 16; ++i) {
                float fa, fb, f1, f2;
                std::memcpy(&fa, &a[i], 4); std::memcpy(&fb, &b[i], 4); std::memcpy(&f1, &x1[i], 4); std::memcpy(&f2, &x2[i], 4);
                std::printf("  [%2d] a %08X (%g)  b %08X (%g)  orig %08X (%g)  mine %08X (%g)%s\n", i, a[i], fa, b[i], fb,
                            x1[i], f1, x2[i], f2, x1[i] != x2[i] ? "  <<" : "");
            }
        }
        // 3x4 affine multiply (out, B, A): separate, out = A, out = B
        fill(o1, 12); std::memcpy(o2, o1, 48);
        reinterpret_cast<F3>(0x416100)(o1, b, a);
        mat34_mul(reinterpret_cast<float*>(o2), reinterpret_cast<float*>(b), reinterpret_cast<float*>(a));
        same(o1, o2, 48, "mat34 mul", it);
        std::memcpy(x1, a, 48); std::memcpy(x2, a, 48);
        reinterpret_cast<F3>(0x416100)(x1, b, x1);
        mat34_mul(reinterpret_cast<float*>(x2), reinterpret_cast<float*>(b), reinterpret_cast<float*>(x2));
        same(x1, x2, 48, "mat34 mul out=A", it);
        std::memcpy(x1, b, 48); std::memcpy(x2, b, 48);
        reinterpret_cast<F3>(0x416100)(x1, x1, a);
        mat34_mul(reinterpret_cast<float*>(x2), reinterpret_cast<float*>(x2), reinterpret_cast<float*>(a));
        same(x1, x2, 48, "mat34 mul out=B", it);
        // effects wrappers
        fill(o1, 4); std::memcpy(o2, o1, 16);
        reinterpret_cast<FSet>(0x984890)(o1, a[0], a[1], a[2], a[3]);
        ffx_set4(o2, a[0], a[1], a[2], a[3]);
        same(o1, o2, 16, "ffx set", it);
        fill(o1, 4); std::memcpy(o2, o1, 16);
        reinterpret_cast<FSplat>(0x9847C0)(o1, a[5]);
        ffx_splat(o2, a[5]);
        same(o1, o2, 16, "ffx splat", it);
        // forwarding wrappers (args are pointers to vectors; 64-byte inputs in case a helper reads more), separate and
        // with out = the first input
        {
            using W3 = void*(__cdecl*)(void*, uint32_t, uint32_t, uint32_t);
            struct { uintptr_t orig; void* repl; const char* name; } fw[] = {
                {0x9855C0, (void*)&ffx_fwd3<0x403B10>, "select wrapper"}, {0x985120, (void*)&ffx_fwd2<0x419490>, "419490 wrapper"},
                {0x984F60, (void*)&ffx_fwd2<0x985020>, "985020 wrapper"}, {0x9B6C40, (void*)&ffx_fwd1<0x407720>, "407720 wrapper"},
                {0x9B5990, (void*)&ffx_fwd2<0x4078D0>, "add wrapper"}, {0x984EA0, (void*)&ffx_fwd1<0x430AC0>, "430AC0 wrapper"},
                {0x985500, (void*)&ffx_fwd1<0x4193E0>, "4193E0 wrapper"}, {0x983C40, (void*)&ffx_lerp, "lerp"}};
            for (auto& t : fw) {
                alignas(16) uint32_t p[16], q[16], r[16], i1[16], i2[16];
                fill(p, 16); fill(q, 16); fill(r, 16);
                fill(o1, 4); std::memcpy(o2, o1, 16);
                reinterpret_cast<W3>(t.orig)(o1, (uint32_t)p, (uint32_t)q, (uint32_t)r);
                reinterpret_cast<W3>(t.repl)(o2, (uint32_t)p, (uint32_t)q, (uint32_t)r);
                same(o1, o2, 16, t.name, it);
                std::memcpy(i1, p, 64); std::memcpy(i2, p, 64);
                reinterpret_cast<W3>(t.orig)(i1, (uint32_t)i1, (uint32_t)q, (uint32_t)r);
                reinterpret_cast<W3>(t.repl)(i2, (uint32_t)i2, (uint32_t)q, (uint32_t)r);
                same(i1, i2, 64, t.name, it);
            }
        }
        // saturate: values around 0 and 1 half the time
        {
            if (it & 1) for (int i = 0; i < 4; ++i) { float v = static_cast<float>(static_cast<int>(rng() % 301) - 100) / 100.0f; std::memcpy(&c[i], &v, 4); }
            fill(o1, 4); std::memcpy(o2, o1, 16);
            reinterpret_cast<F2>(0x407720)(o1, c);
            vec4_saturate(reinterpret_cast<float*>(o2), reinterpret_cast<float*>(c));
            same(o1, o2, 16, "saturate", it);
            std::memcpy(x1, c, 16); std::memcpy(x2, c, 16);
            reinterpret_cast<F2>(0x407720)(x1, x1);
            vec4_saturate(reinterpret_cast<float*>(x2), reinterpret_cast<float*>(x2));
            same(x1, x2, 16, "saturate in place", it);
            fill(c, 4);
        }
        // reciprocal square root (0x419890): random bits, and positive values of every size half the time
        {
            if (it & 1) for (int i = 0; i < 4; ++i) c[i] = rng() & 0x7FFFFFFFu;
            fill(o1, 4); std::memcpy(o2, o1, 16);
            reinterpret_cast<F2>(0x419890)(o1, c);
            vec4_rsqrt(reinterpret_cast<float*>(o2), reinterpret_cast<float*>(c));
            same(o1, o2, 16, "rsqrt", it);
            std::memcpy(x1, c, 16); std::memcpy(x2, c, 16);
            reinterpret_cast<F2>(0x419890)(x1, x1);
            vec4_rsqrt(reinterpret_cast<float*>(x2), reinterpret_cast<float*>(x2));
            same(x1, x2, 16, "rsqrt in place", it);
            fill(c, 4);
        }
        // stream read-splat: the output and the stream pointer moved on
        {
            const uint32_t* s1 = a + (it % 8);
            const uint32_t* s2 = s1;
            fill(o1, 4); std::memcpy(o2, o1, 16);
            using RS = void*(__cdecl*)(void*, uint32_t, const uint32_t**);
            reinterpret_cast<RS>(0x983A90)(o1, 0, &s1);
            ffx_read_splat(o2, 0, &s2);
            same(o1, o2, 16, "read-splat", it);
            if (s1 != s2) { std::printf("read-splat stream pointer differs\n"); ++g_fail; }
        }
        // parameter evaluator, through planted fake evaluators of both kinds
        {
            void** table = reinterpret_cast<void**>(0x112D618);
            void* saved0 = table[0];
            void* saved1 = table[1];
            table[0] = reinterpret_cast<void*>(&fake_eval0);
            table[1] = reinterpret_cast<void*>(&fake_eval1);
            alignas(16) uint32_t obj[4] = {it & 1u, rng(), rng(), 0};
            fill(o1, 4); std::memcpy(o2, o1, 16);
            const uint32_t pa = rng(), pb = rng(), pc = rng();
            void* r1 = call_param_eval(reinterpret_cast<void*>(0x9B4650), obj, o1, pa, pb, pc);
            void* r2 = call_param_eval(reinterpret_cast<void*>(&ffx_param_eval), obj, o2, pa, pb, pc);
            same(o1, o2, 16, "parameter evaluator", it);
            if (r1 != o1 || r2 != o2) { std::printf("parameter evaluator return\n"); ++g_fail; }
            table[0] = saved0;
            table[1] = saved1;
        }
        // float-to-integer converters (and the x87 control word left as it was)
        struct { uintptr_t orig; void* repl; const char* name; } ti[] = {
            {0x9B21A0, (void*)&ffx_to_int_1, "to-int x1"}, {0x9B32A0, (void*)&ffx_to_int_65536, "to-int x65536"},
            {0x9B3030, (void*)&ffx_to_int_256, "to-int x256"}, {0x9B3510, (void*)&ffx_to_int_2p24, "to-int x2^24"}};
        for (auto& t : ti) {
            // half the time values in the integer range the game uses (0..255, 0..1, +-large)
            if (it & 1) for (int i = 0; i < 4; ++i) { float v = static_cast<float>(static_cast<int>(rng() % 200001) - 100000) / 97.0f; std::memcpy(&c[i], &v, 4); }
            fill(o1, 4); std::memcpy(o2, o1, 16);
            unsigned short cw1 = 0, cw2 = 0, cw0 = 0;
            __asm fnstcw cw0
            reinterpret_cast<F2>(t.orig)(o1, c);
            __asm fnstcw cw1
            reinterpret_cast<F2>(t.repl)(o2, c);
            __asm fnstcw cw2
            same(o1, o2, 16, t.name, it);
            if (cw1 != cw0 || cw2 != cw0) { std::printf("x87 control word changed by %s\n", t.name); ++g_fail; }
            fill(c, 4);
        }
        fill(o1, 4); std::memcpy(o2, o1, 16);
        reinterpret_cast<FSplat>(0x984920)(o1, a[6]);
        reinterpret_cast<FSplat>(&ffx_splat_x87)(o2, a[6]);
        same(o1, o2, 16, "ffx splat x87", it);
    }
    std::printf("%d iterations, %d mismatches so far\n", N, g_fail);
    }
    test_cull();
    test_intops();
    test_random();
    test_matset();
    test_flush();
    for (int pass = 0; pass < 2; ++pass) {
        g_nan_heavy = pass == 1;
        test_curves();
        if (!g_nan_heavy) {
            test_eased();
            test_eased_real();
        }
        test_glue();
        test_box();
        test_arrays();
        test_particle_loop();
        test_update_loop();
        test_cisqrt();
    }
    return g_fail ? 1 : 0;
}
