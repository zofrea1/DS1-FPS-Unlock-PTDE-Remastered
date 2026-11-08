#include "bullettrace.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kBulletUpdate = 0x00DAC240;  // BulletIns update: eax = bullet, [esp+4] = frame time
constexpr uint32_t kHitBullet = 0x00DA9600;     // a bullet's hit bullet: [esp+4] bullet, [esp+8] target, [esp+0xC] position
constexpr uint32_t kMapHitGate = 0x01025950;    // a bullet's contact with the map: eax = bullet, [esp+4] hit, [esp+8] victim
constexpr uint32_t kMapHit = 0x00DAC120;        // ... passed: edi = bullet, [esp+4] hit, [esp+8] flag
constexpr uint32_t kGroundHit = 0x010258B0;     // a map-colliding bullet's map contact: eax = bullet, [esp+4] hit, [esp+8] victim
constexpr uint32_t kLand = 0x00DAA1D0;          // ... taken (snap onto the surface, slide): edi = bullet, [esp+4] hit
constexpr uint32_t kContact = 0x00DF1CB0;       // each contact an attack's sweep finds: [esp+4] entry, [esp+8] point, edi = victim
constexpr uint32_t kWorldChrMan = 0x0137DC70;
constexpr uint32_t kHitMan = 0x0137D6AC;  // the attack hit entries (0x1E0 each), by the handle a bullet keeps at +0x54
// push ebp ; mov ebp,esp ; and esp,-16
constexpr uint8_t kPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
// push ecx ; push ebx ; mov ebx,[esp+0Ch]
constexpr uint8_t kContactPrologue[6] = {0x51, 0x53, 0x8B, 0x5C, 0x24, 0x0C};

// The undead dragon (c3420, Valley of Drakes and the Painted World): 34200-34209, its acid (FollowType 4: falls,
// lands on the map and slides). c5390's 53900-53902 and the Gaping Dragon's (c5260) 52600 and its pool 52601.
constexpr int32_t kWatched[] = {34200, 34201, 34202, 34203, 34204, 34205, 34206, 34207, 34208, 34209,
                                53900, 53901, 53902, 52600, 52601};
constexpr int kLinesPerBullet = 24;
constexpr int kContactsPerBullet = 12;
constexpr int kMaxLines = 14000;

struct Seen {
    const uint8_t* bullet = nullptr;
    const uint8_t* entry = nullptr;
    uint32_t handle = 0xFFFFFFFF;  // the entry's own handle: a slot is reused once its attack is over
    int32_t id = 0;
    int lines = 0;
    int contacts = 0;
    uint32_t serial = 0;
};
Seen g_seen[64];
int32_t g_kinds[256];  // every bullet id seen once, so a trace that watches the wrong ids still says what flew
int g_kind_count = 0;
uint32_t g_serial = 0;
int g_lines = 0;

bool watched(int32_t id) {
    for (int32_t w : kWatched) {
        if (w == id) return true;
    }
    return false;
}

bool read(const void* p, void* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
T rd(const void* base, uint32_t off, T fallback = T{}) {
    T v = fallback;
    if (base) read(static_cast<const uint8_t*>(base) + off, &v, sizeof(v));
    return v;
}

// The bullet's state slot (+0x70: 1 wait at +0x274, 2 fly at +0x28C, 3 explosion at +0x2A4; 4 = over) holds
// its vtable, its BulletParam id and the row.
int32_t state_id(const uint8_t* b, int state) {
    switch (state) {
        case 1: return rd<int32_t>(b, 0x278, -1);
        case 2: return rd<int32_t>(b, 0x290, -1);
        case 3: return rd<int32_t>(b, 0x2A8, -1);
        default: return -1;
    }
}

int32_t bullet_id(const uint8_t* b) {
    const int32_t id = state_id(b, rd<int32_t>(b, 0x70, 0));
    return id >= 0 ? id : rd<int32_t>(b, 0x290, -1);
}

bool player_pos(float out[3]) {
    const uint8_t* wcm = rd<const uint8_t*>(reinterpret_cast<const void*>(kWorldChrMan), 0);
    const uint8_t* first = rd<const uint8_t*>(wcm, 4);
    const uint8_t* chr = rd<const uint8_t*>(first, 0);
    const uint8_t* mc = rd<const uint8_t*>(chr, 0x28);
    const uint8_t* phys = rd<const uint8_t*>(mc, 0x1C);
    return phys && read(phys + 0x10, out, 12);
}

// 0xDE91C0: handle >> 16 below 0x80 indexes the table at [man+8]; 0x80 is a list from [man] linked at +0x1D0.
const uint8_t* hit_entry(uint32_t handle) {
    const uint8_t* man = rd<const uint8_t*>(reinterpret_cast<const void*>(kHitMan), 0);
    if (!man || handle == 0xFFFFFFFF) return nullptr;
    const uint32_t idx = handle >> 16;
    const uint8_t* e = nullptr;
    if (idx < 0x80) {
        const uint8_t* table = rd<const uint8_t*>(man, 8);
        e = table ? table + idx * 0x1E0 : nullptr;
    } else if (idx == 0x80) {
        e = rd<const uint8_t*>(man, 0);
        for (int guard = 0; e && rd<uint32_t>(e, 0) != handle && guard < 256; ++guard) e = rd<const uint8_t*>(e, 0x1D0);
    }
    return e && rd<uint32_t>(e, 0) == handle ? e : nullptr;
}

Seen* seen_for(const uint8_t* b, int32_t id, bool* fresh) {
    Seen* oldest = &g_seen[0];
    for (Seen& s : g_seen) {
        if (s.bullet == b && s.id == id) {
            *fresh = false;
            return &s;
        }
        if (s.serial < oldest->serial) oldest = &s;
    }
    *oldest = Seen{};
    oldest->bullet = b;
    oldest->id = id;
    oldest->serial = ++g_serial;
    *fresh = true;
    return oldest;
}

Seen* by_entry(const uint8_t* e) {
    if (!e) return nullptr;
    const uint32_t handle = rd<uint32_t>(e, 0, 0xFFFFFFFF);
    for (Seen& s : g_seen) {
        if (s.entry == e && s.handle == handle) return &s;
    }
    return nullptr;
}

void __stdcall on_update(const uint8_t* b, float dt) {
    if (!b || g_lines >= kMaxLines) return;
    const int state = rd<int32_t>(b, 0x70, 0);
    if (state < 1 || state > 3) return;  // 4: over, until the game frees it
    const int32_t id = state_id(b, state);
    if (id >= 0 && g_kind_count < 256) {
        bool known = false;
        for (int i = 0; i < g_kind_count && !known; ++i) known = g_kinds[i] == id;
        if (!known) {
            g_kinds[g_kind_count++] = id;
            ++g_lines;
            float pos[3] = {};
            read(b + 0x10, pos, 12);
            LOG_INFO("[bullet] first of its kind: bullet %d (state %d) at (%.2f, %.2f, %.2f)", id, state, pos[0],
                     pos[1], pos[2]);
        }
    }
    if (!watched(id)) return;
    bool fresh = false;
    Seen* s = seen_for(b, id, &fresh);
    s->handle = rd<uint32_t>(b, 0x54, 0xFFFFFFFF);
    const uint8_t* e = hit_entry(s->handle);
    s->entry = e;
    if (s->lines >= kLinesPerBullet) return;
    ++s->lines;
    ++g_lines;
    float pos[4] = {}, vel[4] = {};
    read(b + 0x10, pos, 16);
    read(b + 0x30, vel, 16);
    if (fresh) {
        float me[3] = {};
        const bool have_me = player_pos(me);
        LOG_INFO("[bullet] %p new: bullet %d, state %d; player at (%.2f, %.2f, %.2f)%s; frame %.4f s", b, id, state,
                 me[0], me[1], me[2], have_me ? "" : " (unknown)", fixes_last_dt());
    }
    float cur[3] = {}, prev[3] = {};
    if (e) {
        read(e + 0x130, cur, 12);
        read(e + 0x150, prev, 12);
    }
    LOG_INFO("[bullet] %p %d #%d dt %.4f life %.3f pos (%.3f, %.3f, %.3f) vel (%.2f, %.2f, %.2f) dist %.3f state %d "
             "flags %02X ground %.3f | entry %p %s(%.3f, %.3f, %.3f) <- (%.3f, %.3f, %.3f) ttl %.3f done %d first %d",
             b, id, s->lines, dt, rd<float>(b, 0x1D4), pos[0], pos[1], pos[2], vel[0], vel[1], vel[2],
             rd<float>(b, 0x1D0), state, rd<uint8_t>(b, 0x270), rd<float>(b, 0x50), e, e ? "" : "(none) ",
             cur[0], cur[1], cur[2], prev[0], prev[1], prev[2], rd<float>(e, 0xF8), rd<int32_t>(e, 0xFC),
             rd<uint8_t>(e, 0x164));
}

void __stdcall on_hit_bullet(const uint8_t* b, int32_t target, const float* where) {
    if (!b || g_lines >= kMaxLines || !watched(bullet_id(b))) return;
    ++g_lines;
    float p[4] = {};
    const bool have = where && read(where, p, 16);
    LOG_INFO("[bullet] %p %d makes hit bullet %d at (%.3f, %.3f, %.3f)%s, target %d, flags %02X, life %.3f", b,
             bullet_id(b), rd<int32_t>(b, 0x2A8, -1), p[0], p[1], p[2], have ? "" : " (unreadable)", target,
             rd<uint8_t>(b, 0x270), rd<float>(b, 0x1D4));
}

// 0x1025950: the bullet's state takes map hits (vtable +0x20), the hit's +0x34 is below 0x40, and the victim
// (+4) is not the bullet's owner (+0x8C); then 0xDAC120 plays the hit effect where it touched (+0xD0).
void __stdcall on_map_gate(const uint8_t* b, const uint8_t* hit, const uint8_t* victim) {
    if (!b || g_lines >= kMaxLines || !watched(bullet_id(b))) return;
    ++g_lines;
    float p[3] = {};
    read(hit + 0xD0, p, 12);
    LOG_INFO("[bullet] %p %d map contact at (%.3f, %.3f, %.3f): hit+34 %d, victim %08X, owner %08X, state %d", b,
             bullet_id(b), p[0], p[1], p[2], rd<int32_t>(hit, 0x34, -1), rd<uint32_t>(victim, 4),
             rd<uint32_t>(b, 0x8C), rd<int32_t>(b, 0x70));
}

void __stdcall on_map_hit(const uint8_t* b, const uint8_t* hit, uint32_t flag) {
    if (!b || g_lines >= kMaxLines || !watched(bullet_id(b))) return;
    ++g_lines;
    float p[3] = {};
    read(hit + 0xD0, p, 12);
    LOG_INFO("[bullet] %p %d map hit taken at (%.3f, %.3f, %.3f), flag %u, hit-timer %.3f: its hit effect plays", b,
             bullet_id(b), p[0], p[1], p[2], flag & 0xFF, rd<float>(b, 0x1DC));
}

// 0x10258B0 (the map, for a bullet that does not go through it): 0xDA3190 (its state takes map hits, not
// isPenetrateMap) and the hit's +0x34 below 0x40 lead to 0xDAA1D0, which puts it on the surface (+0xE0 the normal).
void __stdcall on_ground_hit(const uint8_t* b, const uint8_t* hit) {
    if (!b || g_lines >= kMaxLines || !watched(bullet_id(b))) return;
    ++g_lines;
    float p[3] = {}, n[3] = {}, v[3] = {};
    read(hit + 0xD0, p, 12);
    read(hit + 0xE0, n, 12);
    read(b + 0x30, v, 12);
    LOG_INFO("[bullet] %p %d map contact (ground) at (%.3f, %.3f, %.3f) normal (%.2f, %.2f, %.2f): hit+34 %d, vel "
             "(%.2f, %.2f, %.2f), ground %.3f, state %d",
             b, bullet_id(b), p[0], p[1], p[2], n[0], n[1], n[2], rd<int32_t>(hit, 0x34, -1), v[0], v[1], v[2],
             rd<float>(b, 0x50), rd<int32_t>(b, 0x70));
}

void __stdcall on_land(const uint8_t* b, const uint8_t* hit) {
    if (!b || g_lines >= kMaxLines || !watched(bullet_id(b))) return;
    ++g_lines;
    float p[3] = {};
    read(b + 0x10, p, 12);
    LOG_INFO("[bullet] %p %d lands: taken at bullet (%.3f, %.3f, %.3f), ground %.3f", b, bullet_id(b), p[0], p[1], p[2],
             rd<float>(b, 0x50));
}

void __stdcall on_contact(const uint8_t* entry, const float* point, const uint8_t* victim) {
    if (g_lines >= kMaxLines) return;
    Seen* s = by_entry(entry);
    if (!s || s->contacts >= kContactsPerBullet) return;
    ++s->contacts;
    ++g_lines;
    float p[3] = {};
    read(point, p, 12);
    const uint8_t* owner = rd<const uint8_t*>(victim, 0xC);
    const uint8_t* obj = rd<const uint8_t*>(owner, 0);
    LOG_INFO("[bullet] %p %d sweep contact at (%.3f, %.3f, %.3f), victim %p kind %u", s->bullet, s->id, p[0], p[1],
             p[2], victim, rd<uint16_t>(obj, 4, 0xFFFF));
}

#define SAVE_ALL                                         \
    __asm pushad                                         \
    __asm sub esp, 0x80                                  \
    __asm movups xmmword ptr [esp + 0x00], xmm0          \
    __asm movups xmmword ptr [esp + 0x10], xmm1          \
    __asm movups xmmword ptr [esp + 0x20], xmm2          \
    __asm movups xmmword ptr [esp + 0x30], xmm3          \
    __asm movups xmmword ptr [esp + 0x40], xmm4          \
    __asm movups xmmword ptr [esp + 0x50], xmm5          \
    __asm movups xmmword ptr [esp + 0x60], xmm6          \
    __asm movups xmmword ptr [esp + 0x70], xmm7

#define RESTORE_ALL                                      \
    __asm movups xmm0, xmmword ptr [esp + 0x00]          \
    __asm movups xmm1, xmmword ptr [esp + 0x10]          \
    __asm movups xmm2, xmmword ptr [esp + 0x20]          \
    __asm movups xmm3, xmmword ptr [esp + 0x30]          \
    __asm movups xmm4, xmmword ptr [esp + 0x40]          \
    __asm movups xmm5, xmmword ptr [esp + 0x50]          \
    __asm movups xmm6, xmmword ptr [esp + 0x60]          \
    __asm movups xmm7, xmmword ptr [esp + 0x70]          \
    __asm add esp, 0x80                                  \
    __asm popad

// After SAVE_ALL the caller's [esp+4] is at [esp + 0xA0 + 4]; each push moves it 4 further.
__declspec(naked) void update_stub() {
    SAVE_ALL
    __asm {
        mov ecx, [esp + 0xA0 + 4]
        push ecx
        push eax
        call on_update
    }
    RESTORE_ALL
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 0DAC246h
        ret
    }
}

__declspec(naked) void hit_bullet_stub() {
    SAVE_ALL
    __asm {
        mov ecx, [esp + 0xA0 + 0x0C]
        push ecx
        mov ecx, [esp + 0xA4 + 0x08]
        push ecx
        mov ecx, [esp + 0xA8 + 0x04]
        push ecx
        call on_hit_bullet
    }
    RESTORE_ALL
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 0DA9606h
        ret
    }
}

__declspec(naked) void map_gate_stub() {
    SAVE_ALL
    __asm {
        mov ecx, [esp + 0xA0 + 0x08]
        push ecx
        mov ecx, [esp + 0xA4 + 0x04]
        push ecx
        push eax
        call on_map_gate
    }
    RESTORE_ALL
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 01025956h
        ret
    }
}

__declspec(naked) void map_hit_stub() {
    SAVE_ALL
    __asm {
        mov ecx, [esp + 0xA0 + 0x08]
        push ecx
        mov ecx, [esp + 0xA4 + 0x04]
        push ecx
        push edi
        call on_map_hit
    }
    RESTORE_ALL
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 0DAC126h
        ret
    }
}

__declspec(naked) void ground_hit_stub() {
    SAVE_ALL
    __asm {
        mov ecx, [esp + 0xA0 + 0x04]
        push ecx
        push eax
        call on_ground_hit
    }
    RESTORE_ALL
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 010258B6h
        ret
    }
}

__declspec(naked) void land_stub() {
    SAVE_ALL
    __asm {
        mov ecx, [esp + 0xA0 + 0x04]
        push ecx
        push edi
        call on_land
    }
    RESTORE_ALL
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 0DAA1D6h
        ret
    }
}

__declspec(naked) void contact_stub() {
    SAVE_ALL
    __asm {
        push edi
        mov ecx, [esp + 0xA4 + 0x08]
        push ecx
        mov ecx, [esp + 0xA8 + 0x04]
        push ecx
        call on_contact
    }
    RESTORE_ALL
    __asm {
        push ecx
        push ebx
        mov ebx, [esp + 0Ch]
        push 0DF1CB6h
        ret
    }
}

bool hook(uint32_t site, const uint8_t* expect, const void* stub) {
    if (std::memcmp(reinterpret_cast<const void*>(site), expect, 6) != 0) {
        LOG_ERROR("BulletTrace: %08X does not match this build. Not hooked.", site);
        return false;
    }
    DWORD old = 0;
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(stub) - (site + 5));
    p[0] = 0xE9;
    std::memcpy(p + 1, &rel, 4);
    p[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    DWORD ignored = 0;
    VirtualProtect(p, 6, old, &ignored);
    return true;
}

}  // namespace

bool bullettrace_install() {
    int ok = 0;
    ok += hook(kBulletUpdate, kPrologue, &update_stub);
    ok += hook(kHitBullet, kPrologue, &hit_bullet_stub);
    ok += hook(kMapHitGate, kPrologue, &map_gate_stub);
    ok += hook(kMapHit, kPrologue, &map_hit_stub);
    ok += hook(kGroundHit, kPrologue, &ground_hit_stub);
    ok += hook(kLand, kPrologue, &land_stub);
    ok += hook(kContact, kContactPrologue, &contact_stub);
    LOG_INFO("BulletTrace: %d of 7 hooks; the undead dragon's acid (34200-34209), c5390's 53900-53902 and the Gaping "
             "Dragon's 52600/52601 are logged with their map contacts ([bullet] lines)",
             ok);
    return ok == 7;
}
