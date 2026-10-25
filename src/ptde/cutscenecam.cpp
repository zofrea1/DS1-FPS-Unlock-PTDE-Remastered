#include "cutscenecam.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kClear = 0x00D09DB2;      // lea eax, [esp+0xC8], before either sample
constexpr uint32_t kClearNext = 0x00D09DB9;
constexpr uint32_t kAmountStore = 0x00D09E2B;
constexpr uint32_t kTimeLoad = 0x00D09DCB;
constexpr uint32_t kPosCmp = 0x00D09E87;     // comiss xmm4, xmm3 (0.5 ? amount)
constexpr uint32_t kPos = 0x00D09E9B;
constexpr uint32_t kRotCmp = 0x00D09F06;
constexpr uint32_t kRot = 0x00D09F1A;

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kFrame = 1.0f / 30.0f;
constexpr float kMaxGap = 1.5f * kFrame;  // key times are seconds (frame index / 30)
constexpr float kPosCut2 = 1.0f;          // 1 m, squared
constexpr float kRotCut2 = 0.25f;         // 0.5 rad, squared

// Set at the start of each call, then by the position sample. The rotation sample of a later call
// does not see a cut whose position block this call skipped.
uint8_t g_pos_valid = 0;
uint8_t g_pos_cut = 0;
uint32_t g_esp = 0;
uint8_t g_did = 0;

float load_f(const uint8_t* p) {
    float v = 0.0f;
    std::memcpy(&v, p, 4);
    return v;
}

void store_f(uint8_t* p, float v) {
    std::memcpy(p, &v, 4);
}

bool amount_inside(float amount) {
    return amount > 0.0f && amount < 1.0f;
}

bool keys_close(float t_a, float t_b) {
    const float gap = std::fabs(t_b - t_a);
    return gap > 0.0f && gap <= kMaxGap;
}

// Writes the blended position over the slot the game fills at esp+0x50. A step longer than 1 m sets
// g_pos_cut so this call's rotation sample keeps the nearest key too.
bool __stdcall cutscene_blend_pos(uint8_t* s) {
    g_pos_valid = 1;
    g_pos_cut = 0;
    const float amount = load_f(s + 0x4C);
    if (!keys_close(load_f(s + 0x80), load_f(s + 0x84)) || !amount_inside(amount)) {
        return false;
    }
    float a[4];
    float b[4];
    std::memcpy(a, s + 0x10, sizeof(a));
    std::memcpy(b, s + 0x20, sizeof(b));
    const float dx = b[0] - a[0];
    const float dy = b[1] - a[1];
    const float dz = b[2] - a[2];
    const float dist2 = dx * dx + dy * dy + dz * dz;
    if (!(dist2 <= kPosCut2)) {
        g_pos_cut = 1;
        return false;
    }
    float out[4];
    out[0] = a[0] + dx * amount;
    out[1] = a[1] + dy * amount;
    out[2] = a[2] + dz * amount;
    out[3] = amount < 0.5f ? a[3] : b[3];
    std::memcpy(s + 0x50, out, sizeof(out));
    return true;
}

// Writes the blended angles over key A and tells the stub to point eax there. The game wraps the angles.
bool __stdcall cutscene_blend_rot(uint8_t* s) {
    if (g_pos_valid != 0 && g_pos_cut != 0) {
        return false;
    }
    const float amount = load_f(s + 0x4C);
    if (!keys_close(load_f(s + 0x80), load_f(s + 0x84)) || !amount_inside(amount)) {
        return false;
    }
    float a[4];
    float b[4];
    std::memcpy(a, s + 0x10, sizeof(a));
    std::memcpy(b, s + 0x30, sizeof(b));
    float delta[3];
    float n2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        delta[i] = std::remainder(b[i] - a[i], kTwoPi);
        n2 += delta[i] * delta[i];
    }
    if (!(n2 <= kRotCut2)) {
        return false;
    }
    float out[4];
    out[0] = a[0] + delta[0] * amount;
    out[1] = a[1] + delta[1] * amount;
    out[2] = a[2] + delta[2] * amount;
    out[3] = amount < 0.5f ? a[3] : b[3];
    std::memcpy(s + 0x10, out, sizeof(out));
    return true;
}

__declspec(naked) void pos_stub() {
    __asm {
        mov dword ptr [g_esp], esp
        pushfd
        pushad
        sub esp, 0x80
        movups xmmword ptr [esp + 0x00], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        push dword ptr [g_esp]
        call cutscene_blend_pos
        mov byte ptr [g_did], al
        movups xmm0, xmmword ptr [esp + 0x00]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        cmp byte ptr [g_did], 0
        jne pos_blended
        popfd
        lea eax, [esp + 0x10]
        ja pos_key_b
        lea eax, [esp + 0x20]
    pos_key_b:
        movq xmm0, qword ptr [eax]
        movq qword ptr [esp + 0x50], xmm0
        movq xmm0, qword ptr [eax + 8]
        movq qword ptr [esp + 0x58], xmm0
        push 0D09EBAh
        ret
    pos_blended:
        popfd
        mov eax, dword ptr [g_esp]
        movq xmm0, qword ptr [eax + 0x58]
        push 0D09EBAh
        ret
    }
}

__declspec(naked) void rot_stub() {
    __asm {
        mov dword ptr [g_esp], esp
        pushfd
        pushad
        sub esp, 0x80
        movups xmmword ptr [esp + 0x00], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        push dword ptr [g_esp]
        call cutscene_blend_rot
        mov byte ptr [g_did], al
        movups xmm0, xmmword ptr [esp + 0x00]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        cmp byte ptr [g_did], 0
        jne rot_blended
        popfd
        lea eax, [esp + 0x10]
        ja rot_key_b
        lea eax, [esp + 0x30]
    rot_key_b:
        push 0D09F24h
        ret
    rot_blended:
        popfd
        mov eax, dword ptr [g_esp]
        add eax, 0x10
        push 0D09F24h
        ret
    }
}

bool write_jump(uint32_t site, size_t size, const void* dest) {
    DWORD old = 0;
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(dest) - (site + 5));
    p[0] = 0xE9;
    std::memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < size; ++i) {
        p[i] = 0x90;
    }
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

}  // namespace

bool cutscenecam_install() {
    static const uint8_t kClearBytes[7] = {0x8D, 0x84, 0x24, 0xC8, 0x00, 0x00, 0x00};
    static const uint8_t kAmountBytes[6] = {0xF3, 0x0F, 0x11, 0x5C, 0x24, 0x4C};
    static const uint8_t kTimeBytes[18] = {0xF3, 0x0F, 0x10, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00,
                                           0xF3, 0x0F, 0x10, 0x94, 0x24, 0x84, 0x00, 0x00, 0x00};
    static const uint8_t kCmpBytes[3] = {0x0F, 0x2F, 0xE3};
    static const uint8_t kPosBytes[31] = {0x8D, 0x44, 0x24, 0x10, 0x77, 0x04, 0x8D, 0x44, 0x24, 0x20, 0xF3, 0x0F,
                                          0x7E, 0x00, 0x66, 0x0F, 0xD6, 0x44, 0x24, 0x50, 0xF3, 0x0F, 0x7E, 0x40,
                                          0x08, 0x66, 0x0F, 0xD6, 0x44, 0x24, 0x58};
    static const uint8_t kRotBytes[10] = {0x8D, 0x44, 0x24, 0x10, 0x77, 0x04, 0x8D, 0x44, 0x24, 0x30};
    if (std::memcmp(reinterpret_cast<const void*>(kClear), kClearBytes, sizeof(kClearBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kAmountStore), kAmountBytes, sizeof(kAmountBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kTimeLoad), kTimeBytes, sizeof(kTimeBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kPosCmp), kCmpBytes, sizeof(kCmpBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kRotCmp), kCmpBytes, sizeof(kCmpBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kPos), kPosBytes, sizeof(kPosBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kRot), kRotBytes, sizeof(kRotBytes)) != 0) {
        LOG_ERROR("Cutscene camera sites do not match this build. Not patching.");
        return false;
    }
    // Clears the position-cut flag on entry, then runs the lea this jump replaces.
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) {
        LOG_ERROR("Could not allocate the cutscene camera cave (Win32=%lu)", GetLastError());
        return false;
    }
    uint8_t* p = cave;
    const uint32_t flag = reinterpret_cast<uint32_t>(&g_pos_valid);
    *p++ = 0xC6;
    *p++ = 0x05;
    std::memcpy(p, &flag, 4);
    p += 4;
    *p++ = 0x00;
    static const uint8_t kLea[7] = {0x8D, 0x84, 0x24, 0xC8, 0x00, 0x00, 0x00};
    std::memcpy(p, kLea, sizeof(kLea));
    p += sizeof(kLea);
    *p++ = 0xE9;
    const int32_t back = static_cast<int32_t>(kClearNext - (reinterpret_cast<uint32_t>(p) + 4));
    std::memcpy(p, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), cave, 32);
    if (!write_jump(kClear, sizeof(kClearBytes), cave) || !write_jump(kPos, sizeof(kPosBytes), &pos_stub) ||
        !write_jump(kRot, sizeof(kRotBytes), &rot_stub)) {
        LOG_ERROR("Could not patch the cutscene camera (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("Cutscene camera blends between 30 FPS keys (sites %08X, %08X)", kPos, kRot);
    return true;
}
