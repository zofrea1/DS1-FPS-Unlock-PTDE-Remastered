#include "bullettrace.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kBulletUpdate = 0x00DAC240;  // BulletIns update: eax = bullet, [esp+4] = frame time
constexpr uint32_t kHitBullet = 0x00DA9600;     // a bullet's hit bullet: [esp+4] bullet, [esp+8] target, [esp+0xC] position
constexpr uint32_t kMapHitGate = 0x01025950;    // a bullet's contact with the map: eax = bullet, [esp+4] hit, [esp+8] victim
constexpr uint32_t kMapHit = 0x00DAC120;        // ... passed: edi = bullet, [esp+4] hit, [esp+8] flag
constexpr uint32_t kGroundHit = 0x010258B0;     // a map-colliding bullet's map contact: eax = bullet, [esp+4] hit, [esp+8] victim
constexpr uint32_t kLand = 0x00DAA1D0;          // ... taken (snap onto the surface, slide): edi = bullet, [esp+4] hit
constexpr uint32_t kQuery = 0x00DF1E70;         // each attack hit entry's sweep, once a frame: [esp+4] entry
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
// The undead dragon fires three globs every 0.1 s for about two seconds: the first few of a burst are logged
// frame by frame, every one gets a summary when it is over.
constexpr int kDetailedPerBurst = 6;
constexpr double kBurstGap = 1.5;  // seconds without a new watched bullet start a new burst
constexpr int kUpdatesAfterLanding = 3;
constexpr int kMaxUpdateLines = 70;
constexpr int kMaxContactLines = 16;
constexpr int kMaxGroundLines = 3;
constexpr int kMaxLines = 14000;

struct Seen {
    const uint8_t* bullet = nullptr;
    int32_t id = 0;
    bool alive = false;
    bool detailed = false;
    double born = 0;
    int updates = 0;
    int update_lines = 0;
    int queries = 0;
    int contacts = 0;
    int contact_lines = 0;
    int ground = 0;
    int ground_lines = 0;
    int lands = 0;
    int first_land = 0;  // the update of the first landing, 0 = none yet
    double first_land_age = 0;
    float lowest = 1e9f;
    float last[3] = {};
    uint32_t serial = 0;
    // The glob's effect (its sfx slot at +0x58: [+4] the effect system, [+8] the instance): places in the instance's
    // objects that held the glob's position when first looked at, logged beside it every update after.
    bool fx_searched = false;
    int fx_tries = 0;
    uint32_t fx_at[8] = {};
    int fx_n = 0;
};
Seen g_seen[96];
int32_t g_kinds[256];  // every bullet id seen once, so a trace that watches the wrong ids still says what flew
int g_kind_count = 0;
uint32_t g_serial = 0;
int g_lines = 0;
double g_burst_last = -100;
int g_burst_detailed = 0;
int g_burst = 0;

double now_s() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// How many bytes from p can be read without touching a guard page (another thread's stack) or an uncommitted one.
uint32_t readable_span(uint32_t p, uint32_t want) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return 0;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    if ((mbi.Protect & ok) == 0) return 0;
    const uint32_t end = reinterpret_cast<uint32_t>(mbi.BaseAddress) + static_cast<uint32_t>(mbi.RegionSize);
    return (end - p) < want ? end - p : want;
}

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

void summarize(Seen& s, const char* why) {
    if (!s.alive) return;
    s.alive = false;
    if (g_lines >= kMaxLines) return;
    ++g_lines;
    LOG_INFO("[bullet] %p %d summary (%s): %.2f s, %d updates, %d hit queries, %d sweep contacts, %d ground contacts, "
             "%d landings, first landing %s at update %d (%.2f s); lowest y %.3f, last (%.3f, %.3f, %.3f); burst %d%s",
             s.bullet, s.id, why, now_s() - s.born, s.updates, s.queries, s.contacts, s.ground, s.lands,
             s.first_land ? "" : "NONE", s.first_land, s.first_land_age, s.lowest, s.last[0], s.last[1], s.last[2],
             g_burst, s.detailed ? ", detailed" : "");
}

Seen* find(const uint8_t* b) {
    for (Seen& s : g_seen) {
        if (s.alive && s.bullet == b) return &s;
    }
    return nullptr;
}

Seen* start(const uint8_t* b, int32_t id) {
    Seen* slot = nullptr;
    for (Seen& s : g_seen) {
        if (!s.alive) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        slot = &g_seen[0];
        for (Seen& s : g_seen) {
            if (s.serial < slot->serial) slot = &s;
        }
        summarize(*slot, "slot reused");
    }
    *slot = Seen{};
    slot->bullet = b;
    slot->id = id;
    slot->alive = true;
    slot->serial = ++g_serial;
    slot->born = now_s();
    if (slot->born - g_burst_last > kBurstGap) {
        ++g_burst;
        g_burst_detailed = 0;
    }
    g_burst_last = slot->born;
    slot->detailed = g_burst_detailed < kDetailedPerBurst;
    if (slot->detailed) ++g_burst_detailed;
    return slot;
}

// The hit entry of a live watched bullet, read from the bullet now (it makes a new one every 1/6 s while its
// hit radius grows, in the middle of its update).
Seen* by_entry(const uint8_t* e) {
    if (!e) return nullptr;
    const uint32_t handle = rd<uint32_t>(e, 0, 0xFFFFFFFF);
    if (handle == 0xFFFFFFFF) return nullptr;
    for (Seen& s : g_seen) {
        if (s.alive && rd<uint32_t>(s.bullet, 0x54, 0xFFFFFFFE) == handle) return &s;
    }
    return nullptr;
}

bool heap_pointer(uint32_t v) {
    return v >= 0x00010000 && v < 0x7FFE0000 && (v & 3) == 0 && !(v >= 0x00400000 && v < 0x01600000);
}

// Breadth first (two levels, 0x400 bytes an object) from the glob's effect instance, for three floats within 5 cm of
// the glob's position.
void fx_search(Seen& s, const uint8_t* b, const float pos[3]) {
    const uint8_t* slot = b + 0x58;
    const uint32_t sys = rd<uint32_t>(slot, 4);
    const uint32_t inst = rd<uint32_t>(slot, 8);
    if (!inst) return;  // no effect yet: the next update looks again
    s.fx_searched = true;
    const uint32_t fx = rd<uint32_t>(reinterpret_cast<const void*>(inst), 0x64);
    const uint32_t iface = sys ? rd<uint32_t>(reinterpret_cast<const void*>(sys), 0x15C) : 0;
    const uint32_t vt = iface ? rd<uint32_t>(reinterpret_cast<const void*>(iface), 0) : 0;
    const uint32_t setfn = vt ? rd<uint32_t>(reinterpret_cast<const void*>(vt), 0x4C) : 0;
    uint32_t queue[256];
    int depth[256];
    int n = 0;
    queue[n] = inst;
    depth[n++] = 0;
    if (fx && fx != inst) {
        queue[n] = fx;
        depth[n++] = 0;
    }
    static uint8_t buf[0x400];
    char where[600] = {};
    int wl = 0;
    for (int head = 0; head < n && s.fx_n < 8; ++head) {
        const uint32_t obj = queue[head];
        const uint32_t span = readable_span(obj, sizeof(buf)) & ~3u;
        if (span < 0x10 || !read(reinterpret_cast<const void*>(obj), buf, span)) continue;
        for (uint32_t o = 0; o + 12 <= span && s.fx_n < 8; o += 4) {
            float f[3];
            std::memcpy(f, buf + o, 12);
            if (std::fabs(f[0] - pos[0]) < 0.05f && std::fabs(f[1] - pos[1]) < 0.05f && std::fabs(f[2] - pos[2]) < 0.05f) {
                s.fx_at[s.fx_n++] = obj + o;
                wl += std::snprintf(where + wl, sizeof(where) - wl, " %08X+%X(d%d)", obj, o, depth[head]);
                o += 8;
            }
        }
        if (depth[head] >= 2) continue;
        for (uint32_t o = 0; o + 4 <= span && n < 256; o += 4) {
            uint32_t v;
            std::memcpy(&v, buf + o, 4);
            if (!heap_pointer(v)) continue;
            bool dup = false;
            for (int k = 0; k < n && !dup; ++k) dup = queue[k] == v;
            if (dup) continue;
            queue[n] = v;
            depth[n++] = depth[head] + 1;
        }
    }
    if (g_lines < kMaxLines) {
        ++g_lines;
        LOG_INFO("[bullet] %p %d effect: system %08X (set-transform %08X), instance %08X, object %08X; %d objects searched; "
                 "the glob's position at:%s",
                 b, s.id, sys, setfn, inst, fx, n, s.fx_n ? where : " none");
    }
}

void __stdcall on_update(const uint8_t* b, float dt) {
    if (!b) return;
    const int state = rd<int32_t>(b, 0x70, 0);
    if (state < 1 || state > 3) {  // 4: over, until the game frees it
        if (Seen* s = find(b)) summarize(*s, "over");
        return;
    }
    const int32_t id = state_id(b, state);
    if (id >= 0 && g_kind_count < 256) {
        bool known = false;
        for (int i = 0; i < g_kind_count && !known; ++i) known = g_kinds[i] == id;
        if (!known && g_lines < kMaxLines) {
            g_kinds[g_kind_count++] = id;
            ++g_lines;
            float pos[3] = {};
            read(b + 0x10, pos, 12);
            LOG_INFO("[bullet] first of its kind: bullet %d (state %d) at (%.2f, %.2f, %.2f)", id, state, pos[0],
                     pos[1], pos[2]);
        }
    }
    Seen* s = find(b);
    if (s && s->id != id) {
        summarize(*s, "now another bullet");
        s = nullptr;
    }
    if (!watched(id)) return;
    const bool fresh = s == nullptr;
    if (!s) s = start(b, id);
    ++s->updates;
    float pos[4] = {}, vel[4] = {};
    read(b + 0x10, pos, 16);
    read(b + 0x30, vel, 16);
    std::memcpy(s->last, pos, 12);
    if (pos[1] < s->lowest) s->lowest = pos[1];
    if (!s->detailed || g_lines >= kMaxLines || s->update_lines >= kMaxUpdateLines) return;
    if (s->first_land && s->updates > s->first_land + kUpdatesAfterLanding) return;
    ++s->update_lines;
    ++g_lines;
    if (fresh) {
        float me[3] = {};
        const bool have_me = player_pos(me);
        ++g_lines;
        LOG_INFO("[bullet] %p new: bullet %d, burst %d, state %d; player at (%.2f, %.2f, %.2f)%s; frame %.4f s", b, id,
                 g_burst, state, me[0], me[1], me[2], have_me ? "" : " (unknown)", fixes_last_dt());
    }
    if (!s->fx_searched && s->fx_tries++ < 4) fx_search(*s, b, pos);
    char fxs[400] = {};
    int fl = 0;
    for (int i = 0; i < s->fx_n; ++i) {
        float f[3] = {};
        read(reinterpret_cast<const void*>(s->fx_at[i]), f, 12);
        fl += std::snprintf(fxs + fl, sizeof(fxs) - fl, " (%.3f, %.3f, %.3f)", f[0], f[1], f[2]);
    }
    const uint32_t handle = rd<uint32_t>(b, 0x54, 0xFFFFFFFF);
    const uint8_t* e = hit_entry(handle);
    float cur[3] = {}, prev[3] = {};
    if (e) {
        read(e + 0x130, cur, 12);
        read(e + 0x150, prev, 12);
    }
    LOG_INFO("[bullet] %p %d #%d dt %.4f life %.3f pos (%.3f, %.3f, %.3f) vel (%.2f, %.2f, %.2f) dist %.3f flags %02X "
             "ground %.3f | entry %08X %s(%.3f, %.3f, %.3f) <- (%.3f, %.3f, %.3f) done %d first %d; queries %d contacts %d"
             " | effect%s",
             b, id, s->updates, dt, rd<float>(b, 0x1D4), pos[0], pos[1], pos[2], vel[0], vel[1], vel[2],
             rd<float>(b, 0x1D0), rd<uint8_t>(b, 0x270), rd<float>(b, 0x50), handle, e ? "" : "(none) ", cur[0],
             cur[1], cur[2], prev[0], prev[1], prev[2], rd<int32_t>(e, 0xFC), rd<uint8_t>(e, 0x164), s->queries,
             s->contacts, s->fx_n ? fxs : " -");
}

// 0xDA9600 also runs when a bullet's life is over (0xDAA0E0), with HitBulletID -1 when it has none: its summary.
// (The game frees a bullet that is over without another update, so a summary from the update never came.)
void __stdcall on_hit_bullet(const uint8_t* b, int32_t target, const float* where) {
    Seen* s = find(b);
    if (!s) return;
    const int32_t hit = rd<int32_t>(b, 0x2A8, -1);
    if (hit >= 0 && s->detailed && g_lines < kMaxLines) {
        ++g_lines;
        float p[4] = {};
        const bool have = where && read(where, p, 16);
        LOG_INFO("[bullet] %p %d makes hit bullet %d at (%.3f, %.3f, %.3f)%s, target %d, flags %02X, life %.3f", b,
                 s->id, hit, p[0], p[1], p[2], have ? "" : " (unreadable)", target, rd<uint8_t>(b, 0x270),
                 rd<float>(b, 0x1D4));
    }
    summarize(*s, rd<float>(b, 0x1D4) <= 0.0f ? "life over" : "hit");
}

// 0x1025950: the bullet's state takes map hits (vtable +0x20), the hit's +0x34 is below 0x40, and the victim
// (+4) is not the bullet's owner (+0x8C); then 0xDAC120 plays the hit effect where it touched (+0xD0).
void __stdcall on_map_gate(const uint8_t* b, const uint8_t* hit, const uint8_t* victim) {
    Seen* s = find(b);
    if (!s || !s->detailed || g_lines >= kMaxLines || s->ground_lines >= kMaxGroundLines) return;
    ++s->ground_lines;
    ++g_lines;
    float p[3] = {};
    read(hit + 0xD0, p, 12);
    LOG_INFO("[bullet] %p %d object contact at (%.3f, %.3f, %.3f): hit+34 %d, victim %08X, owner %08X", b, s->id, p[0],
             p[1], p[2], rd<int32_t>(hit, 0x34, -1), rd<uint32_t>(victim, 4), rd<uint32_t>(b, 0x8C));
}

void __stdcall on_map_hit(const uint8_t* b, const uint8_t* hit, uint32_t flag) {
    Seen* s = find(b);
    if (!s || !s->detailed || g_lines >= kMaxLines) return;
    ++g_lines;
    float p[3] = {};
    read(hit + 0xD0, p, 12);
    LOG_INFO("[bullet] %p %d object contact taken at (%.3f, %.3f, %.3f), flag %u", b, s->id, p[0], p[1], p[2],
             flag & 0xFF);
}

// 0x10258B0 (the map, for a bullet that does not go through it): 0xDA3190 (its state takes map hits, not
// isPenetrateMap) and the hit's +0x34 below 0x40 lead to 0xDAA1D0, which puts it on the surface (+0xE0 the normal).
void __stdcall on_ground_hit(const uint8_t* b, const uint8_t* hit) {
    Seen* s = find(b);
    if (!s) return;
    ++s->ground;
    if (!s->detailed || g_lines >= kMaxLines || s->ground_lines >= kMaxGroundLines) return;
    ++s->ground_lines;
    ++g_lines;
    float p[3] = {}, n[3] = {}, v[3] = {};
    read(hit + 0xD0, p, 12);
    read(hit + 0xE0, n, 12);
    read(b + 0x30, v, 12);
    LOG_INFO("[bullet] %p %d map contact (ground) at (%.3f, %.3f, %.3f) normal (%.2f, %.2f, %.2f): hit+34 %d, vel "
             "(%.2f, %.2f, %.2f), ground %.3f, update %d",
             b, s->id, p[0], p[1], p[2], n[0], n[1], n[2], rd<int32_t>(hit, 0x34, -1), v[0], v[1], v[2],
             rd<float>(b, 0x50), s->updates);
}

void __stdcall on_land(const uint8_t* b, const uint8_t* hit) {
    Seen* s = find(b);
    if (!s) return;
    ++s->lands;
    if (!s->first_land) {
        s->first_land = s->updates;
        s->first_land_age = now_s() - s->born;
    }
    if (!s->detailed || g_lines >= kMaxLines || s->lands > kMaxGroundLines) return;
    ++g_lines;
    float p[3] = {};
    read(b + 0x10, p, 12);
    LOG_INFO("[bullet] %p %d lands at (%.3f, %.3f, %.3f), ground %.3f, update %d", b, s->id, p[0], p[1], p[2],
             rd<float>(b, 0x50), s->updates);
}

// 0xDF1E70, once a frame for each attack hit entry: the sweep of its shape from where it was to where it is.
void __stdcall on_query(const uint8_t* entry) {
    Seen* s = by_entry(entry);
    if (s) ++s->queries;
}

void __stdcall on_contact(const uint8_t* entry, const float* point, const uint8_t* victim) {
    Seen* s = by_entry(entry);
    if (!s) return;
    ++s->contacts;
    if (!s->detailed || g_lines >= kMaxLines || s->contact_lines >= kMaxContactLines) return;
    ++s->contact_lines;
    ++g_lines;
    float p[3] = {};
    read(point, p, 12);
    const uint8_t* owner = rd<const uint8_t*>(victim, 0xC);
    const uint8_t* obj = rd<const uint8_t*>(owner, 0);
    LOG_INFO("[bullet] %p %d sweep contact at (%.3f, %.3f, %.3f), victim %p kind %u, update %d", s->bullet, s->id,
             p[0], p[1], p[2], victim, rd<uint16_t>(obj, 4, 0xFFFF), s->updates);
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

__declspec(naked) void query_stub() {
    SAVE_ALL
    __asm {
        mov ecx, [esp + 0xA0 + 0x04]
        push ecx
        call on_query
    }
    RESTORE_ALL
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        push 0DF1E76h
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
    ok += hook(kQuery, kPrologue, &query_stub);
    ok += hook(kContact, kContactPrologue, &contact_stub);
    LOG_INFO("BulletTrace: %d of 8 hooks; the undead dragon's acid (34200-34209), c5390's 53900-53902 and the Gaping "
             "Dragon's 52600/52601 are logged with their map contacts ([bullet] lines: the first %d of each burst frame "
             "by frame, a summary for every one)",
             ok, kDetailedPerBurst);
    return ok == 8;
}
