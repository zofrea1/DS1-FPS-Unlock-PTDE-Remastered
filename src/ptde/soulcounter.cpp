#include "soulcounter.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kPrologue = 0x00FADA70;
constexpr uint32_t kAdd = 0x00FADCB1;
constexpr uint32_t kShown = 0x108;
constexpr int kSlots = 4;
constexpr double kGapReset = 0.30;
constexpr double kMaxScale = 4.0;

struct Slot {
    void* dialog = nullptr;
    double carry = 0.0;
    double seen = 0.0;
    int sign = 0;
};

Slot g_slots[kSlots];
uint32_t g_stamp[kSlots] = {};
uint32_t g_clock = 0;

double qpc_seconds() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

Slot* slot_for(void* dialog) {
    int oldest = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].dialog == dialog) {
            g_stamp[i] = ++g_clock;
            return &g_slots[i];
        }
        if (g_stamp[i] < g_stamp[oldest]) {
            oldest = i;
        }
    }
    int empty = oldest;
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].dialog == nullptr) {
            empty = i;
            break;
        }
    }
    Slot* s = &g_slots[empty];
    s->dialog = dialog;
    s->carry = 0.0;
    s->seen = 0.0;
    s->sign = 0;
    g_stamp[empty] = ++g_clock;
    return s;
}

float frame_dt(float argument) {
    if (argument > 0.0f && argument < 1.0f) {
        return argument;
    }
    const double step = fixes_last_dt();
    if (step > 0.0 && step < 1.0) {
        return static_cast<float>(step);
    }
    return 1.0f / 30.0f;
}

// esi is the dialog, ecx the game's whole step, [ebp+8] the frame time in seconds.
void __stdcall soul_apply(uint8_t* dialog, int step, float dt) {
    Slot* s = slot_for(dialog);
    const double now = qpc_seconds();
    const double gap = s->seen > 0.0 ? now - s->seen : 0.0;
    s->seen = now;
    const int sign = step > 0 ? 1 : (step < 0 ? -1 : 0);
    if (gap > kGapReset || (sign != 0 && s->sign != 0 && sign != s->sign)) {
        s->carry = 0.0;
    }
    if (sign != 0) {
        s->sign = sign;
    }
    double scale = static_cast<double>(frame_dt(dt)) * 30.0;
    if (!(scale > 0.0)) {
        scale = 1.0;
    }
    if (scale > kMaxScale) {
        scale = kMaxScale;
    }
    const double scaled = static_cast<double>(step) * scale + s->carry;
    const int add = static_cast<int>(scaled);  // toward zero, so the carry keeps the step's sign
    s->carry = scaled - static_cast<double>(add);
    *reinterpret_cast<int*>(dialog + kShown) += add;
}

__declspec(naked) void soul_stub() {
    __asm {
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
        mov eax, [esp + 0x80 + 0x04]
        mov ecx, [esp + 0x80 + 0x18]
        mov edx, [esp + 0x80 + 0x08]
        mov edx, [edx + 8]
        push edx
        push ecx
        push eax
        call soul_apply
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
        push 0FADCB7h
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

bool soulcounter_install() {
    static const uint8_t kPrologueBytes[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    static const uint8_t kBeforeAdd[17] = {0x85, 0xC0, 0x7E, 0x03, 0x41, 0xEB, 0x03, 0x7D, 0x01,
                                           0x49, 0x01, 0x8E, 0x08, 0x01, 0x00, 0x00, 0x8B};
    if (std::memcmp(reinterpret_cast<const void*>(kPrologue), kPrologueBytes, sizeof(kPrologueBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kAdd - 10), kBeforeAdd, sizeof(kBeforeAdd)) != 0) {
        LOG_ERROR("Soul counter at %08X does not match this build. Not patching.", kAdd);
        return false;
    }
    if (!write_jump(kAdd, 6, &soul_stub)) {
        LOG_ERROR("Could not patch the soul counter (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("Soul counter follows the frame time (site %08X, at most 4 steps a hitch)", kAdd);
    return true;
}
