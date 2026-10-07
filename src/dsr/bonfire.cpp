#include "bonfire.h"

#include "log.h"
#include "profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

// Same rule as Prepare to Die Edition (src/ptde/ui.cpp): if an idle bonfire sit is still set
// and no menu has been open for a second, write 0 over that animation id so the character stands.
// The 32-bit addresses do not exist here. There is no separate check for Reverse Hollowing.
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
//     sit id is absent for that stretch. The watch log lists every nonzero id from 0 to 80
//     while a sit is recorded.
//   * The idle sit is not one known field on this exe. While a menu is open, or for a short
//     while after, the player, the current-animation object DSR-Gadget reads
//     (*( *(player + 0x68) + 0x48 ) + 0x80) and the stay-animation block are scanned for
//     7701, 7711 and 7721. Any such word is cleared once the menus have been clear for a second.
//
// PTDE also requires character status 0 (human) or 8 (hollow). That field was not found on
// this executable, so it is not checked. A wrong pointer is worse than a missed kick, so
// every read and the write are guarded, and a pointer below 16 MB is treated as not ready.

namespace {

constexpr uint32_t kWorldChrManRva = 0x1C77E50;
constexpr uint32_t kMenuManRva = 0x1C88D98;
constexpr uint32_t kPlayerOffset = 0x68;
// Heap objects in this process sit well above 16 MB. Smaller values are counts and flags.
constexpr uintptr_t kMinPointer = 0x1000000;
constexpr uintptr_t kMaxPointer = 0x00007FFFFFFFFFFFull;

// Dwords that mean a menu is open when nonzero. The first fourteen are the mask's own
// compares. The rest are menu ids that same mask, and the any-menu check next to it,
// read through the id array and that the fourteen do not cover.
constexpr uint32_t kMenuFlags[] = {
    0x50, 0x5C, 0x60, 0x64, 0x78, 0x8C, 0x94, 0x9C, 0xB0, 0xB4, 0xC0, 0x134, 0x2D8, 0x300,
    0x38, 0x3C, 0x54, 0x84, 0x98, 0x11C, 0x138,
};
// Ids 0..80. The rest list was not among the fourteen, so a sit logs whichever of these is set.
constexpr int kMenuIdScan = 81;

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

// Bytes of address that are committed and readable, capped at size. Never raises.
size_t readable_bytes(uintptr_t address, size_t size) {
    if (address < kMinPointer || address > kMaxPointer || size < 4) {
        return 0;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) {
        return 0;
    }
    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD)) {
        return 0;
    }
    const DWORD protect = info.Protect & 0xFF;
    const bool readable = protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
                          protect == PAGE_EXECUTE_WRITECOPY;
    if (!readable) {
        return 0;
    }
    const uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
    const uintptr_t end = region + info.RegionSize;
    if (address < region || address >= end) {
        return 0;
    }
    const size_t room = static_cast<size_t>(end - address);
    return room < size ? room : size;
}

bool follow(uintptr_t base, uint32_t offset, uintptr_t* out) {
    if (readable_bytes(base + offset, sizeof(uintptr_t)) < sizeof(uintptr_t)) {
        return false;
    }
    uintptr_t next = 0;
    if (!read_ptr(base + offset, &next) || next < kMinPointer || next > kMaxPointer) {
        return false;
    }
    if (readable_bytes(next, 0x10) < 0x10) {
        return false;
    }
    *out = next;
    return true;
}

int read_int(uintptr_t address) {
    uint32_t value = 0;
    if (readable_bytes(address, sizeof(value)) < sizeof(value) || !read_u32(address, &value)) {
        return -1;
    }
    return static_cast<int>(value);
}

// One seated-loop word. where is a literal ("player", "stay").
struct Hit {
    const char* where = "";
    uint32_t offset = 0;
    int value = 0;
    uintptr_t address = 0;
};

struct Sample {
    bool player_ok = false;
    bool menu_ok = false;
    bool cur_ok = false;
    int cur = -1;
    bool stay_ok = false;
    int stay_upper = -1;
    int stay_lower = -1;
    Hit hits[8] = {};
    int hit_count = 0;
    uintptr_t menus = 0;
    uint32_t flags[sizeof(kMenuFlags) / sizeof(kMenuFlags[0])] = {};
};

void note_sit(Sample* sample, const char* where, uintptr_t base, uint32_t offset, int value) {
    if (!is_sit(value) || sample->hit_count >= 8) {
        return;
    }
    const uintptr_t address = base + offset;
    for (int i = 0; i < sample->hit_count; ++i) {
        if (sample->hits[i].address == address) {
            return;
        }
    }
    Hit& hit = sample->hits[sample->hit_count++];
    hit.where = where;
    hit.offset = offset;
    hit.value = value;
    hit.address = address;
}

void scan(Sample* sample, const char* where, uintptr_t base, uint32_t size) {
    const size_t bytes = readable_bytes(base, size);
    for (uint32_t offset = 0; offset + sizeof(uint32_t) <= bytes; offset += sizeof(uint32_t)) {
        const int value = read_int(base + offset);
        note_sit(sample, where, base, offset, value);
    }
}

void read_current(uintptr_t player, Sample* out) {
    uintptr_t map = 0;
    uintptr_t block = 0;
    if (!follow(player, 0x68, &map) || !follow(map, 0x48, &block)) {
        return;
    }
    if (readable_bytes(block + 0x80, sizeof(uint32_t)) < sizeof(uint32_t)) {
        return;
    }
    out->cur_ok = true;
    out->cur = read_int(block + 0x80);
}

void read_stay(uintptr_t player, Sample* out) {
    uintptr_t body = 0;
    uintptr_t stay = 0;
    if (!follow(player, 0x30, &body) || !follow(body, 0x5D0, &stay)) {
        return;
    }
    out->stay_ok = true;
    out->stay_upper = read_int(stay + 0x690);
    out->stay_lower = read_int(stay + 0x13B0);
    scan(out, "stay", stay, 0x1400);
}

bool sample_player(uintptr_t image, Sample* out) {
    uintptr_t world = 0;
    if (!read_ptr(image + kWorldChrManRva, &world) || world < kMinPointer || world > kMaxPointer) {
        return false;
    }
    uintptr_t player = 0;
    if (!follow(world, kPlayerOffset, &player)) {
        return false;
    }
    out->player_ok = true;
    read_current(player, out);

    uintptr_t menus = 0;
    if (follow(image, kMenuManRva, &menus)) {
        out->menu_ok = true;
        out->menus = menus;
        for (size_t i = 0; i < sizeof(kMenuFlags) / sizeof(kMenuFlags[0]); ++i) {
            uint32_t flag = 0;
            if (read_u32(menus + kMenuFlags[i], &flag)) {
                out->flags[i] = flag;
            }
        }
    }

    scan(out, "player", player, 0x700);
    uintptr_t map = 0;
    if (follow(player, 0x68, &map)) {
        scan(out, "map", map, 0x100);
        uintptr_t child = 0;
        if (follow(map, 0x18, &child)) {
            scan(out, "anim", child, 0x100);
        }
        if (follow(map, 0x48, &child)) {
            scan(out, "cur", child, 0x100);
        }
    }
    uintptr_t alt = 0;
    if (follow(player, 0x48, &alt)) {
        scan(out, "map48", alt, 0x100);
    }
    read_stay(player, out);
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

// Nonzero menu ids 0..80, at most twelve. "none" or "unread".
void format_ids(const Sample& sample, char* buf, size_t cap) {
    if (!sample.menu_ok || sample.menus == 0) {
        snprintf(buf, cap, "unread");
        return;
    }
    size_t used = 0;
    buf[0] = 0;
    int shown = 0;
    for (int id = 0; id < kMenuIdScan && shown < 12; ++id) {
        uint32_t value = 0;
        const uintptr_t address = sample.menus + 0x30u + static_cast<uint32_t>(id) * 4u;
        if (!read_u32(address, &value) || value == 0) {
            continue;
        }
        const int wrote = snprintf(buf + used, cap - used, used ? ",%d:%x" : "%d:%x", id, value);
        if (wrote < 0 || static_cast<size_t>(wrote) >= cap - used) {
            break;
        }
        used += static_cast<size_t>(wrote);
        ++shown;
    }
    if (used == 0) {
        snprintf(buf, cap, "none");
    }
}

void format_hits(const Sample& sample, char* buf, size_t cap) {
    if (sample.hit_count == 0) {
        snprintf(buf, cap, "none");
        return;
    }
    size_t used = 0;
    buf[0] = 0;
    for (int i = 0; i < sample.hit_count; ++i) {
        const Hit& hit = sample.hits[i];
        const int wrote = snprintf(buf + used, cap - used, used ? ",%s+%x=%d" : "%s+%x=%d", hit.where, hit.offset,
                                   hit.value);
        if (wrote < 0 || static_cast<size_t>(wrote) >= cap - used) {
            break;
        }
        used += static_cast<size_t>(wrote);
    }
}

void format_word(bool ok, int value, char* buf, size_t cap) {
    if (!ok) {
        snprintf(buf, cap, "na");
        return;
    }
    snprintf(buf, cap, "%d", value);
}

void clear_sits(const Sample& sample, int* fixed) {
    for (int i = 0; i < sample.hit_count; ++i) {
        const Hit& hit = sample.hits[i];
        // The word can change during the one-second wait. Only clear it if it is still a sit.
        if (!is_sit(read_int(hit.address))) {
            continue;
        }
        if (write_u32(hit.address, 0)) {
            ++*fixed;
            LOG_INFO("Bonfire softlock: no menu open for a second while seated; told the character to stand (%d, %s+%x was %d)",
                     *fixed, hit.where, hit.offset, hit.value);
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
        const ULONGLONG now = GetTickCount64();
        Sample sample;
        const bool got = sample_player(image, &sample);
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

        const bool interesting = any_menu || sample.hit_count > 0 || (menu_seen != 0 && now - menu_seen < 30000);
        if (interesting && logs < 80 && now - last_log >= 1000) {
            last_log = now;
            ++logs;
            char menus[192];
            char ids[160];
            char hits[192];
            char cur[16];
            char stay_upper[16];
            char stay_lower[16];
            format_menus(sample, menus, sizeof(menus));
            format_hits(sample, hits, sizeof(hits));
            format_word(sample.cur_ok, sample.cur, cur, sizeof(cur));
            format_word(sample.stay_ok, sample.stay_upper, stay_upper, sizeof(stay_upper));
            format_word(sample.stay_ok, sample.stay_lower, stay_lower, sizeof(stay_lower));
            if (sample.hit_count > 0) {
                format_ids(sample, ids, sizeof(ids));
            } else {
                snprintf(ids, sizeof(ids), "-");
            }
            LOG_INFO("Bonfire watch: cur=%s stayU=%s stayL=%s hits=%s menus=%s ids=%s", cur, stay_upper, stay_lower,
                     hits, menus, ids);
        }

        if (!sample.hit_count || !menus_clear(sample)) {
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
        clear_sits(sample, &fixed);
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
