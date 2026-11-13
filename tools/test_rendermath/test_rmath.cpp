// Runs PTDE's original vector/matrix helpers (the game's code mapped at 0x400000) and FastRenderMath's
// replacements on the same random inputs, and compares every output bit, the return values, aliasing (out = an
// input) and the registers the custom-convention originals keep.
#include <windows.h>

#include <cmath>
#include <cstdint>
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

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "child") != 0) return run_parent(argv[0]);
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
    return g_fail ? 1 : 0;
}
