#include "camera.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

constexpr uint32_t kFollowUpdate = 0x00F02720;  // ChrFollowCam::Update(this, dt, chr, extra), callee pops 16
constexpr uint32_t kCallSites[] = {0x00F148DD, 0x00F148FE};

// Per-frame blend weights in the follow camera object.
constexpr uint32_t kFields[] = {0x238, 0x1BC};
constexpr int kFieldCount = 2;

using FollowFn = void(__stdcall*)(void*, float, void*, void*);
FollowFn g_orig = reinterpret_cast<FollowFn>(kFollowUpdate);
volatile long g_enabled = 1;

struct Field {
    float original = 0.0f;
    float written = -1.0f;
};
struct Camera {
    void* object = nullptr;
    Field field[kFieldCount];
    bool logged = false;
};
Camera g_cameras[4];

Camera* camera_slot(void* object) {
    Camera* free_slot = nullptr;
    for (auto& c : g_cameras) {
        if (c.object == object) return &c;
        if (!c.object && !free_slot) free_slot = &c;
    }
    if (!free_slot) free_slot = &g_cameras[0];
    *free_slot = Camera();
    free_slot->object = object;
    return free_slot;
}

// The value the game means for a weight after n frames' worth of smoothing in one step. Weights
// up to 0.5 are blend amounts (new = old + (target - old) * k); larger values are kept per-frame
// multipliers of the remaining error (error *= m), which scale by a plain power.
float scaled(float original, double n) {
    if (!(original > 0.0f) || original >= 1.0f) return original;
    if (original <= 0.5f) return static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(original), n));
    return static_cast<float>(std::pow(static_cast<double>(original), n));
}

bool read_field(void* object, uint32_t offset, float* out) {
    __try {
        *out = *reinterpret_cast<volatile float*>(static_cast<uint8_t*>(object) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_field(void* object, uint32_t offset, float value) {
    __try {
        *reinterpret_cast<volatile float*>(static_cast<uint8_t*>(object) + offset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void scale_camera(void* object, float dt) {
    if (!object) return;
    double n = static_cast<double>(dt) * 30.0;
    if (!(n > 0.02)) n = 0.02;
    if (n > 10.0) n = 10.0;
    Camera* cam = camera_slot(object);
    for (int i = 0; i < kFieldCount; ++i) {
        float value = 0.0f;
        if (!read_field(object, kFields[i], &value)) return;
        Field& f = cam->field[i];
        if (value != f.written) {
            f.original = value;  // the game (re)wrote it: that is the 30 FPS value
        }
        const float target = g_enabled ? scaled(f.original, n) : f.original;
        f.written = target;
        write_field(object, kFields[i], target);
    }
    if (!cam->logged) {
        cam->logged = true;
        LOG_INFO("Follow camera %p: blend weights +0x238=%.4f +0x1BC=%.4f (30 FPS values)", object,
                 cam->field[0].original, cam->field[1].original);
    }
}

// ---- Gains read straight from the camera object inside the update --------------------------
//
// Besides the two weights above, ChrFollowCam::Update blends several more values toward a target by
// a weight once per frame, read from the camera at the point of use:
//   [+0x1C0] / [+0x1C4]  look-at point toward its target while locked on (0.4 / 0.3)
//   [+0x1A4] / [+0x1B0]  the same with no lock-on (0.1 / 0.03; +0x1A4 itself eases toward +0x1A8)
//   [+0x190]             pivot following the character (0.1; eases toward +0x194)
//   [+0x1A0]             yaw settling back to the reference yaw (0.3, an x87 fmul)
//   [+0x234]             automatic turn toward the walking direction (0.06)
//   [+0x288]             stick input smoothing
//   [+0x320]             camera parameter changes: distance, height, look-at offset, FOV (0.05)
// and the double 0.1 at 0x11E7CD0 eases the camera distance [+0x184] toward [+0x188].
// None use the frame time, so a lock-on pan (which goes through the yaw/pitch weight AND the look-at
// weights) still finished early with only the first two scaled. Each load is redirected through a
// stub that returns 1 - (1 - w)^n for n = dt * 30, computed with the x87 unit so no registers other
// than the destination change.
float g_gain_n = 1.0f;      // dt * 30 of the update in flight
float g_fmul_tmp = 0.0f;    // operand for the redirected fmul
alignas(8) double g_dist_k = 0.1;

// stdcall(raw weight bits) -> eax = scaled weight bits. Preserves every other register and the flags.
__declspec(naked) void gain_thunk() {
    __asm {
        mov eax, [esp + 4]
        pushfd
        cmp dword ptr [g_enabled], 0
        je done
        test eax, eax
        jle done                 // w <= 0 (or negative): unchanged
        cmp eax, 0x3F800000
        jge done                 // w >= 1: unchanged
        sub esp, 4
        fld dword ptr [g_gain_n] // n
        fld1                     // 1, n
        fsub dword ptr [esp + 12] // 1 - w, n
        fyl2x                    // y = n * log2(1 - w)
        fld st(0)                // y, y
        frndint                  // i, y
        fsub st(1), st           // i, f = y - i
        fxch st(1)               // f, i
        f2xm1                    // 2^f - 1, i
        fld1
        faddp st(1), st          // 2^f, i
        fscale                   // 2^y, i
        fstp st(1)               // (1 - w)^n
        fld1
        fsubrp st(1), st         // 1 - (1 - w)^n
        fstp dword ptr [esp]
        mov eax, [esp]
        add esp, 4
    done:
        popfd
        ret 4
    }
}

struct GainSite {
    uint32_t va;
    uint32_t offset;  // camera field
    int xmm;          // destination register of `movss xmmN, [ebx+offset]`, or -1 for `fmul dword ptr [ebx+offset]`
};
const GainSite kGainSites[] = {
    {0x00F0B5F3, 0x1A4, 0}, {0x00F0B5FD, 0x1C0, 0}, {0x00F0B61A, 0x1B0, 1}, {0x00F0B624, 0x1C4, 1},
    {0x00F04FA5, 0x190, 0}, {0x00F079D3, 0x1A0, -1}, {0x00F075D2, 0x234, 2}, {0x00F076CA, 0x234, 2},
    {0x00F06EEB, 0x288, 2}, {0x00F06F31, 0x288, 2}, {0x00F02814, 0x320, 2}, {0x00F02859, 0x320, 2},
    {0x00F02892, 0x320, 2}, {0x00F028D1, 0x320, 2}, {0x00F0297B, 0x320, 2},
};
constexpr uint32_t kDistSite = 0x00F029B4;   // mulsd xmm1, qword ptr [0x11E7CD0]  (0.1)
constexpr uint32_t kDistConst = 0x011E7CD0;

bool expected_load(const GainSite& g, uint8_t* bytes, size_t* size) {
    if (g.xmm < 0) {
        bytes[0] = 0xD8;  // fmul dword ptr [ebx+disp32]
        bytes[1] = 0x8B;
        std::memcpy(bytes + 2, &g.offset, 4);
        *size = 6;
    } else {
        bytes[0] = 0xF3;  // movss xmmN, dword ptr [ebx+disp32]
        bytes[1] = 0x0F;
        bytes[2] = 0x10;
        bytes[3] = static_cast<uint8_t>(0x83 | (g.xmm << 3));
        std::memcpy(bytes + 4, &g.offset, 4);
        *size = 8;
    }
    return true;
}

bool install_gain_sites() {
    for (const GainSite& g : kGainSites) {
        uint8_t want[8];
        size_t size = 0;
        expected_load(g, want, &size);
        if (std::memcmp(reinterpret_cast<const void*>(g.va), want, size) != 0) {
            LOG_ERROR("Camera gain load at %08X does not match this build. Not patching the camera gains.", g.va);
            return false;
        }
    }
    const uint8_t dist_expect[4] = {0xF2, 0x0F, 0x59, 0x0D};
    uint32_t dist_operand = 0;
    std::memcpy(&dist_operand, reinterpret_cast<const void*>(kDistSite + 4), 4);
    if (std::memcmp(reinterpret_cast<const void*>(kDistSite), dist_expect, 4) != 0 || dist_operand != kDistConst) {
        LOG_ERROR("Camera distance smoothing at %08X does not match this build. Not patching the camera gains.",
                  kDistSite);
        return false;
    }
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x400, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) return false;
    uint8_t* p = cave;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    };
    auto emit32 = [&](uint32_t v) {
        std::memcpy(p, &v, 4);
        p += 4;
    };
    auto emit_rel = [&](uint32_t target) {
        emit32(target - (reinterpret_cast<uint32_t>(p) + 4));
    };
    for (const GainSite& g : kGainSites) {
        uint8_t* stub = p;
        const size_t size = g.xmm < 0 ? 6 : 8;
        emit({0x50});                                   // push eax
        emit({0xFF, 0xB3});                             // push dword ptr [ebx+offset]
        emit32(g.offset);
        emit({0xE8});                                   // call gain_thunk
        emit_rel(reinterpret_cast<uint32_t>(&gain_thunk));
        if (g.xmm < 0) {
            emit({0xA3});                               // mov [g_fmul_tmp], eax
            emit32(reinterpret_cast<uint32_t>(&g_fmul_tmp));
            emit({0x58});                               // pop eax
            emit({0xD8, 0x0D});                         // fmul dword ptr [g_fmul_tmp]
            emit32(reinterpret_cast<uint32_t>(&g_fmul_tmp));
        } else {
            emit({0x66, 0x0F, 0x6E, static_cast<uint8_t>(0xC0 | (g.xmm << 3))});  // movd xmmN, eax
            emit({0x58});                               // pop eax
        }
        emit({0xE9});                                   // jmp back
        emit_rel(g.va + static_cast<uint32_t>(size));

        uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(stub) - (g.va + 5));
        std::memcpy(jump + 1, &rel, 4);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(g.va), size, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(reinterpret_cast<void*>(g.va), jump, size);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g.va), size);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(g.va), size, old, &ignored);
    }
    FlushInstructionCache(GetCurrentProcess(), cave, 0x400);
    const uint32_t dist = reinterpret_cast<uint32_t>(&g_dist_k);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kDistSite + 4), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(kDistSite + 4), &dist, 4);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kDistSite), 8);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(kDistSite + 4), 4, old, &ignored);
    LOG_INFO("Follow camera: %u more blend weights follow the frame time (look-at, pivot, yaw settle, auto turn, "
             "stick, parameter changes, distance)",
             static_cast<unsigned>(sizeof(kGainSites) / sizeof(kGainSites[0])) + 1);
    return true;
}

void __stdcall follow_stub(void* camera, float dt, void* chr, void* extra) {
    double n = static_cast<double>(dt) * 30.0;
    if (!(n > 0.02)) n = 0.02;
    if (n > 10.0) n = 10.0;
    g_gain_n = static_cast<float>(n);
    g_dist_k = g_enabled ? 1.0 - std::pow(1.0 - 0.1, n) : 0.1;
    scale_camera(camera, dt);
    g_orig(camera, dt, chr, extra);
}

}  // namespace

bool camera_install() {
    for (uint32_t site : kCallSites) {
        auto* p = reinterpret_cast<uint8_t*>(site);
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, 4);
        if (p[0] != 0xE8 || static_cast<uint32_t>(site + 5 + rel) != kFollowUpdate) {
            LOG_ERROR("Camera call site %08X does not call %08X. Not patching the camera.", site, kFollowUpdate);
            return false;
        }
    }
    for (uint32_t site : kCallSites) {
        auto* p = reinterpret_cast<uint8_t*>(site);
        DWORD old = 0;
        if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            LOG_ERROR("Could not unprotect the camera call at %08X (Win32=%lu)", site, GetLastError());
            return false;
        }
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&follow_stub) - (site + 5));
        std::memcpy(p + 1, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        DWORD ignored = 0;
        VirtualProtect(p + 1, 4, old, &ignored);
    }
    LOG_INFO("Follow camera smoothing follows the frame time (2 call sites)");
    install_gain_sites();
    return true;
}

void camera_set_enabled(bool on) {
    g_enabled = on ? 1 : 0;
}

bool camera_enabled() {
    return g_enabled != 0;
}
