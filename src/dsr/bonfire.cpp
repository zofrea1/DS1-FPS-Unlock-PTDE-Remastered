#include "bonfire.h"

#include "log.h"
#include "profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

// Same rule as Prepare to Die Edition (src/ptde/ui.cpp): if an idle bonfire sit is still set, no menu is open for a
// second, and no menu or prompt has been open for 10 seconds, write 0 over that animation id so the character stands.
// The 10 seconds outlast Reverse Hollowing and Kindle (about 6.3 s each with the menu closed): with one second alone,
// Prepare to Die Edition once stood a character up in the middle of Reverse Hollowing (see src/ptde/ui.cpp). The
// 32-bit addresses do not exist here. There is no separate check for Reverse Hollowing.
//
// The sits were read from this install's own c0000.anibnd (a00.tae), not from a PTDE list.
// The three idle loops are a00_7701, a00_7711 and a00_7721, each 2.667 seconds, and each has a
// 5.2 second sit-down (7700, 7710, 7720) and stand (7702, 7712, 7722). 7501 is a different
// clip, a00_7501, 4.13 seconds. It is not one of those loops, so it is not a reason to stand.
//
// Checked against the 2022 Steam 1.03.1 executable (profile.h):
//   * World character manager RVA 0x1C77E50. The local player is the ChrIns at
//     *(*(image + RVA) + 0x68). It is null while a level is loading.
//   * Menu manager RVA 0x1C88D98. Open state is a dword array at +0x30, one dword per menu id.
//     The mask at RVA 0x71AC10 sets a bit when one of the first fourteen offsets below is
//     nonzero. It also sets a bit when menu id 9 (+0x54) is 1, 2, 3 or 4, and the any-menu
//     check beside it reads ids 2, 3, 21, 26, 59 and 66 the same way (nonzero means open).
//     A normal rest leaves the first fourteen at 0 while the rest list is on screen, so those
//     extra ids have to be clear too. A deeper menu, such as level up, sets +0x50 and the
//     sit id is absent for that stretch.
//   * The idle sit is the local player's animation id at +0x16C (the pose follows it). It was found by scanning
//     the player, the current-animation object DSR-Gadget reads (*( *(player + 0x68) + 0x48 ) + 0x80) and the
//     stay-animation block for 7701, 7711 and 7721; writing 0 at +0x16C stands the character up, and the other
//     copies of that id clear on their own, so only that word is read and written.
//
// PTDE also requires character status 0 (human) or 8 (hollow). That field was not found on
// this executable, so it is not checked. A wrong pointer is worse than a missed kick, so
// every read and the write are guarded, and a pointer below 16 MB is treated as not ready.

namespace {

constexpr uint32_t kWorldChrManRva = 0x1C77E50;
constexpr uint32_t kMenuManRva = 0x1C88D98;
constexpr uint32_t kPlayerOffset = 0x68;
// The local player's animation id: the pose follows it, and writing 0 stands the character up.
constexpr uint32_t kAnimOffset = 0x16C;
// Heap objects in this process sit well above 16 MB. Smaller values are counts and flags.
constexpr uintptr_t kMinPointer = 0x1000000;
constexpr uintptr_t kMaxPointer = 0x00007FFFFFFFFFFFull;
// Seated with no menu this long, and no menu or prompt open for this long, before stepping in.
constexpr ULONGLONG kSeatedMs = 1000;
constexpr ULONGLONG kNoMenuMs = 10000;

// Dwords that mean a menu is open when nonzero. The first fourteen are the mask's own
// compares. The rest are menu ids that same mask, and the any-menu check next to it,
// read through the id array and that the fourteen do not cover.
constexpr uint32_t kMenuFlags[] = {
    0x50, 0x5C, 0x60, 0x64, 0x78, 0x8C, 0x94, 0x9C, 0xB0, 0xB4, 0xC0, 0x134, 0x2D8, 0x300,
    0x38, 0x3C, 0x54, 0x84, 0x98, 0x11C, 0x138,
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

bool is_sit(int anim) {
    return anim == 7701 || anim == 7711 || anim == 7721;
}

// Every read is an exception-guarded load (no VirtualQuery: in this process it walks the page tables of the large
// heap region the player lives in to report its size, about 84 us a call, measured 2026-11-19), and a pointer outside
// [16 MB, user space] is not followed.
bool follow(uintptr_t base, uint32_t offset, uintptr_t* out) {
    uintptr_t next = 0;
    if (!read_ptr(base + offset, &next) || next < kMinPointer || next > kMaxPointer) {
        return false;
    }
    *out = next;
    return true;
}

int read_int(uintptr_t address) {
    uint32_t value = 0;
    if (address < kMinPointer || address > kMaxPointer || !read_u32(address, &value)) {
        return -1;
    }
    return static_cast<int>(value);
}

// The menu manager's open flags (see the top): 1 when a menu or prompt is on screen, 0 when none is, -1 when they
// cannot be read.
int menu_open(uintptr_t image) {
    uintptr_t menus = 0;
    if (!follow(image, kMenuManRva, &menus)) {
        return -1;
    }
    for (uint32_t offset : kMenuFlags) {
        uint32_t flag = 0;
        if (!read_u32(menus + offset, &flag)) {
            return -1;
        }
        if (flag != 0) {
            return 1;
        }
    }
    return 0;
}

// The local player's ChrIns, or 0 while a level loads.
uintptr_t local_player(uintptr_t image) {
    uintptr_t world = 0;
    if (!read_ptr(image + kWorldChrManRva, &world) || world < kMinPointer || world > kMaxPointer) {
        return 0;
    }
    uintptr_t player = 0;
    return follow(world, kPlayerOffset, &player) ? player : 0;
}

// Five times a second: the player's animation id (+0x16C) and the menus' open flags, a couple of dozen guarded reads.
// Up to 2.5.0 each pass also scanned six blocks of the player (about 2,100 words, a VirtualQuery before every read) to
// log where else the sit id showed up, which was only needed to find this word: in the game that cost about 176 ms of
// CPU a pass, 88% of a core (measured with v2.4.2 at 60 FPS, 2026-11-19; a user saw +10% CPU and +25 C on a 7600X).
DWORD WINAPI bonfire_thread(void*) {
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    Sleep(2000);
    ULONGLONG since = 0;
    ULONGLONG menu_seen = 0;  // when a menu or prompt was last open
    int fixed = 0;
    while (GetModuleHandleW(nullptr)) {
        Sleep(200);
        const ULONGLONG now = GetTickCount64();
        const int menu = menu_open(image);
        if (menu == 1) {
            menu_seen = now;
        }
        const uintptr_t player = local_player(image);
        if (!player || !is_sit(read_int(player + kAnimOffset)) || menu != 0) {
            since = 0;
            continue;
        }
        if (since == 0) {
            since = now;
            continue;
        }
        if (now - since < kSeatedMs || now - menu_seen < kNoMenuMs) {
            continue;
        }
        const double seated_s = (now - since) / 1000.0, no_menu_s = (now - menu_seen) / 1000.0;
        since = 0;
        // The word can change during the one-second wait. Only clear it if it is still a sit.
        const int anim = read_int(player + kAnimOffset);
        if (!is_sit(anim)) {
            continue;
        }
        if (write_u32(player + kAnimOffset, 0)) {
            ++fixed;
            LOG_INFO("Bonfire softlock: seated for %.1f s with no menu open for %.1f s; told the character to stand "
                     "(%d, animation %d)", seated_s, no_menu_s, fixed, anim);
        } else {
            LOG_ERROR("Bonfire softlock: could not write the animation id");
        }
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
