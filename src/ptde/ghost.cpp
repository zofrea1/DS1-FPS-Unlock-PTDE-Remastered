#include "ghost.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

constexpr int32_t kUnit = 65536;  // counter units per 30 FPS frame

// Playback (fn 0xE16500).
constexpr uint32_t kPlayDec = 0x00E16533;     // dec dword ptr [esi+274h]          (6 bytes)
constexpr uint32_t kPlayReload = 0x00E16548;  // mov dword ptr [esi+274h], 0Ah     (10 bytes)
// Recording (fn 0xE1DC60).
constexpr uint32_t kRecDec = 0x00E1DC71;      // or edi,-1 ; add [ebx+234h],edi     (9 bytes)
constexpr uint32_t kRecReload = 0x00E1DDDA;   // mov dword ptr [ebx+334h], 0Ah     (10 bytes)
constexpr uint32_t kNetReload = 0x00E1DF7F;   // mov dword ptr [ebx+234h], 5       (10 bytes)

// Read by the stubs: counter units per second (playback multiplies its own dt by it), and minus
// this frame's units for the recorder (it adds edi to both of its counters).
alignas(4) float g_units_per_second = 30.0f * kUnit;
alignas(4) volatile int32_t g_rec_step = -kUnit;

bool write_code(uint32_t address, const uint8_t* bytes, size_t size) {
    auto* p = reinterpret_cast<uint8_t*>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

bool write_call(uint32_t site, size_t size, const void* target) {
    uint8_t bytes[16];
    bytes[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(bytes + 1, &rel, 4);
    for (size_t i = 5; i < size; ++i) bytes[i] = 0x90;
    return write_code(site, bytes, size);
}

// `mov dword ptr [reg+disp32], imm32` (C7 /0) becomes `add dword ptr [reg+disp32], imm32` (81 /0):
// same length, same operands.
bool reload_to_add(uint32_t site, const uint8_t* expect, int32_t frames) {
    uint8_t bytes[10];
    std::memcpy(bytes, expect, 10);
    bytes[0] = 0x81;
    const int32_t value = frames * kUnit;
    std::memcpy(bytes + 6, &value, 4);
    return write_code(site, bytes, 10);
}

}  // namespace

bool ghost_install() {
    static const uint8_t kPlayDecExpect[6] = {0xFF, 0x8E, 0x74, 0x02, 0x00, 0x00};
    static const uint8_t kPlayReloadExpect[10] = {0xC7, 0x86, 0x74, 0x02, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00};
    static const uint8_t kRecDecExpect[9] = {0x83, 0xCF, 0xFF, 0x01, 0xBB, 0x34, 0x02, 0x00, 0x00};
    static const uint8_t kRecReloadExpect[10] = {0xC7, 0x83, 0x34, 0x03, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00};
    static const uint8_t kNetReloadExpect[10] = {0xC7, 0x83, 0x34, 0x02, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00};
    struct Check {
        uint32_t va;
        const uint8_t* bytes;
        size_t size;
    };
    const Check checks[] = {{kPlayDec, kPlayDecExpect, sizeof(kPlayDecExpect)},
                            {kPlayReload, kPlayReloadExpect, sizeof(kPlayReloadExpect)},
                            {kRecDec, kRecDecExpect, sizeof(kRecDecExpect)},
                            {kRecReload, kRecReloadExpect, sizeof(kRecReloadExpect)},
                            {kNetReload, kNetReloadExpect, sizeof(kNetReloadExpect)}};
    for (const Check& c : checks) {
        if (std::memcmp(reinterpret_cast<const void*>(c.va), c.bytes, c.size) != 0) {
            LOG_ERROR("Ghost replay site %08X does not match this build. Not patching.", c.va);
            return false;
        }
    }
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) return false;
    uint8_t* p = cave;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    };
    auto emit32 = [&](uint32_t v) {
        std::memcpy(p, &v, 4);
        p += 4;
    };
    // Playback. xmm0 holds this update's dt (loaded for the dt > 0 test just before); eax and xmm0
    // are overwritten by the code that follows, and the cmp after the call sets its own flags.
    uint8_t* play = p;
    emit({0xF3, 0x0F, 0x59, 0x05});                    // mulss xmm0, [g_units_per_second]  (dt * 30 * unit)
    emit32(reinterpret_cast<uint32_t>(&g_units_per_second));
    emit({0xF3, 0x0F, 0x2D, 0xC0});                    // cvtss2si eax, xmm0
    emit({0x29, 0x86, 0x74, 0x02, 0x00, 0x00});        // sub [esi+274h], eax
    emit({0xC3});                                      // ret
    // Recording: edi = -(this frame's units), added to the network counter here and to the replay
    // counter by the game's own `add [ebx+334h], edi` a few instructions later.
    uint8_t* rec = p;
    emit({0x8B, 0x3D});                                // mov edi, [g_rec_step]
    emit32(reinterpret_cast<uint32_t>(&g_rec_step));
    emit({0x01, 0xBB, 0x34, 0x02, 0x00, 0x00});        // add [ebx+234h], edi
    emit({0xC3});                                      // ret
    FlushInstructionCache(GetCurrentProcess(), cave, 64);

    // Counters already running hold whole frames; they cross zero on the next frame and every
    // reload after that is in the new units.
    bool ok = write_call(kPlayDec, 6, play) && reload_to_add(kPlayReload, kPlayReloadExpect, 10) &&
              write_call(kRecDec, 9, rec) && reload_to_add(kRecReload, kRecReloadExpect, 10) &&
              reload_to_add(kNetReload, kNetReloadExpect, 5);
    if (!ok) {
        LOG_ERROR("Could not patch the ghost replay counters");
        return false;
    }
    LOG_INFO("Ghost replays and the replay recorder follow real time (sample every 1/3 s, network sample every 1/6 s)");
    return true;
}

void ghost_frame(double dt) {
    const double units = dt * 30.0 * kUnit;
    g_rec_step = -static_cast<int32_t>(std::lround(units));
}
