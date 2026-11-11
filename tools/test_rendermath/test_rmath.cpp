// Runs PTDE's original vector/matrix helpers (the game's code mapped at 0x400000) and FastRenderMath's
// replacements on the same random inputs, and compares every output bit, the return values, aliasing (out = an
// input) and the registers the custom-convention originals keep.
#include <windows.h>

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
    return g_fail ? 1 : 0;
}
