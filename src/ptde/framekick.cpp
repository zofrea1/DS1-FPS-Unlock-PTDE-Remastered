#include "framekick.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

// mov byte ptr [edi+0Ch], 1 (edi = the frame rate meter at MoveMapStep+0xF0): "the last 30 s averaged 15 FPS or
// less" (see framekick.h).
constexpr uint32_t kFlagSite = 0x0100E083;
constexpr uint8_t kFlagWrite[4] = {0xC6, 0x47, 0x0C, 0x01};
constexpr uint8_t kNop4[4] = {0x0F, 0x1F, 0x40, 0x00};  // nop dword ptr [eax+0]

bool patch(uint32_t va, const uint8_t* expect, const uint8_t* repl, size_t n) {
    auto* p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(va));
    __try {
        if (std::memcmp(p, expect, n) != 0) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, repl, n);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    DWORD ignored = 0;
    VirtualProtect(p, n, old, &ignored);
    return true;
}

}  // namespace

bool framekick_install(bool fix) {
    if (!fix) {
        LOG_INFO("FixFramerateKick: false (30 seconds averaging 15 FPS or less, a long pause too, end online play "
                 "and return to the title)");
        return true;
    }
    if (!patch(kFlagSite, kFlagWrite, kNop4, sizeof(kFlagWrite))) {
        LOG_ERROR("FixFramerateKick: the frame rate check (0x%08X) did not match this build; it stays on", kFlagSite);
        return false;
    }
    LOG_INFO("FixFramerateKick: a pause or a slow stretch no longer ends online play (\"Framerate is insufficient "
             "for Online play\" and the title screen); the game's 30-second frame rate check is off (0x%08X)",
             kFlagSite);
    return true;
}
