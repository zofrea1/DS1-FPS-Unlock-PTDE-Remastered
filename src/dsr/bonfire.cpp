#include "bonfire.h"

#include "log.h"
#include "profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

// Same symptom as Prepare to Die Edition, and the same response: if the character stays in a
// bonfire sit for a second with no menu open, write 0 over the animation id so it stands.
// PTDE's watchdog (src/ptde/ui.cpp) is the reference. The 32-bit addresses do not exist here.
//
// Checked against the 2022 Steam 1.03.1 executable (profile.h):
//   * World character manager RVA 0x1C77E50. The local player is the ChrIns at
//     *(*(image + RVA) + 0x68). It is null while a level is loading. Other characters are
//     not this pointer.
//   * ChrIns::Update (RVA 0x320AE0) reads a uint32 at ChrIns+0xD0 (the read is at RVA 0x320BC6)
//     and compares it with 0x150E. The first softlock test never held that word on a sit id
//     together with every menu dword clear, so the stand-up write never ran. It is still
//     cleared when it does match.
//   * DSR-Gadget reads the current animation at *( *(player + 0x68) + 0x48 ) + 0x80, and the
//     animation object (its speed is at +0xA8) at *( *(player + 0x68) + 0x18 ) + 0x90.
//     Player+0x68 is the move/map object this executable already uses. The same two steps from
//     player+0x48 cover the older map offset. A sit id in any of those words is cleared too.
//     Player+0xFC is cleared when it holds a sit id, because that is PTDE's force-play field.
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

bool is_sit(int anim) {
    return anim == 7701 || anim == 7711 || anim == 7721;
}

// 7698 light, 7699 kindle, 7700/7710/7720 sit down, 7701/7711/7721 seated, 7702/7712/7722 stand.
bool is_bonfire_anim(int anim) {
    return anim >= 7698 && anim <= 7722;
}

// value -1 and address 0 mean the word could not be read. Plain data so the reads stay out of __try.
struct Slot {
    int value = -1;
    uintptr_t address = 0;
};

struct Sample {
    bool player_ok = false;
    bool menu_ok = false;
    Slot d0;
    Slot fc;
    int c8 = -1;
    int cc = -1;
    int d4 = -1;
    int d8 = -1;
    // *( *(player+0x68)+0x48 )+0x80, the current animation DSR-Gadget reads.
    Slot cur;
    // *( *(player+0x68)+0x18 )+0x90, on the animation object whose speed is at +0xA8.
    Slot a90;
    Slot altcur;
    Slot alta90;
    int queue[4] = {-1, -1, -1, -1};
    uint32_t flags[sizeof(kMenuFlags) / sizeof(kMenuFlags[0])] = {};
};

bool read_slot(uintptr_t address, Slot* out) {
    uint32_t value = 0;
    if (!read_u32(address, &value)) {
        return false;
    }
    out->value = static_cast<int>(value);
    out->address = address;
    return true;
}

bool follow(uintptr_t base, uint32_t offset, uintptr_t* out) {
    uintptr_t next = 0;
    if (!read_ptr(base + offset, &next) || next < kMinPointer) {
        return false;
    }
    *out = next;
    return true;
}

int read_int(uintptr_t address) {
    uint32_t value = 0;
    if (!read_u32(address, &value)) {
        return -1;
    }
    return static_cast<int>(value);
}

bool sample_player(uintptr_t image, Sample* out) {
    uintptr_t world = 0;
    if (!read_ptr(image + kWorldChrManRva, &world) || world < kMinPointer) {
        return false;
    }
    uintptr_t player = 0;
    if (!read_ptr(world + kPlayerOffset, &player) || player < kMinPointer) {
        return false;
    }
    out->player_ok = true;
    read_slot(player + kAnimOffset, &out->d0);
    read_slot(player + 0xFC, &out->fc);
    out->c8 = read_int(player + 0xC8);
    out->cc = read_int(player + 0xCC);
    out->d4 = read_int(player + 0xD4);
    out->d8 = read_int(player + 0xD8);

    uintptr_t map = 0;
    uintptr_t anim = 0;
    if (follow(player, 0x68, &map)) {
        if (follow(map, 0x48, &anim)) {
            read_slot(anim + 0x80, &out->cur);
        }
        if (follow(map, 0x18, &anim)) {
            read_slot(anim + 0x90, &out->a90);
            for (int i = 0; i < 4; ++i) {
                out->queue[i] = read_int(anim + 0xCC + static_cast<uint32_t>(i) * 4);
            }
        }
    }
    if (follow(player, 0x48, &map)) {
        if (follow(map, 0x48, &anim)) {
            read_slot(anim + 0x80, &out->altcur);
        }
        if (follow(map, 0x18, &anim)) {
            read_slot(anim + 0x90, &out->alta90);
        }
    }

    uintptr_t menus = 0;
    if (!read_ptr(image + kMenuManRva, &menus) || menus < kMinPointer) {
        return true;
    }
    out->menu_ok = true;
    for (size_t i = 0; i < sizeof(kMenuFlags) / sizeof(kMenuFlags[0]); ++i) {
        read_u32(menus + kMenuFlags[i], &out->flags[i]);
    }
    return true;
}

bool menus_clear(const Sample& sample) {
    if (!sample.menu_ok) {
        return false;
    }
    for (uint32_t flag : sample.flags) {
        if (flag != 0) {
            return false;
        }
    }
    return true;
}

// "none", "unread", or "50=1,2d8=ffffffff".
void format_menus(const Sample& sample, char* buf, size_t cap) {
    if (!sample.menu_ok) {
        snprintf(buf, cap, "unread");
        return;
    }
    size_t used = 0;
    buf[0] = 0;
    for (size_t i = 0; i < sizeof(kMenuFlags) / sizeof(kMenuFlags[0]); ++i) {
        if (sample.flags[i] == 0) {
            continue;
        }
        const int wrote = snprintf(buf + used, cap - used, used ? ",%x=%x" : "%x=%x", kMenuFlags[i], sample.flags[i]);
        if (wrote < 0 || static_cast<size_t>(wrote) >= cap - used) {
            break;
        }
        used += static_cast<size_t>(wrote);
    }
    if (used == 0) {
        snprintf(buf, cap, "none");
    }
}

bool watched_bonfire(const Sample& sample) {
    const int words[] = {
        sample.d0.value, sample.fc.value, sample.c8,     sample.cc,     sample.d4,     sample.d8,
        sample.cur.value, sample.a90.value, sample.altcur.value, sample.alta90.value,
        sample.queue[0], sample.queue[1], sample.queue[2], sample.queue[3],
    };
    for (int word : words) {
        if (is_bonfire_anim(word)) {
            return true;
        }
    }
    return false;
}

bool seated(const Sample& sample) {
    return is_sit(sample.d0.value) || is_sit(sample.fc.value) || is_sit(sample.cur.value) ||
           is_sit(sample.a90.value) || is_sit(sample.altcur.value) || is_sit(sample.alta90.value);
}

// Clear each distinct word that is actually holding a sit. Neighbors are logged only.
void stand_up(const Sample& sample, int* fixed) {
    const Slot* slots[] = {&sample.d0, &sample.fc, &sample.cur, &sample.a90, &sample.altcur, &sample.alta90};
    uintptr_t seen[6] = {};
    int count = 0;
    for (const Slot* slot : slots) {
        if (!is_sit(slot->value) || slot->address == 0) {
            continue;
        }
        bool already = false;
        for (int i = 0; i < count; ++i) {
            if (seen[i] == slot->address) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        seen[count++] = slot->address;
        if (write_u32(slot->address, 0)) {
            ++*fixed;
            LOG_INFO("Bonfire softlock: no menu open for a second while seated; told the character to stand (%d, id %d)",
                     *fixed, slot->value);
        } else {
            LOG_ERROR("Bonfire softlock: could not write the animation id");
        }
    }
}

DWORD WINAPI bonfire_thread(void*) {
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    Sleep(2000);
    ULONGLONG since = 0;
    ULONGLONG last_log = 0;
    ULONGLONG menu_seen = 0;
    int logs = 0;
    int missed_player = 0;
    int fixed = 0;
    while (GetModuleHandleW(nullptr)) {
        Sleep(200);
        Sample sample;
        const bool got = sample_player(image, &sample);
        const ULONGLONG now = GetTickCount64();
        if (!got || !sample.player_ok) {
            since = 0;
            if (missed_player < 8 && now - last_log >= 5000) {
                last_log = now;
                ++missed_player;
                LOG_INFO("Bonfire watch: local player not readable yet");
            }
            continue;
        }

        bool any_menu = false;
        if (sample.menu_ok) {
            for (uint32_t flag : sample.flags) {
                if (flag != 0) {
                    any_menu = true;
                    break;
                }
            }
        }
        if (any_menu) {
            menu_seen = now;
        }
        // A rest at a bonfire, or the few seconds after its menu closes. Combat does not log.
        const bool interesting = watched_bonfire(sample) || any_menu || (menu_seen != 0 && now - menu_seen < 20000);
        if (interesting && logs < 100 && now - last_log >= 1000) {
            last_log = now;
            ++logs;
            char menus[160];
            format_menus(sample, menus, sizeof(menus));
            LOG_INFO(
                "Bonfire watch: d0=%d fc=%d c8=%d cc=%d d4=%d d8=%d cur=%d a90=%d altcur=%d alta90=%d "
                "q=%d,%d,%d,%d menus=%s",
                sample.d0.value, sample.fc.value, sample.c8, sample.cc, sample.d4, sample.d8, sample.cur.value,
                sample.a90.value, sample.altcur.value, sample.alta90.value, sample.queue[0], sample.queue[1],
                sample.queue[2], sample.queue[3], menus);
        }

        if (!seated(sample) || !menus_clear(sample)) {
            since = 0;
            continue;
        }
        if (since == 0) {
            since = now;
            continue;
        }
        if (now - since < 1000) {
            continue;
        }
        stand_up(sample, &fixed);
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
