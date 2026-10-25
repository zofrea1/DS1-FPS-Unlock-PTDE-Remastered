#include "slidecontact.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kStore = 0x00E298BB;
constexpr uint32_t kMerge = 0x00E298AF;
constexpr int kSlots = 64;
constexpr double kHold = 1.0 / 30.0;
constexpr double kGapReset = 0.25;

struct Slot {
    void* block = nullptr;
    double since = 1.0;  // seconds since the last real contact; kHold or more means no hold
    double seen = 0.0;   // last call, in seconds of the performance counter
    uint32_t stamp = 0;
};

Slot g_slots[kSlots];
uint32_t g_stamp = 0;

double qpc_seconds() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

Slot* slot_for(void* block, bool* fresh) {
    Slot* empty = nullptr;
    Slot* oldest = &g_slots[0];
    for (Slot& s : g_slots) {
        if (s.block == block) {
            *fresh = false;
            return &s;
        }
        if (s.block == nullptr && empty == nullptr) {
            empty = &s;
        }
        if (s.stamp < oldest->stamp) {
            oldest = &s;
        }
    }
    Slot* s = empty != nullptr ? empty : oldest;
    s->block = block;
    s->since = kHold;
    s->seen = 0.0;
    *fresh = true;
    return s;
}

// eax is the block, cl is the byte the game is about to store (bit 0 is this frame's contact).
uint8_t __stdcall slide_contact_byte(void* block, uint32_t bits) {
    const uint8_t value = static_cast<uint8_t>(bits);
    const bool contact = (value & 1) != 0;
    bool fresh = false;
    Slot* s = slot_for(block, &fresh);
    const double now = qpc_seconds();
    const double gap = !fresh && s->seen > 0.0 ? now - s->seen : 1.0;
    s->seen = now;
    s->stamp = ++g_stamp;

    double step = fixes_last_dt();
    if (!(step > 0.0 && step <= kGapReset)) {
        step = kHold;
    }
    // A load, or a step that is already one native frame, drops the bit as the game does at 30 FPS.
    if (fresh || gap > kGapReset || !(step * 30.0 < 1.0)) {
        s->since = contact ? 0.0 : kHold;
        return value;
    }
    if (contact) {
        s->since = 0.0;
        return value;
    }
    if (s->since + step < kHold) {
        s->since += step;
        return static_cast<uint8_t>(value | 1u);
    }
    s->since = kHold;
    return value;
}

// xmm0-xmm2 are live across the store, and the compiler will use xmm, so all eight are saved.
__declspec(naked) void slide_stub() {
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
        mov eax, [esp + 0x80 + 0x1C]
        movzx ecx, byte ptr [esp + 0x80 + 0x18]
        push ecx
        push eax
        call slide_contact_byte
        mov ecx, [esp + 0x80 + 0x1C]
        mov byte ptr [ecx + 0x3C4], al
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
        push 0E298C1h
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

bool slidecontact_install() {
    static const uint8_t kMergeBytes[12] = {0x0F, 0xB6, 0x98, 0xC4, 0x03, 0x00, 0x00, 0x80, 0xE3, 0xF8, 0x0A, 0xCB};
    static const uint8_t kStoreBytes[6] = {0x88, 0x88, 0xC4, 0x03, 0x00, 0x00};
    if (std::memcmp(reinterpret_cast<const void*>(kMerge), kMergeBytes, sizeof(kMergeBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kStore), kStoreBytes, sizeof(kStoreBytes)) != 0) {
        LOG_ERROR("Slide contact at %08X does not match this build. Not patching.", kStore);
        return false;
    }
    if (!write_jump(kStore, sizeof(kStoreBytes), &slide_stub)) {
        LOG_ERROR("Could not patch slide contact (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("Slide contact holds for 1/30 s after the last real contact (site %08X)", kStore);
    return true;
}
