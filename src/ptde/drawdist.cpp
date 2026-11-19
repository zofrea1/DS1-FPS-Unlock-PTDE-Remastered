#include "drawdist.h"

#include "drawdist_twins.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

// DrawMapPieces. A map piece is drawn when one of its draw groups (MSB part +0x38, 128 bits) is among the display
// groups of the frame, which the game takes from the collision the player stands on (its MSB part's +0x48, kept in
// the collision's record at +0x20 and read by 0x4569D0: ecx = record + 8, eax = out): when a map loads (0xDE85D0
// writes them into the draw state's +0x48 at 0xDE862A), when the player steps onto a floor of another block (the
// in-game step 0xEFD2F0 at 0xEFDEE3, into [step+0x38]+0x48), and into the per-block masks rebuilt from the floor
// (0xDE2AE0, 0xDE2C60 -> 0x4791C0 ORs them in), which the pieces' visibility takes too. Each collision is one room:
// it displays its own group, and its draw groups (record +0x10) list the rooms it is shown from, its neighbours.
// Firelink's ledge grass (m5060B2..m5064B2, groups 0-2) showed only from inside the shrine, not from the overlook
// above it (h0027B2: displays group 27, neighbours 0, 1, 2, 11 and 28; 2026-11-18, tested by giving that collision
// groups 0-2: the grass came in a few seconds later).
//  adjacent: those four reads answer the floor's display groups and its draw groups: the pieces seen from this room
//            and from the rooms next to it (the ledge grass from the overlook). Rooms farther away stay culled.
//  all:      every group: every piece of the loaded maps wherever the player stands (the view's own culling still
//            applies). Measured 2026-11-18 at 4K: Demon Ruins +3.5 ms game and +4.2 ms render thread a frame (about
//            300 -> 134 FPS: it draws Lost Izalith behind the walls, same map file), Blighttown +2.4 / +0.6 ms,
//            Firelink +0.5 / +0.6 ms.
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
constexpr uint32_t kLoadSite = 0x00DE8625;  // call 0x4569D0 (a map's load: the draw state's display groups)
constexpr uint32_t kStepSite = 0x00EFDEDE;  // call 0x4569D0 (a floor of another block underfoot)
// The per-block masks rebuilt from every floor of the block (0xDE2AE0, 0xDE2C60 -> 0x4791C0 ORs them in), which
// the pieces' visibility takes too: a floor's own groups alone in the draw state changed nothing (2026-11-18).
constexpr uint32_t kBlockSites[] = {0x00DE2AFA, 0x00DE2C9F};
constexpr uint32_t kLodFn = 0x00F61CD0;
// EffectLodDistance: the effect level checks' call to _ftol on the camera distance (CheckLevel 0xA33600,
// ResetLevelSingle 0xA33720, ResetLevel 0xA33890, DistanceFromCamera 0xA48A00).
constexpr uint32_t kFtol = 0x00ABF1A0;
constexpr uint32_t kFxDistSites[] = {0x00A33700, 0x00A33820, 0x00A339C2, 0x00A48AAA};
constexpr uint8_t kLodHead[9] = {0x51, 0xF3, 0x0F, 0x10, 0x81, 0x50, 0x02, 0x00, 0x00};  // push ecx; movss xmm0,[ecx+250h]

volatile LONG g_mode = 0;  // DrawMapPieces: 0 game, 1 adjacent, 2 all
uint32_t g_get = kGetDisp;
// 0x4569D0 leaves ecx (the floor record + 8) as it was: its draw groups (the neighbouring rooms) are at ecx + 8.
uint32_t g_state = 0;  // the map's draw state ([ebx+0x10] at kLoadSite, [ebx+0x38] at kStepSite): +0x48 its groups
float g_lod_inv = 1.0f;
float g_fx_inv = 1.0f;
uint32_t g_ftol = kFtol;
uint32_t g_lod_back = kLodFn + 9;
bool g_lod_on = false;

__declspec(naked) void get_disp_load() {
    __asm {
        push ecx
        mov ecx, dword ptr [ebx + 0x10]
        mov g_state, ecx
        pop ecx
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        cmp g_mode, 2
        je all
        push edx
        mov edx, dword ptr [ecx + 8]
        or dword ptr [eax], edx
        mov edx, dword ptr [ecx + 12]
        or dword ptr [eax + 4], edx
        mov edx, dword ptr [ecx + 16]
        or dword ptr [eax + 8], edx
        mov edx, dword ptr [ecx + 20]
        or dword ptr [eax + 12], edx
        pop edx
        jmp done
    all:
        mov dword ptr [eax], -1
        mov dword ptr [eax + 4], -1
        mov dword ptr [eax + 8], -1
        mov dword ptr [eax + 12], -1
    done:
        ret
    }
}

__declspec(naked) void get_disp_step() {
    __asm {
        push ecx
        mov ecx, dword ptr [ebx + 0x38]
        mov g_state, ecx
        pop ecx
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        cmp g_mode, 2
        je all
        push edx
        mov edx, dword ptr [ecx + 8]
        or dword ptr [eax], edx
        mov edx, dword ptr [ecx + 12]
        or dword ptr [eax + 4], edx
        mov edx, dword ptr [ecx + 16]
        or dword ptr [eax + 8], edx
        mov edx, dword ptr [ecx + 20]
        or dword ptr [eax + 12], edx
        pop edx
        jmp done
    all:
        mov dword ptr [eax], -1
        mov dword ptr [eax + 4], -1
        mov dword ptr [eax + 8], -1
        mov dword ptr [eax + 12], -1
    done:
        ret
    }
}

__declspec(naked) void get_disp_any() {
    __asm {
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        cmp g_mode, 2
        je all
        push edx
        mov edx, dword ptr [ecx + 8]
        or dword ptr [eax], edx
        mov edx, dword ptr [ecx + 12]
        or dword ptr [eax + 4], edx
        mov edx, dword ptr [ecx + 16]
        or dword ptr [eax + 8], edx
        mov edx, dword ptr [ecx + 20]
        or dword ptr [eax + 12], edx
        pop edx
        jmp done
    all:
        mov dword ptr [eax], -1
        mov dword ptr [eax + 4], -1
        mov dword ptr [eax + 8], -1
        mov dword ptr [eax + 12], -1
    done:
        ret
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

// --- stand-ins (see the top) -----------------------------------------------------------------------------------
constexpr uint32_t kPieceMan = 0x0137D5F4;
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
    // At once when the draw state is known (the player's floor record: draw groups +0x10, display groups +0x20).
    const uint32_t state = g_state;
    __try {
        const uint32_t wcm = *reinterpret_cast<uint32_t*>(0x0137D644);
        const uint32_t chr = wcm ? *reinterpret_cast<uint32_t*>(wcm + 0x3C) : 0;
        const uint32_t floor = chr ? *reinterpret_cast<uint32_t*>(chr + 0x29C) : 0;
        if (state) {
            auto* groups = reinterpret_cast<uint32_t*>(state + 0x48);
            if (mode == 2) {
                for (int i = 0; i < 4; ++i) groups[i] = 0xFFFFFFFFu;
            } else if (floor) {
                const auto* disp = reinterpret_cast<const uint32_t*>(floor + 0x20);
                const auto* nbr = reinterpret_cast<const uint32_t*>(floor + 0x10);
                for (int i = 0; i < 4; ++i) groups[i] = disp[i] | (mode == 1 ? nbr[i] : 0u);
            }
        }
        // And the floor applied again on the next frame (SetFloor 0xE6E580 sees no floor yet), which rebuilds the
        // per-block masks through the sites above.
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
    const bool load = redirect_call(kLoadSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_load));
    bool step = redirect_call(kStepSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_step));
    for (const uint32_t site : kBlockSites) {
        step = redirect_call(site, kGetDisp, reinterpret_cast<const void*>(&get_disp_any)) && step;
    }
    g_lod_on = install_lod();
    g_twins_on = load && step;
    if (!load || !step) {
        LOG_ERROR("DrawMapPieces: the game's code at %08X/%08X does not match this build; not used",
                  static_cast<unsigned>(kLoadSite), static_cast<unsigned>(kStepSite));
    } else if (mode == 1) {
        LOG_INFO("DrawMapPieces = adjacent: the map pieces seen from the player's room and from the rooms next to it "
                 "are drawn (the game: this room's only)");
    } else if (mode == 2) {
        LOG_INFO("DrawMapPieces = all: every map piece of the loaded maps is drawn wherever the player stands");
    }
    if (!g_lod_on) {
        LOG_ERROR("LodDistance: the game's code at %08X does not match this build; not used",
                  static_cast<unsigned>(kLodFn));
    } else if (lod_percent != 100) {
        LOG_INFO("LodDistance: models switch to their lower levels of detail %d%% as far away as the game's", lod_percent);
    }
    return load && step;
}
