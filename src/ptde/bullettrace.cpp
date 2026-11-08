#include "bullettrace.h"
#include "buildcfg.h"

#if FPSU_DIAGNOSTICS

#include "fixes.h"
#include "limiter.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdarg>
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
constexpr uint32_t kAccel = 0x009E6B20;         // the effect library's acceleration movement, once a frame a particle cluster:
                                                // ecx = the movement, [esp+4] the cluster, [esp+8] a time value
// The call to the integrator 0xA4D420. The bytes after it (add esp, 0x54 at 0x9E6C49) are only 3, and the count==0
// path jumps to 0x9E6C4C, so a hook there would land in the middle of that jump. Replacing this 5-byte call leaves
// 0x9E6C4C alone. Verified against the Steam exe: E8 D7 67 06 00.
constexpr uint32_t kAccelCall = 0x009E6C44;
constexpr uint8_t kAccelCallBytes[5] = {0xE8, 0xD7, 0x67, 0x06, 0x00};
constexpr uint32_t kQuery = 0x00DF1E70;         // each attack hit entry's sweep, once a frame: [esp+4] entry
constexpr uint32_t kContact = 0x00DF1CB0;       // each contact an attack's sweep finds: [esp+4] entry, [esp+8] point, edi = victim
constexpr uint32_t kWorldChrMan = 0x0137DC70;
constexpr uint32_t kWorldChrManCoop = 0x0137D644;  // Prepare to Co-op's player: [WorldChrMan+0x3C]
constexpr uint32_t kHitMan = 0x0137D6AC;  // the attack hit entries (0x1E0 each), by the handle a bullet keeps at +0x54
// push ebp ; mov ebp,esp ; and esp,-16
constexpr uint8_t kPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
// push ecx ; push ebx ; mov ebx,[esp+0Ch]
constexpr uint8_t kContactPrologue[6] = {0x51, 0x53, 0x8B, 0x5C, 0x24, 0x0C};
// push ebx ; mov ebx,esp ; sub esp,8
constexpr uint8_t kAccelPrologue[6] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};

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
    int player_hits = 0;  // sweep victim matched the local player's object
    int near_hits = 0;    // kind other than the floor, contact point within 2.5 m of the player
    int contact_lines = 0;
    int ground = 0;
    int ground_lines = 0;
    int lands = 0;
    int first_land = 0;  // the update of the first landing, 0 = none yet
    double first_land_age = 0;
    float lowest = 1e9f;
    float last[3] = {};
    uint32_t serial = 0;
    int burst = 0;
    int cap_born = 0;
    // Closest the glob's centre came to the player's body (a 1.7 m upright segment from the feet), when, and where
    // the player stood then. The hit radius is 0.3 growing to 0.6, so a glob that never comes within about 1 m
    // cannot touch the player at any frame rate.
    float closest = 1e9f;
    double closest_age = 0;
    float closest_you[3] = {};
    // The glob's effect (its sfx slot at +0x58: [+4] the effect system, [+8] the instance): places in the instance's
    // objects that held the glob's position when first looked at, logged beside it every update after.
    bool fx_searched = false;
    int fx_tries = 0;
    uint32_t fx_at[8] = {};
    int fx_n = 0;
    // The glob's effect instance ([bullet+0x60]) as the effect state machine sees it: how many frames it ran for
    // this glob, and the instance's node tree when the glob was new.
    uint32_t fx_inst = 0;
    uint32_t fx_root = 0;  // the state machine's root for it: the root whose first node is the instance
    int fx_calls = 0;
};
Seen g_seen[96];
int32_t g_kinds[256];  // every bullet id seen once, so a trace that watches the wrong ids still says what flew
int g_kind_count = 0;
uint32_t g_serial = 0;
int g_lines = 0;
double g_burst_last = -100;
int g_burst_detailed = 0;
int g_burst = 0;

// One line per burst once its last glob is over: the cap it was fired and ended under, how many globs and how far
// apart, the closest any came to the player, the sweep contacts on the player and the HP the player lost.
struct Burst {
    int id = 0;
    int cap_first = 0;
    int cap_last = 0;
    int globs = 0;
    int alive = 0;
    double first = 0;
    double last_birth = 0;
    float closest = 1e9f;
    int on_you = 0;
    int hp_first = -1;
    int hp_low = -1;
    int hp_max = 0;
    float you_lo[3] = {1e9f, 1e9f, 1e9f};
    float you_hi[3] = {-1e9f, -1e9f, -1e9f};
};
Burst g_bursts[16];

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

const uint8_t* player_chr() {
    const uint8_t* wcm = rd<const uint8_t*>(reinterpret_cast<const void*>(kWorldChrMan), 0);
    const uint8_t* first = rd<const uint8_t*>(wcm, 4);
    return rd<const uint8_t*>(first, 0);
}

Burst& burst_slot(int id) {
    return g_bursts[id & 15];
}

void burst_report(Burst& b) {
    if (b.id == 0) return;
    const int id = b.id;
    b.id = 0;
    if (g_lines >= kMaxLines) return;
    ++g_lines;
    const double span = b.last_birth - b.first;
    const int threes = b.globs / 3;
    LOG_INFO("[bullet] burst %d over: cap %d->%d, %d globs in %.2f s (%.3f s between threes), closest to you %.2f m, "
             "sweep contacts on you %d, HP %d -> lowest %d (max %d), you x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f",
             id, b.cap_first, b.cap_last, b.globs, span, threes > 1 ? span / (threes - 1) : 0.0, b.closest, b.on_you,
             b.hp_first, b.hp_low, b.hp_max, b.you_lo[0], b.you_hi[0], b.you_lo[1], b.you_hi[1], b.you_lo[2],
             b.you_hi[2]);
}

void summarize(Seen& s, const char* why) {
    if (!s.alive) return;
    s.alive = false;
    Burst& b = burst_slot(s.burst);
    if (b.id == s.burst) {
        b.on_you += s.player_hits;
        if (s.closest < b.closest) b.closest = s.closest;
        b.cap_last = limiter_requested_fps();
        if (--b.alive <= 0) burst_report(b);
    }
    if (g_lines >= kMaxLines) return;
    ++g_lines;
    LOG_INFO("[bullet] %p %d summary (%s): %.2f s, %d updates, %d hit queries, %d sweep contacts, on you %d, near %d, "
             "%d ground contacts, %d landings, first landing %s at update %d (%.2f s); lowest y %.3f, "
             "last (%.3f, %.3f, %.3f); closest to you %.2f m at %.2f s (you at %.2f, %.2f, %.2f); cap %d->%d; "
             "effect %08X root %08X ran its states %d times; burst %d%s",
             s.bullet, s.id, why, now_s() - s.born, s.updates, s.queries, s.contacts, s.player_hits, s.near_hits,
             s.ground, s.lands, s.first_land ? "" : "NONE", s.first_land, s.first_land_age, s.lowest, s.last[0],
             s.last[1], s.last[2], s.closest, s.closest_age, s.closest_you[0], s.closest_you[1], s.closest_you[2],
             s.cap_born, limiter_requested_fps(), s.fx_inst, s.fx_root, s.fx_calls, s.burst, s.detailed ? ", detailed" : "");
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
    const int cap = limiter_requested_fps();
    if (slot->born - g_burst_last > kBurstGap) {
        ++g_burst;
        g_burst_detailed = 0;
        Burst& old = burst_slot(g_burst);
        if (old.id) burst_report(old);  // 16 bursts ago and still not over: report what it has
        old = Burst{};
        old.id = g_burst;
        old.cap_first = cap;
        old.first = slot->born;
    }
    g_burst_last = slot->born;
    slot->burst = g_burst;
    slot->cap_born = cap;
    Burst& cur = burst_slot(g_burst);
    if (cur.id == g_burst) {
        ++cur.globs;
        ++cur.alive;
        cur.last_birth = slot->born;
        cur.cap_last = cap;
    }
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
    float me[3] = {};
    if (player_pos(me)) {
        const float dx = pos[0] - me[0], dz = pos[2] - me[2];
        float dy = 0;
        if (pos[1] < me[1]) {
            dy = me[1] - pos[1];
        } else if (pos[1] > me[1] + 1.7f) {
            dy = pos[1] - (me[1] + 1.7f);
        }
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < s->closest) {
            s->closest = d;
            s->closest_age = now_s() - s->born;
            std::memcpy(s->closest_you, me, 12);
        }
        Burst& b = burst_slot(s->burst);
        if (b.id == s->burst) {
            for (int k = 0; k < 3; ++k) {
                if (me[k] < b.you_lo[k]) b.you_lo[k] = me[k];
                if (me[k] > b.you_hi[k]) b.you_hi[k] = me[k];
            }
            const int hp = rd<int32_t>(player_chr(), 0x2D4, -1);
            if (hp >= 0) {
                if (b.hp_first < 0) {
                    b.hp_first = hp;
                    b.hp_max = rd<int32_t>(player_chr(), 0x2D8, 0);
                }
                if (b.hp_low < 0 || hp < b.hp_low) b.hp_low = hp;
            }
        }
    }
    if (!s->detailed || g_lines >= kMaxLines || s->update_lines >= kMaxUpdateLines) return;
    if (s->first_land && s->updates > s->first_land + kUpdatesAfterLanding) return;
    ++s->update_lines;
    ++g_lines;
    if (fresh) {
        const bool have_me = player_pos(me);
        ++g_lines;
        LOG_INFO("[bullet] %p new: bullet %d, burst %d, state %d; player at (%.2f, %.2f, %.2f)%s; frame %.4f s", b, id,
                 g_burst, state, me[0], me[1], me[2], have_me ? "" : " (unknown)", fixes_last_dt());
    }
    if (!s->fx_searched && s->fx_tries++ < 4) fx_search(*s, b, pos);
    if (!s->fx_inst) s->fx_inst = rd<uint32_t>(b, 0x60);
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

// 0x9E6B20 (the acceleration movement, pond2 type 55). The arrays at +0xB0..+0xC8 did not change when read on entry:
// 0xA4D420 writes them, and the entry hook was sampling the inputs. This snapshots them, the call stub reads them
// again after 0xA4D420 returns. A count of 0 never reaches that call (jbe 0x9E6C4C), so only a call that will happen
// is pushed. n == 1 is the drawn model (its numbers are local, not world). A world-position cluster is one whose
// +B4/+B8/+BC point sits within 8 m of a live watched bullet.
constexpr int kMaxShots = 32;
constexpr int kMaxTracks = 128;
constexpr int kMaxStepLines = 500;    // clusters that sit near a glob
constexpr int kMaxLocalLines = 200;   // n == 1 clusters that are not near a glob (the drawn models)
constexpr int kMaxFlatNear = 24;
constexpr int kMaxFlatLocal = 24;
constexpr int kMaxNearPerCluster = 48;   // about one line each 0.05 s, so a 2.3 s glob is covered at 120 FPS too
constexpr int kMaxLocalPerCluster = 8;
constexpr int kMaxPlayerLines = 150;  // per cap
constexpr float kPlayerNear = 2.5f;
constexpr float kParticleNear = 8.0f;
constexpr double kStepWindow = 4.0;
constexpr float kStepEps = 1e-5f;

struct Snap {
    float soa[7][4] = {};
    uint8_t soa_ok = 0;
    float ctx[24] = {};
    int ctx_n = 0;
    float c10[4] = {};
    float c70[4] = {};
    bool c10_ok = false;
    bool c70_ok = false;
};

struct Shot {
    const uint8_t* cluster = nullptr;
    const uint8_t* self = nullptr;
    uint32_t xmmbits = 0;
    uint32_t tbits = 0;
    int32_t count = 0;
    bool n1 = false;
    bool by_glob = false;  // not named near: windows.h defines that word as a macro
    bool tagged = false;   // logged because it was beside a glob earlier, not now
    int32_t near_id = -1;
    double at = 0;
    Snap before;
};

struct Track {
    const uint8_t* cluster = nullptr;
    int burst = -1;
    int lines = 0;
    bool flat = false;
    double logged_at = -100;
};

Shot g_shots[kMaxShots];
int g_shot_n = 0;
Track g_tracks[kMaxTracks];
int g_step_lines = 0;
int g_local_lines = 0;
int g_flat_near = 0;
int g_flat_local = 0;
int g_player_lines = 0;

// The integrator is about to read these itself, so a plain read is enough. VirtualQuery on every cluster would cost
// more at 120 FPS than at 30, which is the difference this trace is trying to measure.
void read_snap(const uint8_t* self, const uint8_t* cluster, Snap& s) {
    for (int k = 0; k < 7; ++k) {
        const uint8_t* a = rd<const uint8_t*>(cluster, 0xB0 + 4 * k);
        if (a && read(a, s.soa[k], 16)) s.soa_ok |= static_cast<uint8_t>(1u << k);
    }
    s.c10_ok = read(cluster + 0x10, s.c10, 16);
    s.c70_ok = read(cluster + 0x70, s.c70, 16);
    const uint8_t* ctx = rd<const uint8_t*>(self, 4);
    if (ctx && read(ctx, s.ctx, sizeof(s.ctx))) s.ctx_n = 24;
}

// The first watched bullet within `reach` of (x,y,z), or null. Squared distance, so the filter stays cheap.
const Seen* bullet_near(float x, float y, float z, float reach) {
    const float lim = reach * reach;
    for (const Seen& s : g_seen) {
        if (!s.alive) continue;
        const float dx = x - s.last[0], dy = y - s.last[1], dz = z - s.last[2];
        if (dx * dx + dy * dy + dz * dz < lim) return &s;
    }
    return nullptr;
}

bool soa_near_bullet(const Snap& s, int count, int32_t& id) {
    if ((s.soa_ok & 0x0E) != 0x0E) return false;  // +B4, +B8, +BC are the position components
    const int n = count < 4 ? count : 4;
    for (int i = 0; i < n; ++i) {
        if (const Seen* b = bullet_near(s.soa[1][i], s.soa[2][i], s.soa[3][i], kParticleNear)) {
            id = b->id;
            return true;
        }
    }
    return false;
}

bool ctx_near_bullet(const Snap& s, int32_t& id) {
    // Vec3s the movement actually passes in: context+0, +0x10, +0x18, +0x20, +0x28.
    static const int kAt[] = {0, 4, 6, 8, 10};
    for (int at : kAt) {
        if (at + 2 >= s.ctx_n) continue;
        if (const Seen* b = bullet_near(s.ctx[at], s.ctx[at + 1], s.ctx[at + 2], kParticleNear)) {
            id = b->id;
            return true;
        }
    }
    return false;
}

int appf(char* b, int n, int at, const char* fmt, ...) {
    if (at < 0 || at >= n - 1) return n - 1;
    va_list ap;
    va_start(ap, fmt);
    const int w = std::vsnprintf(b + at, static_cast<size_t>(n - at), fmt, ap);
    va_end(ap);
    if (w < 0) return at;
    at += w;
    return at > n - 1 ? n - 1 : at;
}

// 0xA4D420 integrates from the cluster's +0x10 to its +0x14 (its step is +0x14 - +0x10, the curves are read at
// both). Once a second: those steps against the game's frame time, for every acceleration cluster, so the next
// log says whether the effect clock follows the frame.
constexpr int kMaxClockLines = 600;
struct Clock {
    double since = 0;
    int calls = 0;
    int same = 0;  // step within 2 % of the frame
    float lo = 1e9f;
    float hi = -1e9f;
    double sum = 0;
    float t0 = 0;
    float t1 = 0;
    int lines = 0;
};
Clock g_clock;
volatile long g_fx_root_calls = 0;  // effect roots through the state machine (0x546550), all effects
// Effect instance creation (fn 0x971560) and its five ways to fail, each a jmp to its exit 0x972092: no free
// instance after a cleanup (0x9715F7), the resource gave nothing (0x971621), emit permission refused (0x9716F1), the
// game's adapter gave nothing (0x971814), building the instance failed (0x97189C).
volatile long g_fx_creates = 0;
volatile long g_fx_fail[5] = {};

void clock_note(const uint8_t* cluster, double now) {
    const float t0 = rd<float>(cluster, 0x10);
    const float t1 = rd<float>(cluster, 0x14);
    const float step = t1 - t0;
    const float frame = static_cast<float>(fixes_last_dt());
    Clock& c = g_clock;
    if (c.since == 0) c.since = now;
    ++c.calls;
    if (frame > 0 && std::fabs(step - frame) <= 0.02f * frame) ++c.same;
    if (step < c.lo) c.lo = step;
    if (step > c.hi) c.hi = step;
    c.sum += step;
    c.t0 = t0;
    c.t1 = t1;
    if (now - c.since < 1.0) return;
    if (c.lines < kMaxClockLines && g_lines < kMaxLines) {
        ++c.lines;
        ++g_lines;
        LOG_INFO("[bullet] fx clock: cap %d, frame %.4f, %d acceleration calls, step min %.4f avg %.4f max %.4f, "
                 "%d within 2%% of the frame; last cluster times %.4f -> %.4f; %ld effect roots through the states; "
                 "%ld effect creations, failed: pool %ld, resource %ld, permission %ld, adapter %ld, build %ld",
                 limiter_requested_fps(), frame, c.calls, c.lo, c.sum / c.calls, c.hi, c.same, c.t0, c.t1,
                 InterlockedExchange(&g_fx_root_calls, 0), InterlockedExchange(&g_fx_creates, 0),
                 InterlockedExchange(&g_fx_fail[0], 0), InterlockedExchange(&g_fx_fail[1], 0),
                 InterlockedExchange(&g_fx_fail[2], 0), InterlockedExchange(&g_fx_fail[3], 0),
                 InterlockedExchange(&g_fx_fail[4], 0));
    }
    const int lines = c.lines;
    c = Clock{};
    c.lines = lines;
    c.since = now;
}

// A cluster first seen within 8 m of a glob keeps being logged for 4 s even when its particles go far from the
// glob: at 120 FPS the clusters beside the globs at 30 FPS were not seen beside them at all.
struct Tag {
    const uint8_t* cluster = nullptr;
    int32_t id = -1;
    double at = 0;
};
Tag g_tags[256];

void tag_cluster(const uint8_t* cluster, int32_t id, double now) {
    Tag* slot = nullptr;
    for (Tag& t : g_tags) {
        if (t.cluster == cluster) {
            t.at = now;
            return;
        }
        if (!slot && (!t.cluster || now - t.at > 4.0)) slot = &t;
    }
    if (slot) *slot = Tag{cluster, id, now};
}

bool tagged(const uint8_t* cluster, double now, int32_t& id) {
    for (const Tag& t : g_tags) {
        if (t.cluster == cluster && now - t.at <= 4.0) {
            id = t.id;
            return true;
        }
    }
    return false;
}

void __stdcall on_accel(const uint8_t* self, const uint8_t* cluster, uint32_t tbits, uint32_t xmmbits) {
    if (!self || !cluster) return;
    const double now = now_s();
    if (rd<int32_t>(cluster, 0x18, 0) > 0) clock_note(cluster, now);
    if (now - g_burst_last > kStepWindow) return;
    while (g_shot_n > 0 && now - g_shots[0].at > 1.0) {
        for (int i = 1; i < g_shot_n; ++i) g_shots[i - 1] = g_shots[i];
        --g_shot_n;
    }
    if (g_shot_n >= kMaxShots) return;
    const int32_t count = rd<int32_t>(cluster, 0x18, 0);
    // Same test as the jbe at 0x9E6B72: (count + 3) >> 2 == 0 skips the integrator.
    if ((static_cast<uint32_t>(count) + 3u) >> 2 == 0) return;
    Shot& shot = g_shots[g_shot_n];
    shot = Shot{};
    shot.cluster = cluster;
    shot.self = self;
    shot.xmmbits = xmmbits;
    shot.tbits = tbits;
    shot.count = count;
    shot.at = now;
    read_snap(self, cluster, shot.before);
    shot.n1 = count == 1;
    shot.by_glob = soa_near_bullet(shot.before, count, shot.near_id) || ctx_near_bullet(shot.before, shot.near_id);
    if (shot.by_glob) {
        tag_cluster(cluster, shot.near_id, now);
    } else if (tagged(cluster, now, shot.near_id)) {
        shot.by_glob = true;
        shot.tagged = true;
    }
    if (!shot.n1 && !shot.by_glob) return;
    ++g_shot_n;
}

bool take_shot(const uint8_t* cluster, Shot& out) {
    int found = -1;
    for (int i = g_shot_n - 1; i >= 0; --i) {
        if (g_shots[i].cluster == cluster) {
            found = i;
            break;
        }
    }
    if (found < 0) return false;
    out = g_shots[found];
    for (int i = found; i < g_shot_n - 1; ++i) g_shots[i] = g_shots[i + 1];
    --g_shot_n;
    return true;
}

Track* track_for(const uint8_t* cluster) {
    Track* free_slot = nullptr;
    Track* other_burst = nullptr;
    for (Track& t : g_tracks) {
        if (t.cluster == cluster && t.burst == g_burst) return &t;
        if (!t.cluster && !free_slot) free_slot = &t;
        if (t.burst != g_burst) other_burst = &t;
    }
    Track* slot = free_slot ? free_slot : other_burst;
    if (!slot) return nullptr;
    *slot = Track{};
    slot->cluster = cluster;
    slot->burst = g_burst;
    return slot;
}

// Called from the stolen `call 0xA4D420`, after it returns and before `add esp, 0x54`.
void __stdcall on_accel_after(const uint8_t* cluster) {
    // Each cap gets its own step budget, so a 30 / 60 / 120 comparison is in one log.
    static int budget_cap = -1;
    const int cap = limiter_requested_fps();
    if (cap != budget_cap) {
        budget_cap = cap;
        g_step_lines = 0;
        g_local_lines = 0;
        g_flat_near = 0;
        g_flat_local = 0;
        for (Track& t : g_tracks) t = Track{};
        if (g_lines < kMaxLines) {
            ++g_lines;
            LOG_INFO("[bullet] step budgets reset for cap %d", cap);
        }
    }
    Shot shot;
    if (!take_shot(cluster, shot)) return;
    const double now = now_s();
    if (g_lines >= kMaxLines || now - g_burst_last > kStepWindow) return;
    Track* tr = track_for(cluster);
    if (!tr) return;
    const bool local_only = shot.n1 && !shot.by_glob;
    Snap after;
    read_snap(shot.self, cluster, after);
    char diff[900] = {};
    int al = 0;
    int shown = 0;
    int hidden = 0;
    static const char* const kSoa[] = {"B0", "B4", "B8", "BC", "C0", "C4", "C8"};
    for (int k = 0; k < 7; ++k) {
        const bool was_ok = (shot.before.soa_ok & (1u << k)) != 0;
        const bool now_ok = (after.soa_ok & (1u << k)) != 0;
        if (!was_ok || !now_ok) continue;
        const char* name = kSoa[k];
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(shot.before.soa[k][i] - after.soa[k][i]) <= kStepEps) continue;
            if (shown >= 14) {
                ++hidden;
                continue;
            }
            ++shown;
            al = appf(diff, sizeof(diff), al, " %s[%d] %.4f->%.4f", name, i, shot.before.soa[k][i], after.soa[k][i]);
        }
    }
    const int cn = shot.before.ctx_n < after.ctx_n ? shot.before.ctx_n : after.ctx_n;
    for (int i = 0; i < cn; ++i) {
        if (std::fabs(shot.before.ctx[i] - after.ctx[i]) <= kStepEps) continue;
        if (shown >= 14) {
            ++hidden;
            continue;
        }
        ++shown;
        al = appf(diff, sizeof(diff), al, " ctx+%X %.4f->%.4f", i * 4, shot.before.ctx[i], after.ctx[i]);
    }
    if (shot.before.c10_ok && after.c10_ok) {
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(shot.before.c10[i] - after.c10[i]) <= kStepEps) continue;
            if (shown >= 14) {
                ++hidden;
                continue;
            }
            ++shown;
            al = appf(diff, sizeof(diff), al, " c+10[%d] %.4f->%.4f", i, shot.before.c10[i], after.c10[i]);
        }
    }
    if (shot.before.c70_ok && after.c70_ok) {
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(shot.before.c70[i] - after.c70[i]) <= kStepEps) continue;
            if (shown >= 14) {
                ++hidden;
                continue;
            }
            ++shown;
            al = appf(diff, sizeof(diff), al, " c+70[%d] %.4f->%.4f", i, shot.before.c70[i], after.c70[i]);
        }
    }
    if (hidden) al = appf(diff, sizeof(diff), al, " +%d", hidden);
    float xmm = 0;
    std::memcpy(&xmm, &shot.xmmbits, 4);
    const char* why = shot.tagged ? (shot.n1 ? "n1 left" : "left")
                                  : shot.by_glob ? (shot.n1 ? "n1 near" : "near") : "n1";
    if (shown == 0) {
        if (tr->flat) return;
        if (local_only) {
            if (g_flat_local >= kMaxFlatLocal) return;
            ++g_flat_local;
        } else {
            if (g_flat_near >= kMaxFlatNear) return;
            ++g_flat_near;
        }
        tr->flat = true;
        ++g_lines;
        LOG_INFO("[bullet] step %p cluster %p n %d flat xmm %.5f (%08X) arg2 %08X frame %.4f t %.4f->%.4f ctx34 %.4f "
                 "cap %d %s glob %d",
                 shot.self, cluster, shot.count, xmm, shot.xmmbits, shot.tbits, fixes_last_dt(), shot.before.c10[0],
                 shot.before.c10[1], shot.before.ctx[13], cap, why, shot.near_id);
        return;
    }
    // One line per cluster per 0.05 s (0.10 s for a local model), so 120 FPS does not spend the whole cap in the
    // first fifth of a second. Each line is that call's own step, entry against the integrator's write.
    const int per_cluster = local_only ? kMaxLocalPerCluster : kMaxNearPerCluster;
    const double gap = local_only ? 0.10 : 0.05;
    if (tr->lines >= per_cluster || (tr->lines > 0 && now - tr->logged_at < gap)) return;
    if (local_only) {
        if (g_local_lines >= kMaxLocalLines) return;
        ++g_local_lines;
    } else if (g_step_lines >= kMaxStepLines) {
        return;
    } else {
        ++g_step_lines;
    }
    ++tr->lines;
    tr->logged_at = now;
    ++g_lines;
    LOG_INFO("[bullet] step %p cluster %p n %d xmm %.5f (%08X) arg2 %08X frame %.4f t %.4f->%.4f ctx34 %.4f cap %d %s "
             "glob %d |%s",
             shot.self, cluster, shot.count, xmm, shot.xmmbits, shot.tbits, fixes_last_dt(), shot.before.c10[0],
             shot.before.c10[1], shot.before.ctx[13], cap, why, shot.near_id, diff);
}

// 0xDF1E70, once a frame for each attack hit entry: the sweep of its shape from where it was to where it is.
void __stdcall on_query(const uint8_t* entry) {
    Seen* s = by_entry(entry);
    if (s) ++s->queries;
}

struct PlayerRefs {
    const uint8_t* chr = nullptr;
    const uint8_t* ctrl = nullptr;
    const uint8_t* phys = nullptr;
    const uint8_t* alt = nullptr;  // [WorldChrMan at 0x137D644 + 0x3C], the co-op player's ChrIns
    float pos[3] = {};
    bool have = false;
};

PlayerRefs player_refs() {
    PlayerRefs r;
    const uint8_t* wcm = rd<const uint8_t*>(reinterpret_cast<const void*>(kWorldChrMan), 0);
    const uint8_t* first = rd<const uint8_t*>(wcm, 4);
    r.chr = rd<const uint8_t*>(first, 0);
    r.ctrl = rd<const uint8_t*>(r.chr, 0x28);
    r.phys = rd<const uint8_t*>(r.ctrl, 0x1C);
    r.have = r.phys && read(r.phys + 0x10, r.pos, 12);
    const uint8_t* coop = rd<const uint8_t*>(reinterpret_cast<const void*>(kWorldChrManCoop), 0);
    r.alt = rd<const uint8_t*>(coop, 0x3C);
    return r;
}

bool same_player(const uint8_t* p, const PlayerRefs& r) {
    return p && (p == r.chr || p == r.ctrl || p == r.phys || p == r.alt);
}

// Runs for every watched bullet, before the 16-line contact cap. "on you" is a pointer match against the local
// player. "near you" is any non-floor contact within 2.5 m, so a wrong pointer chain still shows whether the
// touches stop at 120. The shape is the sweep's +0x100 block; +0xF8 is the timer it subtracts the frame from.
void note_player_touch(Seen& s, const uint8_t* entry, const float* point, const uint8_t* victim) {
    float p[3] = {};
    if (point) read(point, p, 12);
    const uint8_t* owner = rd<const uint8_t*>(victim, 0xC);
    const uint8_t* obj = rd<const uint8_t*>(owner, 0);
    const uint8_t* obj8 = rd<const uint8_t*>(obj, 8);
    const uint16_t kind = rd<uint16_t>(obj, 4, 0xFFFF);
    const PlayerRefs me = player_refs();
    const bool match = same_player(victim, me) || same_player(owner, me) || same_player(obj, me) || same_player(obj8, me);
    float dist = 0;
    bool beside = false;
    if (me.have) {
        const float dx = p[0] - me.pos[0], dy = p[1] - me.pos[1], dz = p[2] - me.pos[2];
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        beside = kind != 0 && dist < kPlayerNear;
    }
    if (match) ++s.player_hits;
    if (beside) ++s.near_hits;
    static int lines_cap = -1;
    if (limiter_requested_fps() != lines_cap) {
        lines_cap = limiter_requested_fps();
        g_player_lines = 0;
    }
    if ((!match && !beside) || g_player_lines >= kMaxPlayerLines || g_lines >= kMaxLines) return;
    ++g_player_lines;
    ++g_lines;
    float shape[12] = {};
    float epos[3] = {};
    float bpos[3] = {};
    float bvel[3] = {};
    read(entry + 0x100, shape, sizeof(shape));
    read(entry + 0x130, epos, 12);
    read(s.bullet + 0x10, bpos, 12);
    read(s.bullet + 0x30, bvel, 12);
    char sh[160] = {};
    int sl = 0;
    for (float v : shape) sl = appf(sh, sizeof(sh), sl, " %.3f", v);
    LOG_INFO("[bullet] %p %d %s at (%.3f, %.3f, %.3f) dist %.2f kind %u upd %d ground %.3f "
             "bullet (%.3f, %.3f, %.3f) vel (%.2f, %.2f, %.2f) you (%.3f, %.3f, %.3f) "
             "+F8 %.4f +13C %.4f shape%s entry (%.3f, %.3f, %.3f) "
             "victim %p owner %p obj %p obj8 %p chr %p ctrl %p phys %p alt %p",
             s.bullet, s.id, match ? "on you" : "near you", p[0], p[1], p[2], dist, kind, s.updates,
             rd<float>(s.bullet, 0x50), bpos[0], bpos[1], bpos[2], bvel[0], bvel[1], bvel[2], me.pos[0], me.pos[1],
             me.pos[2], rd<float>(entry, 0xF8), rd<float>(entry, 0x13C), sh, epos[0], epos[1], epos[2], victim, owner,
             obj, obj8, me.chr, me.ctrl, me.phys, me.alt);
}

void __stdcall on_contact(const uint8_t* entry, const float* point, const uint8_t* victim) {
    Seen* s = by_entry(entry);
    if (!s) return;
    ++s->contacts;
    note_player_touch(*s, entry, point, victim);
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

__declspec(naked) void accel_stub() {
    SAVE_ALL
    __asm {
        // [esp] is xmm0's low dword. After that push, arg2 is at 0xA4+8 and arg1 at 0xA8+4.
        mov edx, [esp]
        push edx
        mov edx, [esp + 0xA4 + 8]
        push edx
        mov edx, [esp + 0xA8 + 4]
        push edx
        push ecx
        call on_accel
    }
    RESTORE_ALL
    __asm {
        push ebx
        mov ebx, esp
        sub esp, 8
        push 09E6B26h
        ret
    }
}

// Replaces `call 0xA4D420` at 0x9E6C44. ebx is still the frame; [ebx+8] is the cluster. Then the original
// `add esp, 0x54` at 0x9E6C49 runs.
__declspec(naked) void accel_call_stub() {
    __asm {
        mov eax, 0A4D420h
        call eax
    }
    SAVE_ALL
    __asm {
        mov eax, [ebx + 8]
        push eax
        call on_accel_after
    }
    RESTORE_ALL
    __asm {
        push 09E6C49h
        ret
    }
}

// The effect state machine (the game's FX adapter, fn 0x546550, called once a frame for each effect root with
// (frame time, root, ...)). Each node of the root's tree ([root+0x34], children +0x8C, siblings +0x88, parent +0x84)
// has its type at [[node+0x90]+4], its state at +0x74 (a pointer into [[node+0x90]+0x58], 16 bytes a state), two
// timers at +0x78 / +0x7C, a flag at +0x80, and its timing block at [[node+0x60]+0x14]. The undead dragon's glob
// effect (16012) has a one-frame burst block {1/60, -1/60, -1}: roots with one are logged frame by frame, a few for
// each cap, to see why that effect's particles are missing above 30 FPS.
constexpr uint32_t kFxStates = 0x00546550;
constexpr uint8_t kFxStatesBytes[7] = {0x8B, 0x44, 0x24, 0x08, 0x83, 0xEC, 0x10};
constexpr int kFxRootsPerCap = 4;
constexpr int kFxCallsPerRoot = 110;
constexpr int kFxMaxNodes = 24;
constexpr int kMaxFxLines = 2400;

struct FxRoot {
    const uint8_t* root = nullptr;
    int calls = 0;   // every call
    int logged = 0;  // lines: every frame for the first 24, then one per 1/30 s of frames
};
FxRoot g_fx_roots[kFxRootsPerCap];
int g_fx_cap = -1;
int g_fx_taken = 0;
int g_fx_lines = 0;

template <typename F>
void fx_walk(const uint8_t* root, F&& each) {
    const uint8_t* node = rd<const uint8_t*>(root, 0x34);
    for (int guard = 0; node && guard < 256; ++guard) {
        each(node);
        const uint8_t* next = rd<const uint8_t*>(node, 0x8C);
        if (!next) next = rd<const uint8_t*>(node, 0x88);
        if (!next) {
            const uint8_t* up = rd<const uint8_t*>(node, 0x84);
            while (up && !rd<const uint8_t*>(up, 0x88)) up = rd<const uint8_t*>(up, 0x84);
            next = up ? rd<const uint8_t*>(up, 0x88) : nullptr;
        }
        node = next;
    }
}

const uint8_t* fx_block(const uint8_t* node) {
    const uint8_t* def = rd<const uint8_t*>(node, 0x60);
    return def ? rd<const uint8_t*>(def, 0x14) : nullptr;
}

bool fx_is_glob(const uint8_t* root) {
    bool found = false;
    fx_walk(root, [&](const uint8_t* node) {
        const uint8_t* b = fx_block(node);
        if (!b || found) return;
        const float life = rd<float>(b, 4), offset = rd<float>(b, 8);
        if (std::fabs(life - 1.0f / 60.0f) < 1e-4f && std::fabs(offset + 1.0f / 60.0f) < 1e-4f &&
            rd<int32_t>(b, 0xC, 0) == -1) {
            found = true;
        }
    });
    return found;
}

void __stdcall on_fxroot(const uint8_t* root, uint32_t dtbits) {
    if (!root) return;
    InterlockedIncrement(&g_fx_root_calls);
    // A live glob's own effect: matched once by its translation (+0x290) sitting on the glob and the one-frame
    // burst node, then counted on every call.
    const uint32_t r = reinterpret_cast<uint32_t>(root);
    bool glob = false;
    for (Seen& g : g_seen) {
        if (g.alive && g.fx_root == r) {
            ++g.fx_calls;
            glob = true;
        }
    }
    if (!glob) {
        const uint32_t first = rd<uint32_t>(root, 0x34);
        for (Seen& g : g_seen) {
            if (g.alive && !g.fx_root && g.fx_inst && g.fx_inst == first) {
                g.fx_root = r;
                ++g.fx_calls;
                glob = true;
                break;
            }
        }
    }
    if (g_fx_lines >= kMaxFxLines || g_lines >= kMaxLines) return;
    const int cap = limiter_requested_fps();
    if (cap != g_fx_cap) {
        g_fx_cap = cap;
        g_fx_taken = 0;
        for (FxRoot& r : g_fx_roots) r = FxRoot{};
    }
    FxRoot* slot = nullptr;
    for (FxRoot& r : g_fx_roots) {
        if (r.root == root) slot = &r;
    }
    if (!slot) {
        if (g_fx_taken >= kFxRootsPerCap || !(glob || fx_is_glob(root))) return;
        slot = &g_fx_roots[g_fx_taken++];
        slot->root = root;
        // The tree once: each node's type and the first six dwords of its block.
        char tree[1400] = {};
        int tl = 0, k = 0;
        fx_walk(root, [&](const uint8_t* node) {
            if (k++ >= kFxMaxNodes) return;
            const uint8_t* cls = rd<const uint8_t*>(node, 0x90);
            const uint8_t* b = fx_block(node);
            tl = appf(tree, sizeof(tree), tl, " | n%d %p type %X", k - 1, node, rd<uint32_t>(cls, 4));
            if (b) {
                tl = appf(tree, sizeof(tree), tl, " block %g %g %d %g %08X %08X", rd<float>(b, 4), rd<float>(b, 8),
                          rd<int32_t>(b, 0xC), rd<float>(b, 0x10), rd<uint32_t>(b, 0x14), rd<uint32_t>(b, 0x18));
            }
        });
        ++g_fx_lines;
        ++g_lines;
        LOG_INFO("[fx] glob effect root %p (cap %d):%s", root, cap, tree);
        // Each node's states ([[node+0x90]+0x58], 16 bytes: transitions, ?, count, actions) and their transitions
        // (8 bytes: target state, condition; the condition's vtable +4 tests it).
        int nk = 0;
        fx_walk(root, [&](const uint8_t* node) {
            if (nk++ >= 6 || g_fx_lines >= kMaxFxLines) return;
            const uint8_t* cls = rd<const uint8_t*>(node, 0x90);
            const uint32_t table = rd<uint32_t>(cls, 0x58);
            char st[1800] = {};
            int sl = 0;
            for (int i = 0; i < 6 && table; ++i) {
                const uint8_t* state = reinterpret_cast<const uint8_t*>(table + 16 * i);
                const uint32_t trans = rd<uint32_t>(state, 0), count = rd<uint32_t>(state, 8);
                const uint32_t acts = rd<uint32_t>(state, 4), nacts = rd<uint32_t>(state, 0xC);
                if (count > 16 || nacts > 64) break;
                sl = appf(st, sizeof(st), sl, " || s%d acts %08X x%u trans x%u", i, acts, nacts, count);
                // Actions are 0x1C bytes: an id and its arguments in place (0xA52660 -> 0xA560F0 -> 0x972830).
                for (uint32_t a = 0; a < nacts && a < 3; ++a) {
                    const uint8_t* act = reinterpret_cast<const uint8_t*>(acts + 0x1C * a);
                    sl = appf(st, sizeof(st), sl, " (act %X: %08X %08X %08X %08X %08X %08X)", rd<uint32_t>(act, 0),
                              rd<uint32_t>(act, 4), rd<uint32_t>(act, 8), rd<uint32_t>(act, 0xC),
                              rd<uint32_t>(act, 0x10), rd<uint32_t>(act, 0x14), rd<uint32_t>(act, 0x18));
                }
                for (uint32_t t = 0; t < count && t < 4; ++t) {
                    const uint8_t* tr = reinterpret_cast<const uint8_t*>(trans + 8 * t);
                    const uint32_t target = rd<uint32_t>(tr, 0);
                    const uint8_t* cond = rd<const uint8_t*>(tr, 4);
                    const uint32_t vt = rd<uint32_t>(cond, 0);
                    sl = appf(st, sizeof(st), sl, " [-> s%d cond %p vt %08X test %08X: %08X %08X %08X %08X %08X]",
                              (target && target >= table) ? static_cast<int>((target - table) / 16) : -1, cond, vt,
                              rd<uint32_t>(reinterpret_cast<const void*>(vt), 4), rd<uint32_t>(cond, 4),
                              rd<uint32_t>(cond, 8), rd<uint32_t>(cond, 0xC), rd<uint32_t>(cond, 0x10),
                              rd<uint32_t>(cond, 0x14));
                }
            }
            ++g_fx_lines;
            ++g_lines;
            LOG_INFO("[fx] root %p n%d %p type %X states:%s", root, nk - 1, node, rd<uint32_t>(cls, 4), st);
        });
    }
    ++slot->calls;
    // Every frame for the first 0.6 s (the glob's ground effect starts at about 0.4 s at 30 FPS), then one a 1/30 s.
    const int every = cap > 30 ? cap / 30 : 1;
    const int dense = cap > 0 ? (cap * 6) / 10 : 24;
    if (slot->logged >= kFxCallsPerRoot || (slot->calls > dense && slot->calls % every != 0)) return;
    ++slot->logged;
    float dt = 0;
    std::memcpy(&dt, &dtbits, 4);
    char line[1400] = {};
    int ll = 0, k = 0;
    fx_walk(root, [&](const uint8_t* node) {
        if (k++ >= kFxMaxNodes) return;
        const uint8_t* cls = rd<const uint8_t*>(node, 0x90);
        const uint32_t table = rd<uint32_t>(cls, 0x58);
        const uint32_t state = rd<uint32_t>(node, 0x74);
        const int idx = (table && state >= table) ? static_cast<int>((state - table) / 16) : -1;
        ll = appf(line, sizeof(line), ll, " | n%d s%d %.4f %.4f %u", k - 1, idx, rd<float>(node, 0x78),
                  rd<float>(node, 0x7C), rd<uint8_t>(node, 0x80));
    });
    ++g_fx_lines;
    ++g_lines;
    LOG_INFO("[fx] root %p call %d dt %.4f frame %.4f cap %d%s", root, slot->calls, dt, fixes_last_dt(), cap, line);
}

__declspec(naked) void fxstates_stub() {
    SAVE_ALL
    __asm {
        mov ecx, [esp + 0xA0 + 4]
        push ecx
        mov ecx, [esp + 0xA4 + 8]
        push ecx
        call on_fxroot
    }
    RESTORE_ALL
    __asm {
        mov eax, dword ptr [esp + 8]
        sub esp, 10h
        push 0546557h
        ret
    }
}

constexpr uint32_t kFxCreate = 0x00971560;  // push ebx ; mov ebx,esp ; sub esp,8
constexpr uint32_t kFxCreateExit = 0x00972092;
constexpr uint32_t kFxFailSites[5] = {0x009715F7, 0x00971621, 0x009716F1, 0x00971814, 0x0097189C};

// The glob that owns an effect node: climb the parents (+0x84) to the top node, which is the bullet's instance.
Seen* fx_owner(const uint8_t* node) {
    if (!node) return nullptr;
    const uint8_t* top = node;
    for (int i = 0; i < 12; ++i) {
        const uint8_t* up = rd<const uint8_t*>(top, 0x84);
        if (!up) break;
        top = up;
    }
    const uint32_t t = reinterpret_cast<uint32_t>(top);
    for (Seen& g : g_seen) {
        if (g.alive && g.fx_inst && g.fx_inst == t) return &g;
    }
    return nullptr;
}

// Lines for the first few globs a cap: every action their effect nodes run and every effect creation one of their
// nodes starts.
constexpr int kFxActGlobsPerCap = 3;
constexpr int kMaxFxActLines = 900;
const Seen* g_fx_act_globs[kFxActGlobsPerCap] = {};
int g_fx_act_cap = -1;
int g_fx_act_n = 0;
int g_fx_act_lines = 0;

bool fx_act_wanted(const Seen* g) {
    if (!g || g_fx_act_lines >= kMaxFxActLines || g_lines >= kMaxLines) return false;
    const int cap = limiter_requested_fps();
    if (cap != g_fx_act_cap) {
        g_fx_act_cap = cap;
        g_fx_act_n = 0;
        for (const Seen*& x : g_fx_act_globs) x = nullptr;
    }
    for (const Seen* x : g_fx_act_globs) {
        if (x == g) return true;
    }
    if (g_fx_act_n >= kFxActGlobsPerCap) return false;
    g_fx_act_globs[g_fx_act_n++] = g;
    return true;
}

void fx_node_text(const uint8_t* node, char* out, int size) {
    const uint8_t* cls = rd<const uint8_t*>(node, 0x90);
    const uint32_t table = rd<uint32_t>(cls, 0x58);
    const uint32_t state = rd<uint32_t>(node, 0x74);
    const int idx = (table && state >= table) ? static_cast<int>((state - table) / 16) : -1;
    std::snprintf(out, size, "%p type %X s%d %.4f %.4f", node, rd<uint32_t>(cls, 4), idx, rd<float>(node, 0x78),
                  rd<float>(node, 0x7C));
}

int32_t aid_now(const int32_t* id) {
    return rd<int32_t>(id, 0, -1);
}

// 0xB80F70: the basic action handler runs action *id for a node with its arguments (table 0x12DDC60).
void __stdcall on_fxaction(const int32_t* id, const uint8_t* ctx, const uint8_t* args) {
    Seen* g = fx_owner(ctx);
    if (!fx_act_wanted(g)) return;
    char nt[96] = {};
    fx_node_text(ctx, nt, sizeof(nt));
    ++g_fx_act_lines;
    ++g_lines;
    LOG_INFO("[fx] action %X glob %p age %.4f cap %d frame %.4f node %s args %08X %08X %08X %08X %08X %08X",
             rd<int32_t>(id, 0, -1), g->bullet, now_s() - g->born, limiter_requested_fps(), fixes_last_dt(), nt,
             rd<uint32_t>(args, 0), rd<uint32_t>(args, 4), rd<uint32_t>(args, 8), rd<uint32_t>(args, 0xC),
             rd<uint32_t>(args, 0x10), rd<uint32_t>(args, 0x14));
    // Action 0x62 goes on only when the node's own parameter 9 ([[node+0x60]][9], group 1 of 0x97A880) evaluates
    // non-zero: the node definition's parameters 0-12, and parameter 9 evaluated as the game does (vtable +0x28).
    if (aid_now(id) == 0x62 && g_lines < kMaxLines) {
        const uint8_t* def = rd<const uint8_t*>(ctx, 0x60);
        const uint8_t* plist = rd<const uint8_t*>(def, 0);
        for (int i = 0; i <= 12 && g_lines < kMaxLines; ++i) {
            const uint8_t* prm = rd<const uint8_t*>(plist, 4 * i);
            if (!prm) continue;
            const uint32_t vt = rd<uint32_t>(prm, 0);
            char data[600] = {};
            int dl = 0;
            int ptrs = 0;
            for (int k = 1; k <= 8; ++k) {
                const uint32_t v = rd<uint32_t>(prm, 4 * k);
                float f = 0;
                std::memcpy(&f, &v, 4);
                dl = appf(data, sizeof(data), dl, " %08X(%g)", v, f);
            }
            for (int k = 1; k <= 8 && ptrs < 2; ++k) {
                const uint32_t v = rd<uint32_t>(prm, 4 * k);
                if (!heap_pointer(v) || v == reinterpret_cast<uint32_t>(prm)) continue;
                ++ptrs;
                dl = appf(data, sizeof(data), dl, " |+%X:", 4 * k);
                for (int j = 0; j < 10; ++j) {
                    const uint32_t w = rd<uint32_t>(reinterpret_cast<const void*>(v), 4 * j);
                    float f = 0;
                    std::memcpy(&f, &w, 4);
                    dl = appf(data, sizeof(data), dl, " %g", f);
                }
            }
            int value = 0x7FFFFFFF;
            if (i == 9 && vt) {
                using Eval = int(__thiscall*)(const void*, const void*);
                const Eval eval = reinterpret_cast<Eval>(rd<uint32_t>(reinterpret_cast<const void*>(vt), 0x28));
                __try {
                    value = eval(prm, ctx);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    value = 0x7FFFFFFE;
                }
            }
            ++g_lines;
            LOG_INFO("[fx] node %p cap %d def param %d: %p vt %08X eval %08X%s%s", ctx, limiter_requested_fps(), i,
                     prm, vt, rd<uint32_t>(reinterpret_cast<const void*>(vt), 0x28), data,
                     i == 9 ? "" : "");
            if (i == 9) {
                ++g_lines;
                LOG_INFO("[fx] node %p cap %d: parameter 9 evaluates to %d (0x%08X) with the node at %.4f / %.4f", ctx,
                         limiter_requested_fps(), value, value, rd<float>(ctx, 0x78), rd<float>(ctx, 0x7C));
            }
        }
    }
    // The parameters of the actions that make the ground part: args[0] lists them (0x5471C0), each one's vtable
    // +0x28 evaluates it for the node. Once per action id and cap.
    const int32_t aid = rd<int32_t>(id, 0, -1);
    static int dumped_cap[3] = {-1, -1, -1};
    const int slot = aid == 0x62 ? 0 : aid == 0x10 ? 1 : aid == 0x15 ? 2 : -1;
    if (slot < 0 || dumped_cap[slot] == limiter_requested_fps()) return;
    dumped_cap[slot] = limiter_requested_fps();
    const uint8_t* list = rd<const uint8_t*>(args, 0);
    const uint32_t count = rd<uint32_t>(args, 4);
    for (uint32_t i = 0; i < count && i < 4 && g_lines < kMaxLines; ++i) {
        const uint8_t* prm = rd<const uint8_t*>(list, 4 * i);
        const uint32_t vt = rd<uint32_t>(prm, 0);
        char data[400] = {};
        int dl = 0;
        for (int k = 1; k <= 10; ++k) {
            const uint32_t v = rd<uint32_t>(prm, 4 * k);
            float f = 0;
            std::memcpy(&f, &v, 4);
            dl = appf(data, sizeof(data), dl, " %08X(%g)", v, f);
        }
        // a pointer field often holds the keys: the first eight dwords behind field 1 and 2
        const uint8_t* k1 = rd<const uint8_t*>(prm, 4);
        const uint8_t* k2 = rd<const uint8_t*>(prm, 8);
        const uint8_t* const keys[2] = {k1, k2};
        for (const uint8_t* kp : keys) {
            if (!kp || !heap_pointer(reinterpret_cast<uint32_t>(kp))) continue;
            dl = appf(data, sizeof(data), dl, " |");
            for (int k = 0; k < 8; ++k) {
                const uint32_t v = rd<uint32_t>(kp, 4 * k);
                float f = 0;
                std::memcpy(&f, &v, 4);
                dl = appf(data, sizeof(data), dl, " %g", f);
            }
        }
        ++g_lines;
        LOG_INFO("[fx] action %X param %u: %p vt %08X eval %08X (+2C %08X):%s", aid, i, prm, vt,
                 rd<uint32_t>(reinterpret_cast<const void*>(vt), 0x28),
                 rd<uint32_t>(reinterpret_cast<const void*>(vt), 0x2C), data);
    }
}

// 0x971560 entry: an effect creation; logged when one of its first four arguments is a node of a followed glob.
void __stdcall on_fxcreate(const uint32_t* stack) {
    const uint32_t ret = rd<uint32_t>(stack, 0);
    for (int i = 1; i <= 4; ++i) {
        const uint8_t* arg = reinterpret_cast<const uint8_t*>(rd<uint32_t>(stack, 4 * i));
        if (!arg || !heap_pointer(reinterpret_cast<uint32_t>(arg))) continue;
        Seen* g = fx_owner(arg);
        if (!g || !fx_act_wanted(g)) continue;
        char nt[96] = {};
        fx_node_text(arg, nt, sizeof(nt));
        ++g_fx_act_lines;
        ++g_lines;
        LOG_INFO("[fx] create from %08X glob %p age %.4f cap %d (arg %d node %s) args %08X %08X %08X %08X %08X %08X",
                 ret, g->bullet, now_s() - g->born, limiter_requested_fps(), i, nt, rd<uint32_t>(stack, 4),
                 rd<uint32_t>(stack, 8), rd<uint32_t>(stack, 12), rd<uint32_t>(stack, 16), rd<uint32_t>(stack, 20),
                 rd<uint32_t>(stack, 24));
        return;
    }
}

__declspec(naked) void fxcreate_stub() {
    __asm {
        lock inc dword ptr [g_fx_creates]
        pushad
        lea eax, [esp + 20h]
        push eax
        call on_fxcreate
        popad
        push ebx
        mov ebx, esp
        sub esp, 8
        push 0971566h
        ret
    }
}

// thiscall (ecx = handler), [esp+4] id*, [esp+8] node, [esp+0Ch] args; push ebp ; mov ebp,esp ; sub esp,8
__declspec(naked) void fxaction_stub() {
    SAVE_ALL
    __asm {
        mov eax, [esp + 0xA0 + 0Ch]
        push eax
        mov eax, [esp + 0xA4 + 8]
        push eax
        mov eax, [esp + 0xA8 + 4]
        push eax
        call on_fxaction
    }
    RESTORE_ALL
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 8
        push 0B80F76h
        ret
    }
}

__declspec(naked) void fxfail0_stub() {
    __asm {
        lock inc dword ptr [g_fx_fail]
        push 0972092h
        ret
    }
}
__declspec(naked) void fxfail1_stub() {
    __asm {
        lock inc dword ptr [g_fx_fail + 4]
        push 0972092h
        ret
    }
}
__declspec(naked) void fxfail2_stub() {
    __asm {
        lock inc dword ptr [g_fx_fail + 8]
        push 0972092h
        ret
    }
}
__declspec(naked) void fxfail3_stub() {
    __asm {
        lock inc dword ptr [g_fx_fail + 12]
        push 0972092h
        ret
    }
}
__declspec(naked) void fxfail4_stub() {
    __asm {
        lock inc dword ptr [g_fx_fail + 16]
        push 0972092h
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

bool hook_bytes(uint32_t site, const uint8_t* expect, int n, const void* stub) {
    if (n < 5 || std::memcmp(reinterpret_cast<const void*>(site), expect, n) != 0) {
        LOG_ERROR("BulletTrace: %08X does not match this build. Not hooked.", site);
        return false;
    }
    DWORD old = 0;
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(stub) - (site + 5));
    p[0] = 0xE9;
    std::memcpy(p + 1, &rel, 4);
    for (int i = 5; i < n; ++i) p[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, n);
    DWORD ignored = 0;
    VirtualProtect(p, n, old, &ignored);
    return true;
}

bool hook(uint32_t site, const uint8_t* expect, const void* stub) {
    return hook_bytes(site, expect, 6, stub);
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
    ok += hook(kAccel, kAccelPrologue, &accel_stub);
    ok += hook_bytes(kAccelCall, kAccelCallBytes, 5, &accel_call_stub);
    ok += hook(kContact, kContactPrologue, &contact_stub);
    ok += hook_bytes(kFxStates, kFxStatesBytes, 7, &fxstates_stub);
    {
        static const uint8_t kCreateBytes[6] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};
        static void (*const kFailStubs[5])() = {fxfail0_stub, fxfail1_stub, fxfail2_stub, fxfail3_stub, fxfail4_stub};
        int fx_ok = hook_bytes(kFxCreate, kCreateBytes, 6, &fxcreate_stub) ? 1 : 0;
        for (int i = 0; i < 5; ++i) {
            // Each site is a jmp rel32 to the exit; check that before taking it over.
            uint8_t want[5] = {0xE9};
            const int32_t rel = static_cast<int32_t>(kFxCreateExit - (kFxFailSites[i] + 5));
            std::memcpy(want + 1, &rel, 4);
            fx_ok += hook_bytes(kFxFailSites[i], want, 5, reinterpret_cast<const void*>(kFailStubs[i])) ? 1 : 0;
        }
        static const uint8_t kActionBytes[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};
        fx_ok += hook_bytes(0x00B80F70, kActionBytes, 6, &fxaction_stub) ? 1 : 0;
        LOG_INFO("BulletTrace: effect creation counters and the action handler, %d of 7 sites", fx_ok);
    }
    LOG_INFO("BulletTrace: %d of 11 hooks; the undead dragon's acid (34200-34209), c5390's 53900-53902 and the Gaping "
             "Dragon's 52600/52601 are logged with their map contacts ([bullet] lines: the first %d of each burst frame "
             "by frame, a summary for every one with its closest approach to you, a line per burst with the cap and the HP "
             "you lost, the particle step after the integrator with the cluster's times, an fx clock line each second, "
             "a line when a sweep touches you, [fx] lines for a few glob effects a cap; step and touch budgets start again at "
             "every cap change)",
             ok, kDetailedPerBurst);
    return ok == 11;
}

#else  // A release build leaves this diagnostic out (see buildcfg.h).

bool bullettrace_install() { return false; }

#endif  // FPSU_DIAGNOSTICS
