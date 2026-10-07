#include "bonfire.h"

#include "log.h"
#include "profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Same symptom as Prepare to Die Edition, and the same response: if the character stays in a
// bonfire sit for a second with no menu open, write 0 over the animation id so it stands.
// PTDE's watchdog (src/ptde/ui.cpp) is the reference. The 32-bit addresses do not exist here.
//
// Checked against the 2022 Steam 1.03.1 executable (profile.h):
//   * World character manager RVA 0x1C77E50. The local player is the ChrIns at
//     *(*(image + RVA) + 0x68). It is null while a level is loading. Other characters are
//     not this pointer.
//   * ChrIns::Update (RVA 0x320AE0) reads the live animation id as a uint32 at ChrIns+0xD0
//     (the read is at RVA 0x320BC6). That is the word this writes. PTDE's animation id lives
//     behind a different chain, at *(*animPtr)+0xFC, which this executable does not use.
//   * Menu manager RVA 0x1C88D98. The game's own open-menu mask (RVA 0x71AC10) sets a bit
//     when one of the dwords below is nonzero, so "no menu" means every one of them is 0.
//     Three offsets match PTDE (0x50 reinforce, 0x60 dialog, 0x78 level-up). The others
//     moved. PTDE's byte flags at 0x40, 0x4C, 0x80, 0x84 and 0xAC are not fields of this
//     menu manager, and a byte read would miss a dword whose low byte is 0.
//   * Sit ids 7701, 7711 and 7721 are the ones the PTDE watchdog stands up from (rest writes
//     7710, and the seated menu pose is 7711). They are not immediates in this executable;
//     they are the shared bonfire sits, and a test has to confirm them.
//
// PTDE also requires character status 0 (human) or 8 (hollow) at +0xA28 of its status block.
// That field was not found on this executable, so it is not checked. A wrong pointer is worse
// than a missed kick, so every read and the write are guarded, and a pointer below 0x10000
// is treated as not ready.

namespace {

constexpr uint32_t kWorldChrManRva = 0x1C77E50;
constexpr uint32_t kMenuManRva = 0x1C88D98;
constexpr uint32_t kPlayerOffset = 0x68;
constexpr uint32_t kAnimOffset = 0xD0;
constexpr uintptr_t kMinPointer = 0x10000;

// Dwords the mask function compares with zero. Order follows that function.
constexpr uint32_t kMenuFlags[] = {
    0x50, 0x5C, 0x60, 0x64, 0x78, 0x8C, 0x94, 0x9C, 0xB0, 0xB4, 0xC0, 0x134, 0x2D8, 0x300,
};

bool exe_supported() {
    __try {
        auto* image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        if (!image) {
            return false;
        }
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
            return false;
        }
        return nt->FileHeader.TimeDateStamp == kBuild.timestamp &&
               nt->OptionalHeader.SizeOfImage == kBuild.image_size &&
               nt->OptionalHeader.CheckSum == kBuild.checksum;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_u32(uintptr_t address, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(uintptr_t address, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_u32(uintptr_t address, uint32_t value) {
    __try {
        *reinterpret_cast<uint32_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool is_sit(uint32_t anim) {
    return anim == 7701 || anim == 7711 || anim == 7721;
}

// True when the local player is seated and every open-menu dword is zero.
// On success, *anim_address is the uint32 to write.
bool stuck_at_bonfire(uintptr_t image, uintptr_t* anim_address) {
    uintptr_t world = 0;
    if (!read_ptr(image + kWorldChrManRva, &world) || world < kMinPointer) {
        return false;
    }
    uintptr_t player = 0;
    if (!read_ptr(world + kPlayerOffset, &player) || player < kMinPointer) {
        return false;
    }
    uint32_t anim = 0;
    if (!read_u32(player + kAnimOffset, &anim) || !is_sit(anim)) {
        return false;
    }

    uintptr_t menus = 0;
    if (!read_ptr(image + kMenuManRva, &menus) || menus < kMinPointer) {
        return false;
    }
    for (uint32_t offset : kMenuFlags) {
        uint32_t open = 0;
        if (!read_u32(menus + offset, &open)) {
            return false;
        }
        if (open != 0) {
            return false;
        }
    }
    *anim_address = player + kAnimOffset;
    return true;
}

DWORD WINAPI bonfire_thread(void*) {
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    Sleep(2000);
    ULONGLONG since = 0;
    int fixed = 0;
    while (GetModuleHandleW(nullptr)) {
        Sleep(200);
        uintptr_t address = 0;
        if (!stuck_at_bonfire(image, &address)) {
            since = 0;
            continue;
        }
        const ULONGLONG now = GetTickCount64();
        if (since == 0) {
            since = now;
            continue;
        }
        if (now - since < 1000) {
            continue;
        }
        if (write_u32(address, 0)) {
            ++fixed;
            LOG_INFO("Bonfire softlock: no menu open for a second while seated; told the character to stand (%d)",
                     fixed);
        } else {
            LOG_ERROR("Bonfire softlock: could not write the animation id");
        }
        since = 0;
    }
    return 0;
}

}  // namespace

bool bonfire_start() {
    if (!exe_supported()) {
        LOG_ERROR("Bonfire softlock watchdog left off: exe is not the supported 2022 Steam build");
        return false;
    }
    HANDLE thread = CreateThread(nullptr, 0, bonfire_thread, nullptr, 0, nullptr);
    if (!thread) {
        LOG_ERROR("Bonfire softlock watchdog thread was not created");
        return false;
    }
    CloseHandle(thread);
    LOG_INFO("Bonfire softlock watchdog running");
    return true;
}
