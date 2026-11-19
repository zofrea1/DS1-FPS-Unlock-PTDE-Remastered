#include "drawdist.h"

#include "drawdist_twins.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

// DrawMapPieces. A map piece is drawn when one of its draw groups (MSB part +0x38, 128 bits) meets its block's masks,
// which the game builds from the collision the player stands on (its MSB part's +0x48, kept in the collision's record
// at +0x20 and read by 0x4569D0: ecx = record + 8, eax = out). Each collision is one room: it displays its own group,
// and its draw groups (record +0x10) list the rooms it is shown from, its neighbours. Firelink's ledge grass
// (m5060B2..m5064B2, groups 0-2) showed only from inside the shrine, not from the overlook above it (h0027B2: displays
// group 27, neighbours 0, 1, 2, 11 and 28; 2026-11-18, tested by giving that collision groups 0-2: the grass came in a
// few seconds later). Every block has two masks, and the same two go to every world manager by block index:
//  - the draw mask (+0x0C), rebuilt when the floor or the map changes (0xDE2C60, from 0xDE85D0 at 0xDE8615: the
//    floor's groups at 0xDE2C9F, the linked blocks' from the map's connections), written to the map pieces
//    (WorldMapManImp [0x137D5F4]), characters, objects, collisions, map effects, sounds and point lights;
//  - the streaming mask (+0x1C), rebuilt every frame (0xDE7C30: the floors at FieldArea+0x3C and +0x44..+0x60, each
//    through 0xDE2AE0 at 0xDE807C / 0xDE8096, the floor's groups at 0xDE2AFA), which decides what is in the scene at
//    all: a map piece only while its groups meet it (0xE9CB80), a character is asked for (loaded and made active,
//    [chr+0xC]+0xC = 2, 0xE3E870 at 0xDE81F0) only while its floor's groups meet it, and likewise objects and the
//    collisions in the physics world.
// The two options widen what the player's floor adds to its block's two masks, for the map pieces alone: the floor's
// contribution is noted as the game reads it (0xDE2C9F, 0xDE2AFA) and ORed into the map piece manager's masks after
// the game has written them (after 0xDE2C60 returns; before 0xE9BD70 at 0xDE8198, right after the streaming masks are
// written). Characters, objects, collisions, effects, sounds and lights keep the game's own masks.
//  adjacent: the floor's display groups and its draw groups: the pieces seen from this room and from the rooms next to
//            it (the ledge grass from the overlook). Rooms farther away stay culled.
//  all:      every group: every piece of the loaded maps wherever the player stands (the view's own culling still
//            applies). Measured 2026-11-18 at 4K: Demon Ruins +3.5 ms game and +4.2 ms render thread a frame (about
//            300 -> 134 FPS: it draws Lost Izalith behind the walls, same map file), Blighttown +2.4 / +0.6 ms,
//            Firelink +0.5 / +0.6 ms (before 2.7.1, with every other manager's masks widened as well).
// Up to 2.7.0 the four reads themselves answered the widened groups, so every manager got them: in the whole block
// (all) or the next rooms (adjacent) every character was asked for and active, and with it the map's events that wait
// for one to be loaded. The Undead Burg's 11015100 places Solaire's summon sign at the Bell Gargoyles once his summon
// (6540, beside their fog) is loaded and the player is human, and disables his Undead Burg self (6001): with all it ran
// at the Undead Burg bonfire and Solaire was gone from his place past the Taurus Demon (user report 2026-11-19;
// reproduced: flag 11015100 on and 6001 disabled within a second of being human there; not with game). An infinite
// draw distance mod for Remastered was reported to do the same.
// LodDistance. 0xF61CD0 (ecx = the model, esi = its LodBank row, al = its current level, xmm1 = its distance from the
// camera) picks level 0, 1 or 2 from the row's lv01/lv12 border distances and margins (default row 0: 15/30 m in
// Prepare to Die Edition, 25/50 m in Remastered). The distance is scaled by 100 / LodDistance on entry, so every
// border moves out by LodDistance percent.
//
// Stand-ins. In five maps some surfaces exist twice: a detailed piece drawn from the rooms that look at it up close
// and a cheaper stand-in of the same surfaces drawn from the rooms that see it from afar (Anor Londo's m2200B1 and its
// stand-in m8010B1; m8000B1 stands in for ten of the cathedral's pieces). The game never draws both, as no room lists
// both, but adjacent (or all) adds a neighbouring room's pieces, so the two were drawn in the same place and fought
// over the depth (z-fighting: the terraces flickered, Anor Londo 2026-11-19). The pairs (drawdist_twins.h, found from
// the maps' own data: pieces with no draw group in common whose triangles share planes and overlap, at least 0.25 m^2)
// are checked once a frame: a piece that would be drawn only because of a neighbouring room is hidden while its twin
// is drawn by the player's own room, and of two twins both drawn only because of neighbours the one with fewer
// vertices stays. Hidden means its draw groups cleared in the map piece manager ([0x137D5F4]: +0x1C the blocks' count,
// +0x20 the blocks, 0x5C each, one per map in a fixed order; a block's +0x2C its pieces' count, +0x30 its pieces,
// 0x50 each: +0x04 the id (block << 24 | index), +0x08 the draw groups the frame's culling tests), put back as soon as
// that no longer holds. Nothing the game itself draws from the player's room is ever hidden.
//
// EffectLodDistance. The fires' level of detail (torches, bonfires, braziers: the 181 effects built on template
// effect 2117, and 2116 and 2118 for 70 more) moves between states by level checks: CheckLevel (data3 type 138,
// 0xA33600) is `int(distance from the camera) < slot n`, n = 5, 6, 7, 8 (the value of the effect's parameter slot n,
// [context+0x60][n], read through 0x97A880 kind 1), ResetLevelSingle (139) and ResetLevel (140) likewise, and the
// nearest state alone has the embers. The distance is scaled by 100 / EffectLodDistance before it is made an
// integer, so every boundary moves out by that percent. (Scaling n instead, in the effect files, pointed the checks
// at slots the effects do not have: the game crashed at load with n = 16, 2026-11-18.)
namespace {

constexpr uint32_t kGetDisp = 0x004569D0;
constexpr uint32_t kDrawGetSite = 0x00DE2C9F;    // call 0x4569D0 in 0xDE2C60 (the draw masks): the floor's groups
constexpr uint32_t kStreamGetSite = 0x00DE2AFA;  // call 0x4569D0 in 0xDE2AE0 (a floor's streaming groups)
constexpr uint32_t kBuildDraw = 0x00DE2C60;      // stdcall(FieldArea), eax = the floor: every block's draw mask
constexpr uint32_t kBuildDrawSite = 0x00DE8615;  // its call in FieldArea's update 0xDE85D0
constexpr uint32_t kStreamFloor = 0x00DE2AE0;    // stdcall(FieldArea, masks), eax = a floor: adds its groups
constexpr uint32_t kStreamFloorSites[] = {0x00DE807C, 0x00DE8096};  // its calls in the streaming update 0xDE7C30
constexpr uint32_t kStreamDone = 0x00E9BD70;     // called by 0xDE7C30 right after it wrote the streaming masks
constexpr uint32_t kStreamDoneSite = 0x00DE8198;
constexpr uint32_t kPieceManVar = 0x0137D5F4;    // WorldMapManImp, the map pieces' manager
constexpr uint32_t kMapBlockSize = 0x5C;         // its +0x20: one per block (count +0x1C)
constexpr uint32_t kMapBlockDraw = 0x0C;         // the block's draw mask (16 bytes)
constexpr uint32_t kMapBlockStream = 0x1C;       // the block's streaming mask
constexpr int kMaxBlocks = 32;
constexpr uint32_t kLodFn = 0x00F61CD0;
// EffectLodDistance: the effect level checks' call to _ftol on the camera distance (CheckLevel 0xA33600,
// ResetLevelSingle 0xA33720, ResetLevel 0xA33890, DistanceFromCamera 0xA48A00).
constexpr uint32_t kFtol = 0x00ABF1A0;
constexpr uint32_t kFxDistSites[] = {0x00A33700, 0x00A33820, 0x00A339C2, 0x00A48AAA};
constexpr uint8_t kLodHead[9] = {0x51, 0xF3, 0x0F, 0x10, 0x81, 0x50, 0x02, 0x00, 0x00};  // push ecx; movss xmm0,[ecx+250h]

volatile LONG g_mode = 0;  // DrawMapPieces: 0 game, 1 adjacent, 2 all
uint32_t g_get = kGetDisp;
uint32_t g_build_draw = kBuildDraw;
uint32_t g_stream_floor = kStreamFloor;
uint32_t g_stream_done = kStreamDone;
float g_lod_inv = 1.0f;
float g_fx_inv = 1.0f;
uint32_t g_ftol = kFtol;
uint32_t g_lod_back = kLodFn + 9;
bool g_lod_on = false;

// What the floors add for the map pieces alone, per block, until it is ORed into the map piece manager's masks (the
// game thread: FieldArea's update and the streaming update run there, one after the other).
uint32_t g_extra_draw[kMaxBlocks][4] = {};
uint32_t g_extra_stream[kMaxBlocks][4] = {};
bool g_any_draw = false;
bool g_any_stream = false;
volatile LONG g_in_stream = 0;  // inside the streaming update's floor calls (0xDE2AE0 has other callers: load checks)

// A floor's groups as the game reads them (rec8 = the floor's record + 8, the ecx of 0x4569D0): its block (record
// +0x0F) gets its draw groups (record +0x10, the rooms next to it) with adjacent, or every group with all.
void __cdecl note_floor(uint32_t rec8, int stream) {
    __try {
        const int block = *reinterpret_cast<const int8_t*>(rec8 + 7);
        if (block < 0 || block >= kMaxBlocks) return;
        uint32_t* e = stream ? g_extra_stream[block] : g_extra_draw[block];
        const auto* nbr = reinterpret_cast<const uint32_t*>(rec8 + 8);
        for (int k = 0; k < 4; ++k) e[k] |= g_mode == 2 ? 0xFFFFFFFFu : nbr[k];
        (stream ? g_any_stream : g_any_draw) = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The noted groups ORed into every block's mask at `off` in the map piece manager, then forgotten.
void apply_extra(uint32_t (*extra)[4], bool* any, uint32_t off) {
    if (!*any) return;
    *any = false;
    __try {
        const uint32_t man = *reinterpret_cast<const uint32_t*>(kPieceManVar);
        const uint32_t blocks = man ? *reinterpret_cast<const uint32_t*>(man + 0x20) : 0;
        const int count = man ? *reinterpret_cast<const int*>(man + 0x1C) : 0;
        for (int b = 0; blocks && b < count && b < kMaxBlocks; ++b) {
            auto* mask = reinterpret_cast<uint32_t*>(blocks + b * kMapBlockSize + off);
            for (int k = 0; k < 4; ++k) mask[k] |= extra[b][k];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    std::memset(extra, 0, sizeof(uint32_t) * 4 * kMaxBlocks);
}

void __cdecl apply_draw() { apply_extra(g_extra_draw, &g_any_draw, kMapBlockDraw); }
void __cdecl apply_stream() { apply_extra(g_extra_stream, &g_any_stream, kMapBlockStream); }

// In place of `call 0x4569D0` in 0xDE2C60 (the draw masks): the game's answer, the floor noted.
__declspec(naked) void get_disp_draw() {
    __asm {
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        pushad
        push 0
        push ecx
        call note_floor
        add esp, 8
        popad
    done:
        ret
    }
}

// In place of `call 0x4569D0` in 0xDE2AE0 (a floor's streaming groups): noted when the streaming update asked.
__declspec(naked) void get_disp_stream() {
    __asm {
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        cmp g_in_stream, 0
        je done
        pushad
        push 1
        push ecx
        call note_floor
        add esp, 8
        popad
    done:
        ret
    }
}

// In place of `call 0xDE2C60` (stdcall, one argument, eax = the floor): the game's draw masks, then ours for the
// map pieces.
__declspec(naked) void build_draw() {
    __asm {
        push dword ptr [esp + 4]
        call dword ptr [g_build_draw]
        pushad
        call apply_draw
        popad
        ret 4
    }
}

// In place of `call 0xDE2AE0` in the streaming update (stdcall, two arguments, eax = the floor).
__declspec(naked) void stream_floor() {
    __asm {
        mov dword ptr [g_in_stream], 1
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        call dword ptr [g_stream_floor]
        mov dword ptr [g_in_stream], 0
        ret 8
    }
}

// In place of `call 0xE9BD70` right after the streaming masks are written: ours for the map pieces, then the call.
__declspec(naked) void stream_done() {
    __asm {
        pushad
        call apply_stream
        popad
        jmp dword ptr [g_stream_done]
    }
}

// In place of `call _ftol` (st0 = the camera distance in metres): the distance scaled, then _ftol, which returns to
// the level check.
__declspec(naked) void fx_ftol() {
    __asm {
        fmul dword ptr [g_fx_inv]
        jmp dword ptr [g_ftol]
    }
}

__declspec(naked) void lod_entry() {
    __asm {
        mulss xmm1, g_lod_inv
        push ecx
        movss xmm0, dword ptr [ecx + 0x250]
        jmp dword ptr [g_lod_back]
    }
}

bool write_bytes(uint32_t at, const void* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, n);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

// A call rel32 at `site` to `from` made to call `to`.
bool redirect_call(uint32_t site, uint32_t from, const void* to) {
    const auto* p = reinterpret_cast<const uint8_t*>(site);
    int32_t rel = 0;
    std::memcpy(&rel, p + 1, 4);
    if (p[0] != 0xE8 || site + 5 + rel != from) return false;
    uint8_t code[5] = {0xE8};
    const int32_t nrel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (site + 5));
    std::memcpy(code + 1, &nrel, 4);
    return write_bytes(site, code, 5);
}

// A call rel32 at `site` that calls `from`.
bool calls(uint32_t site, uint32_t from) {
    const auto* p = reinterpret_cast<const uint8_t*>(site);
    int32_t rel = 0;
    std::memcpy(&rel, p + 1, 4);
    return p[0] == 0xE8 && site + 5 + rel == from;
}

// --- stand-ins (see the top) -----------------------------------------------------------------------------------
constexpr uint32_t kPieceMan = kPieceManVar;
constexpr uint32_t kWorldChrMan = 0x0137D644;  // +0x3C the player, whose +0x29C is the floor record underfoot
constexpr size_t kTwinPieces = sizeof(drawdist_twins::kPieces) / sizeof(drawdist_twins::kPieces[0]);
constexpr size_t kTwinPairs = sizeof(drawdist_twins::kPairs) / sizeof(drawdist_twins::kPairs[0]);
bool g_hidden[kTwinPieces] = {};
volatile LONG g_hidden_count = 0;
volatile LONG g_hides = 0;  // pieces hidden so far (for the log)

// The live draw groups of a table piece, or null when its map is not loaded or does not match the table.
uint32_t* live_groups(const drawdist_twins::Piece& p) {
    const uint32_t man = *reinterpret_cast<const uint32_t*>(kPieceMan);
    if (!man) return nullptr;
    const uint32_t blocks = *reinterpret_cast<const uint32_t*>(man + 0x20);
    const int count = *reinterpret_cast<const int*>(man + 0x1C);
    if (!blocks || p.block >= count) return nullptr;
    const uint32_t block = blocks + p.block * 0x5C;
    const int pieces = *reinterpret_cast<const int*>(block + 0x2C);
    const uint32_t arr = *reinterpret_cast<const uint32_t*>(block + 0x30);
    if (!arr || pieces != p.map_pieces || p.index >= pieces) return nullptr;
    const uint32_t obj = arr + p.index * 0x50;
    const uint32_t id = *reinterpret_cast<const uint32_t*>(obj + 4);
    if ((id >> 24) != p.block || (id & 0xFFFF) != p.index) return nullptr;
    return reinterpret_cast<uint32_t*>(obj + 8);
}

bool groups_are(const uint32_t* g, const uint32_t* want) {
    return g[0] == want[0] && g[1] == want[1] && g[2] == want[2] && g[3] == want[3];
}

bool any_of(const uint32_t* g, const uint32_t* mask) {
    return (g[0] & mask[0]) | (g[1] & mask[1]) | (g[2] & mask[2]) | (g[3] & mask[3]);
}

void show_all_twins() {
    for (size_t i = 0; i < kTwinPieces; ++i) {
        if (!g_hidden[i]) continue;
        uint32_t* g = live_groups(drawdist_twins::kPieces[i]);
        static const uint32_t zero[4] = {};
        if (g && groups_are(g, zero)) std::memcpy(g, drawdist_twins::kPieces[i].groups, 16);
        g_hidden[i] = false;
    }
    g_hidden_count = 0;
}

void twins_tick_body() {
    const LONG mode = g_mode;
    if (mode == 0) {
        if (g_hidden_count) show_all_twins();
        return;
    }
    const uint32_t wcm = *reinterpret_cast<const uint32_t*>(kWorldChrMan);
    const uint32_t chr = wcm ? *reinterpret_cast<const uint32_t*>(wcm + 0x3C) : 0;
    const uint32_t floor = chr ? *reinterpret_cast<const uint32_t*>(chr + 0x29C) : 0;
    if (!floor) return;  // loading or no floor yet: keep what is set
    uint32_t own[4], seen[4];
    std::memcpy(own, reinterpret_cast<const void*>(floor + 0x20), 16);
    if (mode == 2) {
        for (int k = 0; k < 4; ++k) seen[k] = 0xFFFFFFFFu;
    } else {
        const auto* nbr = reinterpret_cast<const uint32_t*>(floor + 0x10);
        for (int k = 0; k < 4; ++k) seen[k] = own[k] | nbr[k];
    }
    bool hide[kTwinPieces] = {};
    for (size_t k = 0; k < kTwinPairs; ++k) {
        const auto& pr = drawdist_twins::kPairs[k];
        const auto& a = drawdist_twins::kPieces[pr.a];
        const auto& b = drawdist_twins::kPieces[pr.b];
        if (!any_of(a.groups, seen) || !any_of(b.groups, seen)) continue;
        const bool a_own = any_of(a.groups, own), b_own = any_of(b.groups, own);
        if (a_own && b_own) continue;  // the game's own picture
        if (a_own) {
            hide[pr.b] = true;
        } else if (b_own) {
            hide[pr.a] = true;
        } else {
            const bool keep_a = a.verts < b.verts || (a.verts == b.verts && a.index < b.index);
            hide[keep_a ? pr.b : pr.a] = true;
        }
    }
    static const uint32_t zero[4] = {};
    LONG hidden = 0;
    for (size_t i = 0; i < kTwinPieces; ++i) {
        const auto& p = drawdist_twins::kPieces[i];
        uint32_t* g = live_groups(p);
        if (!g) {
            g_hidden[i] = false;  // its map is gone (or not this build's): nothing of ours is left there
            continue;
        }
        if (hide[i] && !g_hidden[i] && groups_are(g, p.groups)) {
            std::memcpy(g, zero, 16);
            g_hidden[i] = true;
            InterlockedIncrement(&g_hides);
            static bool told[kTwinPieces] = {};
            if (!told[i]) {
                told[i] = true;
                LOG_INFO("DrawMapPieces: %s (map %s) hidden while a twin of the same surfaces is drawn (no z-fighting)",
                         p.name, p.map);
            }
        } else if (!hide[i] && g_hidden[i]) {
            if (groups_are(g, zero)) std::memcpy(g, p.groups, 16);
            g_hidden[i] = false;
        }
        hidden += g_hidden[i] ? 1 : 0;
    }
    g_hidden_count = hidden;
}

bool g_twins_on = false;

bool install_lod() {
    if (std::memcmp(reinterpret_cast<const void*>(kLodFn), kLodHead, sizeof(kLodHead)) != 0) return false;
    uint8_t code[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&lod_entry) - (kLodFn + 5));
    std::memcpy(code + 1, &rel, 4);
    return write_bytes(kLodFn, code, sizeof(code));
}

}  // namespace

void drawdist_tick() {
    if (!g_twins_on) return;
    __try {
        twins_tick_body();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void drawdist_set_mode(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    InterlockedExchange(&g_mode, mode);
    // The streaming masks follow on the next frame; the draw masks when the floor is applied again (SetFloor 0xE6E580
    // sees no floor yet on the next frame, and FieldArea's update rebuilds them).
    __try {
        const uint32_t wcm = *reinterpret_cast<uint32_t*>(kWorldChrMan);
        const uint32_t chr = wcm ? *reinterpret_cast<uint32_t*>(wcm + 0x3C) : 0;
        if (chr) *reinterpret_cast<uint32_t*>(chr + 0x29C) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void drawdist_set_effect_lod(int percent) {
    if (percent < 25) percent = 25;
    if (percent > 1000) percent = 1000;
    g_fx_inv = 100.0f / static_cast<float>(percent);
}

void drawdist_set_lod(int percent) {
    if (percent < 25) percent = 25;
    if (percent > 1000) percent = 1000;
    g_lod_inv = 100.0f / static_cast<float>(percent);
}

bool drawdist_install(int mode, int lod_percent, int effect_percent) {
    drawdist_set_lod(lod_percent);
    drawdist_set_effect_lod(effect_percent);
    bool fx = true;
    for (const uint32_t site : kFxDistSites) {
        fx = redirect_call(site, kFtol, reinterpret_cast<const void*>(&fx_ftol)) && fx;
    }
    if (!fx) {
        LOG_ERROR("EffectLodDistance: the effect level checks do not match this build; not used");
    } else if (effect_percent != 100) {
        LOG_INFO("EffectLodDistance: fires (torches, bonfires) keep each level of detail %d%% as far away as the "
                 "game's", effect_percent);
    }
    InterlockedExchange(&g_mode, (mode < 0 || mode > 2) ? 0 : mode);
    // All six sites checked before any is changed: the map pieces' masks are widened at two places and noted at two.
    bool match = calls(kDrawGetSite, kGetDisp) && calls(kStreamGetSite, kGetDisp) && calls(kBuildDrawSite, kBuildDraw) &&
                 calls(kStreamDoneSite, kStreamDone);
    for (const uint32_t site : kStreamFloorSites) match = match && calls(site, kStreamFloor);
    bool on = false;
    if (match) {
        on = redirect_call(kDrawGetSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_draw)) &&
             redirect_call(kStreamGetSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_stream)) &&
             redirect_call(kBuildDrawSite, kBuildDraw, reinterpret_cast<const void*>(&build_draw));
        for (const uint32_t site : kStreamFloorSites) {
            on = on && redirect_call(site, kStreamFloor, reinterpret_cast<const void*>(&stream_floor));
        }
        on = on && redirect_call(kStreamDoneSite, kStreamDone, reinterpret_cast<const void*>(&stream_done));
    }
    g_lod_on = install_lod();
    g_twins_on = on;
    if (!on) {
        InterlockedExchange(&g_mode, 0);
        LOG_ERROR("DrawMapPieces: the game's code at %08X/%08X does not match this build; not used",
                  static_cast<unsigned>(kBuildDrawSite), static_cast<unsigned>(kStreamDoneSite));
    } else if (mode == 1) {
        LOG_INFO("DrawMapPieces = adjacent: the map pieces seen from the player's room and from the rooms next to it "
                 "are drawn (the game: this room's only); characters, objects and the rest keep the game's rooms");
    } else if (mode == 2) {
        LOG_INFO("DrawMapPieces = all: every map piece of the loaded maps is drawn wherever the player stands; "
                 "characters, objects and the rest keep the game's rooms");
    }
    if (!g_lod_on) {
        LOG_ERROR("LodDistance: the game's code at %08X does not match this build; not used",
                  static_cast<unsigned>(kLodFn));
    } else if (lod_percent != 100) {
        LOG_INFO("LodDistance: models switch to their lower levels of detail %d%% as far away as the game's", lod_percent);
    }
    return on;
}
