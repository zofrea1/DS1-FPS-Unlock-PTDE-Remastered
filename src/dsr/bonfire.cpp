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
//   * A played softlock left every menu dword at 0 and ChrIns+0xD0 at 0. The current animation
//     DSR-Gadget reads, *( *(player + 0x68) + 0x48 ) + 0x80, was 7501 (use humanity) and then
//     empty. The sit itself was not in that word. While a bonfire menu was just open, the player,
//     that animation object, and the stay-animation block (*( *(player + 0x30) + 0x5D0 ), upper
//     body at +0x690 and lower body at +0x13B0) are scanned for 7701, 7711, and 7721.
//   * If the humanity-use word goes empty after the menu closes, it is set to 0 once. That is the
//     same kick PTDE uses, aimed at the word that actually changed during the softlock.
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
// than a missed kick, so every read and the write are guarded, and a pointer below 16 MB
// is treated as not ready.

namespace {

constexpr uint32_t kWorldChrManRva = 0x1C77E50;
constexpr uint32_t kMenuManRva = 0x1C88D98;
constexpr uint32_t kPlayerOffset = 0x68;
// Heap objects in this process sit well above 16 MB. Smaller values are counts and flags.
constexpr uintptr_t kMinPointer = 0x1000000;
constexpr uintptr_t kMaxPointer = 0x00007FFFFFFFFFFFull;

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

// 7501 is the humanity-use animation. It played during the softlock and must not be cleared.
constexpr int kHumanityUse = 7501;

// Empty, idle, or a seated loop. Not the stand-up ids, and not the humanity use.
bool idle_kick(int anim) {
    return anim == -1 || anim == 0 || is_sit(anim);
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
    uintptr_t cur_address = 0;
    bool stay_ok = false;
    int stay_upper = -1;
    int stay_lower = -1;
    Hit hits[8] = {};
    int hit_count = 0;
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
    out->cur_address = block + 0x80;
    out->cur = read_int(out->cur_address);
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

// look scans for a sit id. The current-animation word is read either way.
bool sample_player(uintptr_t image, bool look, Sample* out) {
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
        for (size_t i = 0; i < sizeof(kMenuFlags) / sizeof(kMenuFlags[0]); ++i) {
            uint32_t flag = 0;
            if (read_u32(menus + kMenuFlags[i], &flag)) {
                out->flags[i] = flag;
            }
        }
    }
    if (!look) {
        return true;
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
        if (write_u32(hit.address, 0)) {
            ++*fixed;
            LOG_INFO("Bonfire softlock: no menu open for a second while seated; told the character to stand (%d, %s+%x was %d)",
                     *fixed, hit.where, hit.offset, hit.value);
        } else {
            LOG_ERROR("Bonfire softlock: could not write the animation id");
        }
    }
}

// True once this slot should not be tried again. A humanity use still in progress stays armed.
bool kick_humanity_slot(uintptr_t address, int* fixed) {
    const int value = read_int(address);
    if (value == kHumanityUse) {
        return false;
    }
    if (!idle_kick(value)) {
        return true;
    }
    if (write_u32(address, 0)) {
        ++*fixed;
        LOG_INFO("Bonfire softlock: humanity use ended while seated; told the character to stand (%d)", *fixed);
    } else {
        LOG_ERROR("Bonfire softlock: could not write the animation id");
    }
    return true;
}

DWORD WINAPI bonfire_thread(void*) {
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    Sleep(2000);
    ULONGLONG since = 0;
    ULONGLONG last_log = 0;
    ULONGLONG menu_seen = 0;
    uintptr_t humanity_slot = 0;
    int logs = 0;
    int missed_player = 0;
    int fixed = 0;
    while (GetModuleHandleW(nullptr)) {
        Sleep(200);
        const ULONGLONG now = GetTickCount64();
        const bool recent = menu_seen != 0 && now - menu_seen < 30000;
        Sample sample;
        const bool got = sample_player(image, recent, &sample);
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
            if (!recent) {
                sample = Sample();
                sample_player(image, true, &sample);
            }
        }
        if (sample.cur_ok && sample.cur == kHumanityUse && menu_seen != 0 && now - menu_seen < 30000) {
            humanity_slot = sample.cur_address;
        }

        const bool interesting = any_menu || (menu_seen != 0 && now - menu_seen < 30000);
        if (interesting && logs < 80 && now - last_log >= 1000) {
            last_log = now;
            ++logs;
            char menus[160];
            char hits[192];
            char cur[16];
            char stay_upper[16];
            char stay_lower[16];
            format_menus(sample, menus, sizeof(menus));
            format_hits(sample, hits, sizeof(hits));
            format_word(sample.cur_ok, sample.cur, cur, sizeof(cur));
            format_word(sample.stay_ok, sample.stay_upper, stay_upper, sizeof(stay_upper));
            format_word(sample.stay_ok, sample.stay_lower, stay_lower, sizeof(stay_lower));
            LOG_INFO("Bonfire watch: cur=%s stayU=%s stayL=%s hits=%s menus=%s", cur, stay_upper, stay_lower, hits,
                     menus);
        }

        const bool humanity_playing = sample.cur_ok && sample.cur == kHumanityUse;
        const bool humanity_done = humanity_slot != 0 && !humanity_playing;
        if ((!sample.hit_count && !humanity_done) || !menus_clear(sample)) {
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
        if (sample.hit_count) {
            clear_sits(sample, &fixed);
        }
        if (humanity_done) {
            const uintptr_t slot = sample.cur_ok && sample.cur_address ? sample.cur_address : humanity_slot;
            if (kick_humanity_slot(slot, &fixed)) {
                humanity_slot = 0;
            }
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
