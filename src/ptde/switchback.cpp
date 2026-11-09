#include "switchback.h"

#include "fixes.h"
#include "log.h"
#include "turnhistory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kWriterSlot = 0x0118E81C;  // TurnAnim vtable 0x118E810, slot 3
constexpr uint32_t kWriterFn = 0x00E13160;
constexpr uint32_t kCheckFn = 0x00E10DB0;
constexpr uint32_t kCheckSprint = 0x00FE13A4;  // call 0E10DB0h
constexpr uint32_t kCheckRun = 0x00FE1499;     // call 0E10DB0h

using WriterFn = void*(__thiscall*)(void* self, void* out, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6,
                                    uint32_t a7);
using CheckFn = bool(__thiscall*)(void* self, float threshold, uint32_t* out);

void* __fastcall hk_writer(void* self, void* /*edx*/, void* out, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6,
                           uint32_t a7) {
    void* r = reinterpret_cast<WriterFn>(kWriterFn)(self, out, a3, a4, a5, a6, a7);
    turnhistory_record(static_cast<const uint8_t*>(self), static_cast<float>(fixes_last_dt()));
    return r;
}

bool __fastcall hk_check(void* self, void* /*edx*/, float threshold, uint32_t* out) {
    const int r = turnhistory_check(static_cast<const uint8_t*>(self), threshold, out);
    if (r < 0) return reinterpret_cast<CheckFn>(kCheckFn)(self, threshold, out);
    return r != 0;
}

bool write_code(uint32_t address, const void* bytes, size_t size) {
    auto* p = reinterpret_cast<uint8_t*>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

bool retarget_call(uint32_t site, const void* target) {
    uint8_t call[5];
    call[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(target) - (site + 5));
    std::memcpy(call + 1, &rel, 4);
    return write_code(site, call, sizeof(call));
}

}  // namespace

bool switchback_install() {
    static const uint8_t kSprintExpect[5] = {0xE8, 0x07, 0xFA, 0xE2, 0xFF};
    static const uint8_t kRunExpect[5] = {0xE8, 0x12, 0xF9, 0xE2, 0xFF};
    static const uint8_t kCheckExpect[6] = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56};  // sub esp,14h ; push ebx/ebp/esi
    if (*reinterpret_cast<const uint32_t*>(kWriterSlot) != kWriterFn ||
        std::memcmp(reinterpret_cast<const void*>(kCheckSprint), kSprintExpect, sizeof(kSprintExpect)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kCheckRun), kRunExpect, sizeof(kRunExpect)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kCheckFn), kCheckExpect, sizeof(kCheckExpect)) != 0) {
        LOG_ERROR("Switchback check at %08X does not match this build. Not patching.", kCheckSprint);
        return false;
    }
    TurnHistoryLayout layout;
    layout.angle = 0x80;
    layout.dir = 0xF8;
    layout.index = 0x170;
    layout.size = 0x174;
    turnhistory_configure(layout, 30.0);
    const uint32_t hook = reinterpret_cast<uint32_t>(&hk_writer);
    if (!write_code(kWriterSlot, &hook, sizeof(hook))) {
        LOG_ERROR("Could not hook the turn ring writer (Win32=%lu)", GetLastError());
        return false;
    }
    if (!retarget_call(kCheckSprint, &hk_check) || !retarget_call(kCheckRun, &hk_check)) {
        LOG_ERROR("Could not patch the switchback check (Win32=%lu)", GetLastError());
        return false;
    }
    LOG_INFO("Switchback animation checks the stick over the original game's 30 FPS frames (sites %08X, %08X)",
             kCheckSprint, kCheckRun);
    return true;
}
