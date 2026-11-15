#include "eventwaits.h"

#include "eventwait.h"
#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kVtable = 0x011DE3FC;  // NS_FRPG::EcTestPassTime
constexpr uint32_t kTestSlot = kVtable + 4;
constexpr uint32_t kGameTest = 0x00BB22D0;
constexpr double kRate = 30.0;

using TestFn = void(__fastcall*)(uint8_t* self, void* edx, float dt, int unused);
volatile bool g_on = true;

// The game's test: remaining -= dt (in double, stored as float), bit 0 of +8 = remaining <= 0.
void __fastcall hk_test(uint8_t* self, void* edx, float dt, int unused) {
    if (!g_on) {
        reinterpret_cast<TestFn>(kGameTest)(self, edx, dt, unused);
        return;
    }
    auto* remaining = reinterpret_cast<float*>(self + 0x18);
#if FPSU_DIAGNOSTICS
    const float before = *remaining;
#endif
    const bool done = eventwait::step(self, remaining, dt, kRate, GetTickCount64());
    self[8] = static_cast<uint8_t>(done ? (self[8] | 1) : (self[8] & 0xFE));
#if FPSU_DIAGNOSTICS
    // The first few waits that end, for the log: when they began (their length) and when they ended.
    static int logged = 0;
    if (done && logged < 8) {
        ++logged;
        LOG_INFO("FixEventWaits: a wait ended (%.4f s left before this frame of %.4f s)", before, dt);
    }
#endif
}

}  // namespace

bool eventwaits_install() {
    auto* slot = reinterpret_cast<uint32_t*>(kTestSlot);
    const uint8_t kExpect[6] = {0xF3, 0x0F, 0x10, 0x41, 0x18, 0xF3};  // movss xmm0, [ecx+18h] ; movss ...
    if (*slot != kGameTest || std::memcmp(reinterpret_cast<const void*>(kGameTest), kExpect, sizeof(kExpect)) != 0) {
        LOG_ERROR("FixEventWaits: the event wait test (vtable %08X) does not match this build. Not patching.", kVtable);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
        LOG_ERROR("FixEventWaits: could not patch the event wait test (Win32=%lu)", GetLastError());
        return false;
    }
    *slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&hk_test));
    DWORD ignored = 0;
    VirtualProtect(slot, 4, old, &ignored);
    LOG_INFO("FixEventWaits: event script waits last as in the original game at 30 FPS (the wait test, vtable %08X)",
             kVtable);
    return true;
}

void eventwaits_set(bool on) { g_on = on; }
