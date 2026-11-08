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
constexpr uint32_t kWorldChrMan = 0x0137DC70;
// push ebp ; mov ebp,esp ; and esp,-16 (both functions begin so)
constexpr uint8_t kPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};

constexpr int32_t kWatched[] = {52600, 52601, 52900, 52901, 52920, 52921};
constexpr int kLinesPerBullet = 48;  // the acid has 9 updates at 30 FPS, 18 at 60, 35 at 120
constexpr int kLinesPerPool = 3;
constexpr int kMaxLines = 4000;

struct Seen {
    const uint8_t* bullet = nullptr;
    int32_t id = 0;
    int lines = 0;
    uint32_t serial = 0;
};
Seen g_seen[32];
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
T rd(const uint8_t* base, uint32_t off, T fallback = T{}) {
    T v = fallback;
    read(base + off, &v, sizeof(v));
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

bool player_pos(float out[3]) {
    const uint8_t* wcm = rd<const uint8_t*>(reinterpret_cast<const uint8_t*>(kWorldChrMan), 0);
    if (!wcm) return false;
    const uint8_t* first = rd<const uint8_t*>(wcm, 4);
    const uint8_t* chr = first ? rd<const uint8_t*>(first, 0) : nullptr;
    const uint8_t* mc = chr ? rd<const uint8_t*>(chr, 0x28) : nullptr;
    const uint8_t* phys = mc ? rd<const uint8_t*>(mc, 0x1C) : nullptr;
    return phys && read(phys + 0x10, out, 12);
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

void __stdcall on_update(const uint8_t* b, float dt) {
    if (!b || g_lines >= kMaxLines) return;
    const int state = rd<int32_t>(b, 0x70, 0);
    if (state < 1 || state > 3) return;  // 4: over, until the game frees it
    const int32_t id = state_id(b, state);
    if (!watched(id) && !watched(rd<int32_t>(b, 0x290, -1))) return;
    bool fresh = false;
    Seen* s = seen_for(b, id, &fresh);
    const bool pool = id == 52601 || id == 52901 || id == 52921;
    if (s->lines >= (pool ? kLinesPerPool : kLinesPerBullet)) return;
    ++s->lines;
    ++g_lines;
    float pos[4] = {}, vel[4] = {};
    read(b + 0x10, pos, 16);
    read(b + 0x30, vel, 16);
    const float life = rd<float>(b, 0x1D4);
    const float dist = rd<float>(b, 0x1D0);
    const uint8_t flags = rd<uint8_t>(b, 0x270);
    const int32_t hit_bullet = rd<int32_t>(b, 0x2A8, -1);
    if (fresh) {
        float me[3] = {};
        const bool have_me = player_pos(me);
        LOG_INFO("[bullet] %p new: bullet %d (hit bullet %d), state %d; player at (%.2f, %.2f, %.2f)%s; frame %.4f s",
                 b, id, hit_bullet, state, me[0], me[1], me[2], have_me ? "" : " (unknown)", fixes_last_dt());
    }
    LOG_INFO("[bullet] %p %d #%d dt %.4f life %.4f pos (%.3f, %.3f, %.3f) vel (%.2f, %.2f, %.2f) dist %.3f state %d "
             "flags %02X",
             b, id, s->lines, dt, life, pos[0], pos[1], pos[2], vel[0], vel[1], vel[2], dist, state, flags);
}

void __stdcall on_hit_bullet(const uint8_t* b, int32_t target, const float* where) {
    if (!b || g_lines >= kMaxLines) return;
    const int32_t fly = rd<int32_t>(b, 0x290, -1);
    if (!watched(fly) && !watched(state_id(b, rd<int32_t>(b, 0x70, 0)))) return;
    ++g_lines;
    float p[4] = {};
    const bool have = where && read(where, p, 16);
    float me[3] = {};
    const bool have_me = player_pos(me);
    LOG_INFO("[bullet] %p %d makes hit bullet %d at (%.3f, %.3f, %.3f)%s, target %d, flags %02X, life %.4f; player at "
             "(%.2f, %.2f, %.2f)%s",
             b, fly, rd<int32_t>(b, 0x2A8, -1), p[0], p[1], p[2], have ? "" : " (unreadable)", target,
             rd<uint8_t>(b, 0x270), rd<float>(b, 0x1D4), me[0], me[1], me[2], have_me ? "" : " (unknown)");
}

// The game's own registers and xmm are kept; the hooked function then runs its prologue as written.
__declspec(naked) void update_stub() {
    __asm {
        pushad
        sub esp, 0x80
        movups xmmword ptr [esp + 0x00], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        mov ecx, [esp + 0xA0 + 4]
        push ecx
        push eax
        call on_update
        movups xmm0, xmmword ptr [esp + 0x00]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 0DAC246h
        ret
    }
}

__declspec(naked) void hit_bullet_stub() {
    __asm {
        pushad
        sub esp, 0x80
        movups xmmword ptr [esp + 0x00], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        mov ecx, [esp + 0xA0 + 0x0C]
        push ecx
        mov ecx, [esp + 0xA4 + 0x08]
        push ecx
        mov ecx, [esp + 0xA8 + 0x04]
        push ecx
        call on_hit_bullet
        movups xmm0, xmmword ptr [esp + 0x00]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 0DA9606h
        ret
    }
}

bool hook(uint32_t site, const void* stub) {
    if (std::memcmp(reinterpret_cast<const void*>(site), kPrologue, sizeof(kPrologue)) != 0) {
        LOG_ERROR("BulletTrace: %08X does not match this build. Not hooked.", site);
        return false;
    }
    DWORD old = 0;
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (!VirtualProtect(p, sizeof(kPrologue), PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(stub) - (site + 5));
    p[0] = 0xE9;
    std::memcpy(p + 1, &rel, 4);
    p[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(kPrologue));
    DWORD ignored = 0;
    VirtualProtect(p, sizeof(kPrologue), old, &ignored);
    return true;
}

}  // namespace

bool bullettrace_install() {
    const bool a = hook(kBulletUpdate, &update_stub);
    const bool b = hook(kHitBullet, &hit_bullet_stub);
    if (a || b) {
        LOG_INFO("BulletTrace: the dragons' acid (bullets 52600, 52900, 52920) and its pool are logged ([bullet] lines)");
    }
    return a && b;
}
