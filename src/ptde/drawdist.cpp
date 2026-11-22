#include "drawdist.h"

#include "drawdist_detail.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
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
// The two options widen what the player's floor adds to its block's two masks, for what is only seen: the floor's
// contribution is noted as the game reads it (0xDE2C9F, 0xDE2AFA) and ORed into the masks of the map pieces, the map
// effects (WorldSfxManImp [0x137D6E4]: torches' fire, mist, embers) and the map's point lights
// (WorldMapPointLightManImp [0x137D6EC]) after the game has written them (after 0xDE2C60 returns; before 0xE9BD70 at
// 0xDE8198, right after the streaming masks are written); the renderer's own groups (GameRend+0x48, see kLoadSite) are
// widened the same way. Since 2.7.1 (DrawMapObjects) the objects too (WorldObjManImp [0x137D5F8]: ladders, doors,
// crates, barrels, item corpses; both masks, so they are loaded as when the player stands there: checked 2026-11-21 in
// the Undead Burg with all, every one of its 528 objects stayed where the MSB puts it, crates and barrels only move
// once broken; with the streaming mask alone left out they were not drawn). Characters, collisions and sounds keep
// the game's own masks: the renderer draws only what is in the scene, so they are drawn where the game has them
// (characters cannot be drawn without being loaded, and loading them wakes them: 2026-11-21 with all from the Undead
// Burg bridge 137 more became active within a second, one of the Bell Gargoyles left its roof and their fight began,
// rats walked off).
//  adjacent: the floor's display groups and its draw groups: the pieces seen from this room and from the rooms next to
//            it (the ledge grass from the overlook). Rooms farther away stay culled.
//  all:      every group: every piece of the loaded maps wherever the player stands (the view's own culling still
//            applies). Measured 2026-11-18 at 4K: Demon Ruins +3.5 ms game and +4.2 ms render thread a frame (about
//            300 -> 134 FPS: it draws Lost Izalith behind the walls, same map file), Blighttown +2.4 / +0.6 ms,
//            Firelink +0.5 / +0.6 ms (measured up to 2.7.0, with every other manager's masks widened as well).
// Since 2.7.1 only those three managers and the renderer are widened. Up to 2.7.0 (the 2.6.0 and 2.7.0 releases) the
// four reads themselves answered the widened groups, so every manager got them: in the whole block (all) or the next
// rooms (adjacent) every character was asked for and active, and with it the map's events that wait for one to be
// loaded. The Undead Burg's 11015100 places Solaire's summon sign at the Bell Gargoyles once his summon
// (6540, beside their fog) is loaded and the player is human, and disables his Undead Burg self (6001): with all it ran
// at the Undead Burg bonfire and Solaire was gone from his place past the Taurus Demon (user report 2026-11-19;
// reproduced: flag 11015100 on and 6001 disabled within a second of being human there; not with game). An infinite
// draw distance mod for Remastered was reported to do the same.
// LodDistance. 0xF61CD0 (ecx = the model, esi = its LodBank row, al = its current level, xmm1 = its distance from the
// camera) picks level 0, 1 or 2 from the row's lv01/lv12 border distances and margins (default row 0: 15/30 m in
// Prepare to Die Edition, 25/50 m in Remastered). The distance is scaled by 100 / LodDistance on entry, so every
// border moves out by LodDistance percent.
//
// Detail rules (since 2.7.1). Many surfaces exist twice: a detailed piece drawn from the rooms that look at it up close
// and a cheaper stand-in of the same surfaces drawn from the rooms that see it from afar (from the Undead Burg's bridge
// the game draws m2420B1, 23k vertices, for the 24 pieces of the lower Burg, about 430k; Anor Londo's m8010B1 for
// m2200B1). Seen from one room the game draws one of them; adjacent and all add the other, so the two fought over the
// depth (z-fighting: Anor Londo's terraces flickered, 2026-11-19), and up to the 2.7.1 tests the added one was hidden,
// which kept the coarse one wherever the player's room showed it (user, 2026-11-21: "we want the high detail to win
// almost always"). The tables (drawdist_detail.h, from the maps' own data by gen_detail.py) list every pair of pieces
// whose surfaces overlap (same plane within 5 cm, 0.25 m^2 or more, whatever their draw groups) with the finer of the
// two (smaller triangles where they overlap), the stand-ins (pieces that finer pieces cover: 75% of the area on their
// surfaces, nothing over 3% farther than 1 m from them) and the coarse far backdrops (a side over 250 m, nearly all of
// the area in triangles over 100 m^2). Once a frame, with adjacent or all:
//  - a backdrop the game does not draw from here is not added: Darkroot's valley floor below the Undead Burg
//    (m2372B1), drawn from the bridge's deck where the parapets hide its edges, looked like a broken dark slab from
//    Solaire's platform next to it (user report 2026-11-21);
//  - a stand-in that would be drawn is hidden and the finer pieces covering it are drawn in its place, from any room
//    (their groups set to all ones), unless the game draws all of them itself (its own picture) or the player is
//    within one of those pieces' surfaces (their 2 m occupancy grids; the occlusion code leaves a piece of another
//    room holding the player out);
//  - of two other overlapping pieces with no draw group in common (versions of the same surfaces for different
//    rooms, usually two tessellations of one shape rather than a coarse and a detailed copy: drawn together they cut
//    through each other where they are not in one plane, as the cliff and wall seen along the Undead Burg bridge did,
//    user report 2026-11-21), one is drawn: the player's room's own, or of two others the finer;
//  - of the same model placed twice (rooms that differ in light or fog), the player's room's copy;
//  - of two overlapping pieces still both drawn (not both by the game itself), the coarser is drawn one depth level
//    farther than the finer (occl.cpp's depth bias: a level is about 10 cm at the piece's nearest point; the game's
//    own pieces start at level 0, added ones at 1), so the finer wins where they overlap and the coarser's other
//    surfaces stay.
// Hidden means its draw groups cleared in the map piece manager ([0x137D5F4]: +0x1C the blocks' count, +0x20 the
// blocks, 0x5C each, one per map in a fixed order; a block's +0x2C its pieces' count, +0x30 its pieces, 0x50 each:
// +0x04 the id (block << 24 | index), +0x08 the draw groups the frame's culling tests). Everything is put back in game
// mode. Lower Burg from the bridge drawn in detail (with the sun's shadows and the point lights the detailed pieces
// take and the stand-in does not): 320 -> 271 FPS at 4K looking down on it.
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
// The renderer's own groups (GameRend, FieldArea+0x10: +0x48), taken from the floor when a map loads (0xDE8625, into
// [ebx+0x10]+0x48) and when the player steps onto a floor of another block (0xEFDEDE, into [ebx+0x38]+0x48): what is in
// the scene is drawn only where its groups meet them too (a map piece added to the scene by its block's streaming mask
// alone was not drawn: the same draw calls, 2026-11-19).
constexpr uint32_t kLoadSite = 0x00DE8625;
constexpr uint32_t kStepSite = 0x00EFDEDE;
constexpr uint32_t kDrawGetSite = 0x00DE2C9F;    // call 0x4569D0 in 0xDE2C60 (the draw masks): the floor's groups
constexpr uint32_t kStreamGetSite = 0x00DE2AFA;  // call 0x4569D0 in 0xDE2AE0 (a floor's streaming groups)
constexpr uint32_t kBuildDraw = 0x00DE2C60;      // stdcall(FieldArea), eax = the floor: every block's draw mask
constexpr uint32_t kBuildDrawSite = 0x00DE8615;  // its call in FieldArea's update 0xDE85D0
constexpr uint32_t kStreamFloor = 0x00DE2AE0;    // stdcall(FieldArea, masks), eax = a floor: adds its groups
constexpr uint32_t kStreamFloorSites[] = {0x00DE807C, 0x00DE8096};  // its calls in the streaming update 0xDE7C30
constexpr uint32_t kStreamDone = 0x00E9BD70;     // called by 0xDE7C30 right after it wrote the streaming masks
constexpr uint32_t kStreamDoneSite = 0x00DE8198;
constexpr uint32_t kPieceManVar = 0x0137D5F4;    // WorldMapManImp, the map pieces' manager (+0x1C blocks' count)
constexpr uint32_t kFieldAreaVar = 0x01378520;   // FieldArea: +8 its world info, +0x0C the blocks the masks go to
constexpr int kMaxBlocks = 32;
// The masks widened, per manager: its +0x20 = one entry per block (the order of the map piece manager's), each with
// its draw mask (0xDE2C60 writes it, 0xDE2F67 / 0xDE2FC4 for the effects and lights) and its streaming mask (0xDE7C30,
// 0xDE817B for the effects; the lights have none), 16 bytes each.
struct Widened {
    uint32_t man_var;
    uint32_t size;
    uint32_t draw;
    uint32_t stream;  // 0: none
};
constexpr Widened kWidened[] = {
    {0x0137D5F4, 0x5C, 0x0C, 0x1C},  // WorldMapManImp: map pieces
    {0x0137D6E4, 0x3C, 0x1C, 0x0C},  // WorldSfxManImp: map effects
    {0x0137D6EC, 0x2C, 0x0C, 0},     // WorldMapPointLightManImp: point lights
    {0x0137D5F8, 0xEC, 0x0C, 0x1C},  // WorldObjManImp: objects (ladders, doors, crates, barrels, item corpses)
};
constexpr uint32_t kLodFn = 0x00F61CD0;
// EffectLodDistance: the effect level checks' call to _ftol on the camera distance (CheckLevel 0xA33600,
// ResetLevelSingle 0xA33720, ResetLevel 0xA33890, DistanceFromCamera 0xA48A00).
constexpr uint32_t kFtol = 0x00ABF1A0;
constexpr uint32_t kFxDistSites[] = {0x00A33700, 0x00A33820, 0x00A339C2, 0x00A48AAA};
constexpr uint8_t kLodHead[9] = {0x51, 0xF3, 0x0F, 0x10, 0x81, 0x50, 0x02, 0x00, 0x00};  // push ecx; movss xmm0,[ecx+250h]

volatile LONG g_mode = 0;  // DrawMapPieces: 0 game, 1 adjacent, 2 all, 3 adjacent+

// adjacent+ (2026-11-21, user: "something in-between adjacent and all ... include those crates ... rule out the really
// far stuff"): the rooms up to kHops steps away, each step a room's draw groups (its neighbours). A room is a collision
// of the floor's block: WorldHitManImp [0x137D6B8] +0x24 the blocks, 0x68 each (the map piece manager's order): +0x3C
// the count, +0x40 the HitIns, 0x84 each, in place: +0x10 its draw groups (the rooms next to it), +0x20 its display
// groups (its own), +0xF its block. From the Undead Burg bridge's deck (room 113): adjacent 10 rooms, two steps 22,
// three 33 of the map's 128; of the objects there, adjacent adds 59, three steps 82 + 41 more, all 289 more; three
// steps bring most of the street below the bridge's start (17 of its 24 objects; the rest only with all).
constexpr uint32_t kHitManVar = 0x0137D6B8;
constexpr int kHops = 3;
struct HopCache {
    uint32_t rec8;
    uint32_t hits;  // the block's collision array it was made from (a reloaded map makes another)
    uint32_t groups[4];
};
HopCache g_hops[8] = {};
int g_hop_next = 0;

bool meets(const uint32_t* a, const uint32_t* b) {
    return (a[0] & b[0]) | (a[1] & b[1]) | (a[2] & b[2]) | (a[3] & b[3]);
}

// The rooms within kHops steps of the floor (rec8 = its HitIns + 8), its own included; false when its block's
// collisions cannot be read (then the floor's draw groups alone: adjacent).
bool hop_groups(uint32_t rec8, uint32_t out[4]) {
    const uint32_t hit = rec8 - 8;
    const int block = *reinterpret_cast<const int8_t*>(hit + 0xF);
    const uint32_t man = *reinterpret_cast<const uint32_t*>(kHitManVar);
    const uint32_t arr = man ? *reinterpret_cast<const uint32_t*>(man + 0x24) : 0;
    if (!arr || block < 0 || block >= kMaxBlocks) return false;
    const uint32_t blk = arr + block * 0x68;
    const int n = *reinterpret_cast<const int*>(blk + 0x3C);
    const uint32_t hits = *reinterpret_cast<const uint32_t*>(blk + 0x40);
    if (!hits || n <= 0 || n > 4096 || hit < hits || hit >= hits + n * 0x84) return false;
    for (const HopCache& c : g_hops) {
        if (c.rec8 == rec8 && c.hits == hits) {
            std::memcpy(out, c.groups, 16);
            return true;
        }
    }
    uint32_t set[4];
    for (int k = 0; k < 4; ++k) {
        set[k] = reinterpret_cast<const uint32_t*>(hit + 0x20)[k] | reinterpret_cast<const uint32_t*>(hit + 0x10)[k];
    }
    for (int step = 1; step < kHops; ++step) {
        uint32_t next[4] = {set[0], set[1], set[2], set[3]};
        for (int i = 0; i < n; ++i) {
            const uint32_t h = hits + i * 0x84;
            if (!meets(reinterpret_cast<const uint32_t*>(h + 0x20), set)) continue;
            const auto* nb = reinterpret_cast<const uint32_t*>(h + 0x10);
            for (int k = 0; k < 4; ++k) next[k] |= nb[k];
        }
        std::memcpy(set, next, 16);
    }
    HopCache& c = g_hops[g_hop_next];
    g_hop_next = (g_hop_next + 1) % 8;
    c.rec8 = rec8;
    c.hits = hits;
    std::memcpy(c.groups, set, 16);
    std::memcpy(out, set, 16);
    return true;
}

// What the mode adds for a floor (rec8 = its HitIns + 8): its draw groups (adjacent), the rooms within kHops steps
// (adjacent+) or every group (all).
void widen_groups(uint32_t rec8, uint32_t out[4]) {
    const LONG mode = g_mode;
    if (mode == 2) {
        for (int k = 0; k < 4; ++k) out[k] = 0xFFFFFFFFu;
        return;
    }
    if (mode == 3 && hop_groups(rec8, out)) return;
    std::memcpy(out, reinterpret_cast<const void*>(rec8 + 8), 16);
}

// The renderer's groups widened in place (the stubs below: ecx = the floor's record + 8, eax = 0x4569D0's answer).
void __cdecl widen_into(uint32_t rec8, uint32_t* out) {
    __try {
        uint32_t w[4];
        widen_groups(rec8, w);
        for (int k = 0; k < 4; ++k) out[k] |= w[k];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

volatile LONG g_widen = 15;  // diagnostics (RemoteControl drawwiden): the widened managers, bit i = kWidened[i]
uint32_t g_get = kGetDisp;
uint32_t g_build_draw = kBuildDraw;
uint32_t g_stream_floor = kStreamFloor;
uint32_t g_stream_done = kStreamDone;
float g_lod_inv = 1.0f;
float g_fx_inv = 1.0f;
uint32_t g_ftol = kFtol;
uint32_t g_lod_back = kLodFn + 9;
bool g_lod_on = false;

// What the floors add for the widened managers alone, per block, until it is ORed into their masks (the game thread:
// FieldArea's update and the streaming update run there, one after the other).
uint32_t g_extra_draw[kMaxBlocks][4] = {};
// Each block's draw mask as the game wrote it (0xDE2C60, before ours is ORed in): what the game itself draws of each
// loaded map from where the player is (the player's map from the floor's groups, the others from the map's
// connections). The floor's groups say nothing about another map's pieces (each map numbers its groups itself): judged
// by them, a neighbouring map's pieces the game draws counted as another room's (user report 2026-11-20: at the
// aqueduct's end the Undead Burg was missing, pieces of it left out as "holding the player", detailed ones hidden for
// their stand-ins). Game thread.
uint32_t g_game_draw[kMaxBlocks][4] = {};
int g_game_draw_blocks = 0;  // blocks recorded (0: none yet)
uint32_t g_game_sfx[kMaxBlocks][4] = {};  // the same for the map effects (WorldSfxManImp: 0x3C a block, mask +0x1C)
int g_game_sfx_blocks = 0;
uint32_t g_game_draw_gen = 0;
uint32_t g_extra_stream[kMaxBlocks][4] = {};
bool g_any_draw = false;
bool g_any_stream = false;
volatile LONG g_in_stream = 0;  // inside the streaming update's floor calls (0xDE2AE0 has other callers: load checks)
// 0x4569D0 leaves ecx (the floor record + 8) as it was: its draw groups (the neighbouring rooms) are at ecx + 8.
uint32_t g_rend = 0;  // GameRend ([ebx+0x10] at kLoadSite, [ebx+0x38] at kStepSite): +0x48 the groups it draws

// The renderer's groups widened in place (eax = the answer of 0x4569D0): the floor's draw groups added (adjacent), the
// rooms within kHops steps (adjacent+), or every group (all). Only GameRend takes these two answers.
__declspec(naked) void get_disp_load() {
    __asm {
        push ecx
        mov ecx, dword ptr [ebx + 0x10]
        mov g_rend, ecx
        pop ecx
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        pushad
        push eax
        push ecx
        call widen_into
        add esp, 8
        popad
    done:
        ret
    }
}

__declspec(naked) void get_disp_step() {
    __asm {
        push ecx
        mov ecx, dword ptr [ebx + 0x38]
        mov g_rend, ecx
        pop ecx
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        pushad
        push eax
        push ecx
        call widen_into
        add esp, 8
        popad
    done:
        ret
    }
}

// A floor's groups as the game reads them (rec8 = the floor's record + 8, the ecx of 0x4569D0): its block (record
// +0x0F) gets its draw groups (record +0x10, the rooms next to it) with adjacent, or every group with all.
void __cdecl note_floor(uint32_t rec8, int stream) {
    __try {
        const int block = *reinterpret_cast<const int8_t*>(rec8 + 7);
        if (block < 0 || block >= kMaxBlocks) return;
        uint32_t* e = stream ? g_extra_stream[block] : g_extra_draw[block];
        uint32_t w[4];
        widen_groups(rec8, w);
        for (int k = 0; k < 4; ++k) e[k] |= w[k];
        (stream ? g_any_stream : g_any_draw) = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The noted groups ORed into the draw (or streaming) masks of the widened managers, as many blocks as the game wrote
// (FieldArea's world info +0x0C, and no more than the map piece manager holds), then forgotten.
void apply_extra(uint32_t (*extra)[4], bool* any, bool stream) {
    if (!*any) return;
    *any = false;
    __try {
        const uint32_t fa = *reinterpret_cast<const uint32_t*>(kFieldAreaVar);
        const uint32_t info = fa ? *reinterpret_cast<const uint32_t*>(fa + 8) : 0;
        const uint32_t pieces = *reinterpret_cast<const uint32_t*>(kPieceManVar);
        int count = info ? *reinterpret_cast<const int*>(info + 0x0C) : 0;
        const int piece_blocks = pieces ? *reinterpret_cast<const int*>(pieces + 0x1C) : 0;
        if (count > piece_blocks) count = piece_blocks;
        if (count > kMaxBlocks) count = kMaxBlocks;
        for (int wi = 0; wi < static_cast<int>(sizeof(kWidened) / sizeof(kWidened[0])); ++wi) {
            if (!(g_widen & (1 << wi))) continue;
            const Widened& w = kWidened[wi];
            const uint32_t off = stream ? w.stream : w.draw;
            const uint32_t man = *reinterpret_cast<const uint32_t*>(w.man_var);
            const uint32_t blocks = man ? *reinterpret_cast<const uint32_t*>(man + 0x20) : 0;
            if (!off || !blocks) continue;
            for (int b = 0; b < count; ++b) {
                auto* mask = reinterpret_cast<uint32_t*>(blocks + b * w.size + off);
                for (int k = 0; k < 4; ++k) mask[k] |= extra[b][k];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    std::memset(extra, 0, sizeof(uint32_t) * 4 * kMaxBlocks);
}

// The render thread's copy of a block's draw mask: 0xDE2C60 also queues each block's mask for the two render objects of
// its world info entry ([FieldArea+8]+0x10, 0x3C each: +0x2C through 0x1000610, +0x30 through 0xFCE250; ecx = the
// object, eax = the render command buffer [[[0x13783C0]+8]]+0x28, the 16-byte mask on the stack, the caller pops it).
// A map piece in the scene and in GameRend's groups was still not drawn while these had the game's mask (2026-11-19).
constexpr uint32_t kRenderMaskA = 0x01000610;
constexpr uint32_t kRenderMaskB = 0x00FCE250;
constexpr uint32_t kRenderCmdVar = 0x013783C0;

void queue_render_mask(uint32_t fn, uint32_t obj, uint32_t buf, const uint32_t* m) {
    const uint32_t m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3];
    __asm {
        push m3
        push m2
        push m1
        push m0
        mov ecx, obj
        mov eax, buf
        call dword ptr [fn]
        add esp, 16
    }
}

// Right after 0xDE2C60 (the game thread, where it queued its own): every block given extra groups gets its widened
// mask (the map piece manager's, now ORed) queued for its render objects again.
void requeue_render_masks(const bool* widened, int count) {
    __try {
        const uint32_t fa = *reinterpret_cast<const uint32_t*>(kFieldAreaVar);
        const uint32_t info = fa ? *reinterpret_cast<const uint32_t*>(fa + 8) : 0;
        const uint32_t entries = info ? *reinterpret_cast<const uint32_t*>(info + 0x10) : 0;
        const uint32_t pieces = *reinterpret_cast<const uint32_t*>(kPieceManVar);
        const uint32_t blocks = pieces ? *reinterpret_cast<const uint32_t*>(pieces + 0x20) : 0;
        const uint32_t r = *reinterpret_cast<const uint32_t*>(kRenderCmdVar);
        const uint32_t r8 = r ? *reinterpret_cast<const uint32_t*>(r + 8) : 0;
        const uint32_t r0 = r8 ? *reinterpret_cast<const uint32_t*>(r8) : 0;
        const uint32_t buf = r0 ? *reinterpret_cast<const uint32_t*>(r0 + 0x28) : 0;
        if (!entries || !blocks || !buf) return;
        const int piece_blocks = *reinterpret_cast<const int*>(pieces + 0x1C);
        for (int b = 0; b < count && b < piece_blocks; ++b) {
            if (!widened[b]) continue;
            const auto* mask = reinterpret_cast<const uint32_t*>(blocks + b * 0x5C + 0x0C);
            const uint32_t a = *reinterpret_cast<const uint32_t*>(entries + b * 0x3C + 0x2C);
            const uint32_t c = *reinterpret_cast<const uint32_t*>(entries + b * 0x3C + 0x30);
            if (a) queue_render_mask(kRenderMaskA, a, buf, mask);
            if (c) queue_render_mask(kRenderMaskB, c, buf, mask);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The map piece manager's draw masks as the game has just written them (before apply_extra).
void note_game_draw() {
    __try {
        const uint32_t fa = *reinterpret_cast<const uint32_t*>(kFieldAreaVar);
        const uint32_t info = fa ? *reinterpret_cast<const uint32_t*>(fa + 8) : 0;
        const uint32_t pieces = *reinterpret_cast<const uint32_t*>(kPieceManVar);
        const uint32_t blocks = pieces ? *reinterpret_cast<const uint32_t*>(pieces + 0x20) : 0;
        int count = info ? *reinterpret_cast<const int*>(info + 0x0C) : 0;
        const int piece_blocks = pieces ? *reinterpret_cast<const int*>(pieces + 0x1C) : 0;
        if (count > piece_blocks) count = piece_blocks;
        if (count > kMaxBlocks) count = kMaxBlocks;
        if (!blocks) count = 0;
        for (int b = 0; b < count; ++b) std::memcpy(g_game_draw[b], reinterpret_cast<const void*>(blocks + b * 0x5C + 0x0C), 16);
        g_game_draw_blocks = count;
        const uint32_t sfx = *reinterpret_cast<const uint32_t*>(0x0137D6E4);
        const uint32_t sblocks = sfx ? *reinterpret_cast<const uint32_t*>(sfx + 0x20) : 0;
        int scount = sfx ? *reinterpret_cast<const int*>(sfx + 0x1C) : 0;
        if (scount > count) scount = count;
        if (!sblocks) scount = 0;
        for (int b = 0; b < scount; ++b) std::memcpy(g_game_sfx[b], reinterpret_cast<const void*>(sblocks + b * 0x3C + 0x1C), 16);
        g_game_sfx_blocks = scount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_game_draw_blocks = 0;
        g_game_sfx_blocks = 0;
    }
    ++g_game_draw_gen;
}

void __cdecl apply_draw() {
    note_game_draw();
    if (!g_any_draw) return;
    bool widened[kMaxBlocks] = {};
    int count = 0;
    __try {
        const uint32_t fa = *reinterpret_cast<const uint32_t*>(kFieldAreaVar);
        const uint32_t info = fa ? *reinterpret_cast<const uint32_t*>(fa + 8) : 0;
        count = info ? *reinterpret_cast<const int*>(info + 0x0C) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (count > kMaxBlocks) count = kMaxBlocks;
    for (int b = 0; b < kMaxBlocks; ++b) {
        widened[b] = (g_extra_draw[b][0] | g_extra_draw[b][1] | g_extra_draw[b][2] | g_extra_draw[b][3]) != 0;
    }
    apply_extra(g_extra_draw, &g_any_draw, false);
    requeue_render_masks(widened, count);
}
void __cdecl apply_stream() { apply_extra(g_extra_stream, &g_any_stream, true); }

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
// map pieces, effects and lights.
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

// --- scenery characters -------------------------------------------------------------------------------------------
// Characters keep the game's rooms: each frame 0xE3E870 (WorldChrMan, from 0xDE81F0) walks every character of the
// loaded maps and tests its groups (0x456B50) against its block's streaming mask with 0x42DAA0 at 0xE3EA13 (eax = the
// AND of the two, al = any bit set); a character that passes is made active ([chr+0xC]+0xC = 2) unless an event has
// turned it off (chr+0x14), one that does not is made inactive (0) and is neither updated nor drawn. A loaded,
// active character the player's room does not display is drawn wherever adjacent or all widen the renderer's groups,
// at the lower update rate the game keeps for characters it believes unseen (Firelink's crow from below its ruins,
// user report 2026-11-20). The characters below also pass that test wherever their map is loaded (with all, or with
// adjacent too where their entry says so), so they are seen from as far as the widened groups reach (the crow from the
// aqueduct to the Undead Burg). Only characters checked to be scenery: no event of theirs depends on where the player
// is, and being active early starts nothing. While one is active it is also updated every frame: WorldChrMan's pass
// (0xE3F7D7..0xE3F8D7) gives a character the player's room does not display the rate -2 (updated now and then: its
// animation moved in steps from afar) and an inactive one -1 (none), unless chr+0x1FD bit 0 fixes the rate (chr+0xF4)
// as it stands, as the event command "Set Network Update Rate: Always" does for the Hellkite; that bit with rate 0 is
// set while the character is active ([chr+0xC]+8 = 2) and taken off again when it is not (or DrawMapPieces is game),
// so the game's -1 applies to it inactive as before.
// (Making every character active was what woke whole maps up to 2.7.0: see the top.)
struct Scenery {
    int entity;
    int from_mode;     // the lowest DrawMapPieces mode it applies with: 1 adjacent, 2 all
    int hostile[2];    // event flags that make it hostile (0: none): then the game's rooms again, so an angered
                       // character does not follow the player out of them
    const char* what;
};
// Firelink Shrine (m10_02), checked 2026-11-20 against every map's events (and the template events they are passed
// to) and their talk scripts: no event waits for one of them to be loaded (no 4[07] backread condition, the
// Solaire kind), the events that move, anger, kill or remove them go by event flags, the player's map, damage and
// HP; every flag their talk scripts set follows a spoken line, a menu choice or the player within 5 m. The
// Asylum's arrival event (11020001) keeps them all unloaded until the crow has landed (backread removed, chr+0x14,
// which the test above keeps). One step depends on the Fire Keeper being active: 11020110 sends her talk script
// 1701 once flag 11020609 is set and waits for its answer (flag 11020101): it may now come while the player is still
// on the aqueduct, with the same result. Left out: the Undead Merchant's Firelink copy (6240, c2510 on the aqueduct),
// whose template event (11020504) gives AI commands, short warps and a home area. Hostile flags from their
// hostility events (11020510 and 11020501-11020503). Each with adjacent too (the user's choice, 2026-11-20).
constexpr Scenery kScenery[] = {
    // c3510 on the ruins north of the bonfire. Its one event (11025050) makes it immortal and, once its HP is below
    // 30%, plays its fly-away animation (7000) and turns its AI off.
    {1020100, 1, {0, 0}, "Firelink Shrine's crow"},
    {6270, 1, {1461, 0}, "the Crestfallen Warrior"},
    {6060, 1, {0, 0}, "the Fire Keeper"},  // in her cell: immortal, never hostile
    {6031, 1, {1096, 0}, "Big Hat Logan (Firelink)"},
    {6041, 1, {1114, 0}, "Griggs of Vinheim (Firelink)"},
    {6070, 1, {1176, 0}, "Rhea of Thorolund (Firelink)"},
    {6080, 1, {1197, 1195}, "Petrus of Thorolund"},
    {6090, 1, {1213, 0}, "Vince of Thorolund"},
    {6100, 1, {1223, 0}, "Nico of Thorolund"},
    {6131, 1, {1253, 0}, "Laurentius (Firelink)"},
    {6181, 1, {1314, 0}, "Ingward (Firelink)"},
    {6261, 1, {1434, 0}, "Domhnall of Zena (Firelink)"},
    {6287, 1, {1512, 0}, "Siegmeyer of Catarina (Firelink)"},
    {6292, 1, {1547, 0}, "Sieglinde of Catarina (Firelink)"},
    {6301, 1, {1574, 0}, "Knight Lautrec (Firelink)"},
    {6322, 1, {1627, 0}, "Trusty Patches (Firelink)"},
    {6330, 1, {0, 0}, "Kingseeker Frampt"},  // leaves when attacked (11020425), never fights
};
constexpr int kSceneryCount = static_cast<int>(sizeof(kScenery) / sizeof(kScenery[0]));
constexpr uint32_t kChrTestSite = 0x00E3EA13;  // call 0x42DAA0 in 0xE3E870's loop (esi = the character)
constexpr uint32_t kMaskAny = 0x0042DAA0;
// Their characters as WorldChrMan's loop last passed them (game thread): it visits every character of the loaded maps
// each frame, so the entity id at chr+0x208 names them there without the event system's lookup (simpler; measured, the
// lookups themselves cost little: the game thread's +0.23..0.28 ms with them kept is their every-frame updates, see
// scenery_tick). A character not passed for two frames is gone (its map unloaded): its pointer is
// not used again.
constexpr uint32_t kChrEntity = 0x208;
uint32_t g_scenery_chr[kSceneryCount] = {};    // the character last passed
uint32_t g_scenery_seen[kSceneryCount] = {};   // the frame it was (g_scenery_frame)
uint8_t g_scenery_want[kSceneryCount] = {};    // kept active this frame (mode, option, not hostile)
uint32_t g_scenery_fixed[kSceneryCount] = {};  // the character whose rate we fixed (its bit 0 was clear before)
uint32_t g_scenery_frame = 1;
volatile LONG g_scenery_on = 0;  // the test site redirected and not switched off (RemoteControl `scenery 0`); 2:
                                 // kept active at the game's rate (RemoteControl `scenery 2`, diagnostics)
volatile LONG g_scenery_told = 0;

// An event flag (EventFlagMan [0x137D7D4], 0xD38C40: eax = the manager, cl = 0, push id, returns al), or false.
bool flag_on(int id) {
    const uint32_t efm = *reinterpret_cast<const uint32_t*>(0x0137D7D4);
    if (!efm) return false;
    uint8_t on = 0;
    __try {
        __asm {
            push id
            xor ecx, ecx
            mov eax, efm
            mov edx, 0x00D38C40
            call edx
            mov on, al
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        on = 0;
    }
    return on != 0;
}

// Enabled: the event system's enable state. [chr+0xC]+0x14 bit 0 is set while an event keeps the character disabled
// (0xE69A40, called by the event command at 0xD6DEA0 with the entity and the state, sets it with the controller's
// +0xC4 bit 6; the game's own backread check 0xD6DE60 counts a disabled one as not loaded): at Firelink, those who have
// not come to the shrine yet. Kept while the character is inactive (chr+0x1FD bit 7 is not: the game sets that one
// when an enabled character first becomes active, so it was clear for everyone on a save loaded away from the
// shrine; user report 2026-11-20, "Corndog" loaded on the aqueduct).
bool chr_enabled(uint32_t chr) {
    const uint32_t state = *reinterpret_cast<const uint32_t*>(chr + 0xC);
    return state && !(*reinterpret_cast<const uint8_t*>(state + 0x14) & 1);
}

// For every character the loop tests (cdecl, from chr_test): the game's answer `ok`, or yes for one of ours kept
// active; ours are noted for scenery_tick.
int __cdecl scenery_check(uint32_t chr, int ok) {
    if (!g_scenery_on || !chr) return ok;
    const int id = *reinterpret_cast<const int*>(chr + kChrEntity);
    if (id != 1020100 && (id < 6000 || id > 6999)) return ok;  // every entity in the table
    for (int k = 0; k < kSceneryCount; ++k) {
        if (id != kScenery[k].entity) continue;
        g_scenery_chr[k] = chr;
        g_scenery_seen[k] = g_scenery_frame;
        // Only one an event has not disabled (keeping the others active cost the render thread about 17%).
        if (ok || !g_scenery_want[k] || !chr_enabled(chr)) return ok;
        if (!(g_scenery_told & (1 << k))) {
            InterlockedOr(&g_scenery_told, 1 << k);
            LOG_INFO("DrawMapPieces: %s kept active outside its rooms (seen from farther, updated every frame)",
                     kScenery[k].what);
        }
        return 1;
    }
    return ok;
}

// In place of the call to 0x42DAA0 at kChrTestSite: the game's answer, then scenery_check's.
__declspec(naked) void chr_test() {
    __asm {
        mov edx, 0x0042DAA0
        call edx
        push ecx
        movzx eax, al
        push eax
        push esi
        call scenery_check
        add esp, 8
        pop ecx
        ret
    }
}

// Each frame (game thread, before WorldChrMan's pass): which are kept active, and the update rate of those that are.
void scenery_tick() {
    const LONG mode = g_mode;
    const uint32_t frame = ++g_scenery_frame;
    const bool flags_now = (frame % 10) == 0;  // the hostile flags every tenth frame
    for (int k = 0; k < kSceneryCount; ++k) {
        const Scenery& sc = kScenery[k];
        const bool on = g_scenery_on && mode >= sc.from_mode;
        if (!on) {
            g_scenery_want[k] = 0;
        } else if (flags_now || !g_scenery_want[k]) {
            const bool hostile = (sc.hostile[0] && flag_on(sc.hostile[0])) || (sc.hostile[1] && flag_on(sc.hostile[1]));
            g_scenery_want[k] = hostile ? 0 : 1;
        }
        const uint32_t chr = frame - g_scenery_seen[k] <= 2 ? g_scenery_chr[k] : 0;
        if (g_scenery_fixed[k] && g_scenery_fixed[k] != chr) g_scenery_fixed[k] = 0;  // gone (its map unloaded)
        if (!chr) continue;
        const uint32_t state = *reinterpret_cast<const uint32_t*>(chr + 0xC);
        const bool active = state && *reinterpret_cast<const int32_t*>(state + 8) == 2;
        auto* flags = reinterpret_cast<uint8_t*>(chr + 0x1FD);
        // Enabled ones kept active are updated every frame (a disabled one the game made active itself keeps its rate).
        if (active && g_scenery_want[k] && chr_enabled(chr) && g_scenery_on == 1) {
            if (!g_scenery_fixed[k] && !(*flags & 1)) g_scenery_fixed[k] = chr;  // the bit is ours to take off
            if (g_scenery_fixed[k] == chr) {
                *flags = static_cast<uint8_t>(*flags | 1);
                *reinterpret_cast<int32_t*>(chr + 0xF4) = 0;
            }
        } else if (g_scenery_fixed[k] == chr) {
            *flags = static_cast<uint8_t>(*flags & ~1);  // the game's rate again (-1 while inactive)
            g_scenery_fixed[k] = 0;
        }
    }
}

// --- detail rules (see the top) ----------------------------------------------------------------------------------
namespace D = drawdist_detail;
constexpr uint32_t kPieceMan = kPieceManVar;
constexpr uint32_t kWorldChrMan = 0x0137D644;  // +0x3C the player, whose +0x29C is the floor record underfoot
constexpr size_t kDetPieces = sizeof(D::kPieces) / sizeof(D::kPieces[0]);
constexpr size_t kDetPairs = sizeof(D::kPairs) / sizeof(D::kPairs[0]);
constexpr size_t kDetStandins = sizeof(D::kStandins) / sizeof(D::kStandins[0]);
constexpr size_t kDetObjTwins = sizeof(D::kObjTwins) / sizeof(D::kObjTwins[0]);
constexpr uint32_t kObjMan = 0x0137D5F8;  // WorldObjManImp: +0x1C blocks, +0x20 entries 0xEC (+0x0C draw mask, +0x3C n,
                                          // +0x40 ObjIns 0xC0 each in the MSB's order: +0x14 groups, +0x6C/7C/8C x y z)
constexpr int kRolePieces = 512;  // pieces a map may have for the roles (the largest has 195)
constexpr uint32_t kOnes[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
constexpr uint32_t kZero[4] = {};

// What we wrote into a table piece's live groups: nothing (its own), zero (hidden) or all ones (drawn in place of a
// stand-in, from any room).
enum : uint8_t { kAsIs = 0, kHidden = 1, kForced = 2 };
uint8_t g_state[kDetPieces] = {};
// For occl.cpp (DrawMapPieces' depth bias and OcclusionCulling): per block and piece index, bit 0 = drawn from any room
// in a stand-in's place (counts as added), bit 1 = drawn in a stand-in's place at all (forced or by the rooms' masks:
// the occlusion code's own test of the player inside its box is left to holds() here; its box can hold the player
// with its surfaces metres away, and leaving it out opened a hole where the stand-in was, user report 2026-11-21:
// m2140B1 under the start of the Undead Burg bridge), bits 4-7 = depth level + 1 (0 = not in the table: the generic
// rule).
uint8_t g_role[kMaxBlocks][kRolePieces] = {};
uint32_t g_role_gen = 0;
volatile LONG g_detail_on = 1;  // RemoteControl `detailrules 0|1|2` (diagnostics A/B; 2: pairs of two maps left out)

// The live draw groups of a table piece, or null when its map is not loaded or does not match the table.
uint32_t* live_groups(const D::Piece& p) {
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

// A block's live draw mask (what is drawn of it, ours ORed in) and the game's own (as 0xDE2C60 wrote it; the live one
// before any was noted), or false when the block is not loaded.
bool block_masks(int block, uint32_t seen[4], uint32_t game[4]) {
    const uint32_t man = *reinterpret_cast<const uint32_t*>(kPieceMan);
    if (!man || block < 0) return false;
    const uint32_t blocks = *reinterpret_cast<const uint32_t*>(man + 0x20);
    const int count = *reinterpret_cast<const int*>(man + 0x1C);
    if (!blocks || block >= count) return false;
    std::memcpy(seen, reinterpret_cast<const void*>(blocks + block * 0x5C + 0x0C), 16);
    std::memcpy(game, block < g_game_draw_blocks ? g_game_draw[block] : seen, 16);
    return true;
}

const uint32_t* written(uint8_t state, const D::Piece& p) {
    return state == kHidden ? kZero : state == kForced ? kOnes : p.groups;
}

// Puts what we wrote back (game mode, rules off). A piece whose groups are no longer ours is left alone.
void restore_all() {
    for (size_t i = 0; i < kDetPieces; ++i) {
        if (g_state[i] == kAsIs) continue;
        const auto& p = D::kPieces[i];
        uint32_t* g = live_groups(p);
        if (g && groups_are(g, written(g_state[i], p))) std::memcpy(g, p.groups, 16);
        g_state[i] = kAsIs;
    }
}

// kObjTwins left out (1): an object of another room lying on a map piece drawn here (see the rules below). For occl.cpp
// (drawdist_object_hidden), which leaves the object's draws out; changes count in g_role_gen like the roles.
uint8_t g_objhide[kDetObjTwins] = {};
bool g_objhide_any = false;

void publish_roles(const uint8_t (&role)[kMaxBlocks][kRolePieces], const uint8_t (&objhide)[kDetObjTwins]) {
    bool changed = false;
    if (std::memcmp(role, g_role, sizeof(g_role)) != 0) {
        std::memcpy(g_role, role, sizeof(g_role));
        changed = true;
    }
    if (std::memcmp(objhide, g_objhide, sizeof(g_objhide)) != 0) {
        std::memcpy(g_objhide, objhide, sizeof(g_objhide));
        g_objhide_any = false;
        for (const uint8_t h : g_objhide) g_objhide_any = g_objhide_any || h != 0;
        changed = true;
    }
    if (changed) ++g_role_gen;
}

// The live object of a kObjTwins entry (its ObjIns), or 0 when its map is not loaded or does not match the table.
uint32_t live_object(const D::ObjTwin& o) {
    const uint32_t man = *reinterpret_cast<const uint32_t*>(kObjMan);
    if (!man) return 0;
    const int count = *reinterpret_cast<const int*>(man + 0x1C);
    const uint32_t blocks = *reinterpret_cast<const uint32_t*>(man + 0x20);
    if (!blocks || o.block >= count) return 0;
    const uint32_t blk = blocks + o.block * 0xEC;
    const int n = *reinterpret_cast<const int*>(blk + 0x3C);
    const uint32_t arr = *reinterpret_cast<const uint32_t*>(blk + 0x40);
    if (!arr || n != o.map_objects || o.order >= n) return 0;
    const uint32_t obj = arr + o.order * 0xC0;
    const auto* f = reinterpret_cast<const float*>(obj);
    const float dx = f[0x6C / 4] - o.pos[0], dy = f[0x7C / 4] - o.pos[1], dz = f[0x8C / 4] - o.pos[2];
    return dx * dx + dy * dy + dz * dz < 0.25f ? obj : 0;
}

// The player's position (physics: chr +0x28 controller, +0x1C physics, +0x10 x y z), or false.
bool player_at(uint32_t chr, float out[3]) {
    const uint32_t ctrl = chr ? *reinterpret_cast<const uint32_t*>(chr + 0x28) : 0;
    const uint32_t phys = ctrl ? *reinterpret_cast<const uint32_t*>(ctrl + 0x1C) : 0;
    if (!phys) return false;
    std::memcpy(out, reinterpret_cast<const void*>(phys + 0x10), 12);
    return true;
}

// Whether the piece's own surfaces come near the player: within its occupancy (2 m cells its triangles pass through and
// their neighbours, so about 2 to 4 m), or within its box where it has none. A piece's box can reach far past its
// surfaces: the lower Burg's m2140B1 holds the Undead Burg bridge's deck in its box, its nearest triangle 5 m away.
bool holds(const D::Piece& p, const float at[3]) {
    constexpr float m = 0.5f;
    if (!(at[0] > p.box[0] - m && at[0] < p.box[3] + m && at[1] > p.box[1] - m && at[1] < p.box[4] + m &&
          at[2] > p.box[2] - m && at[2] < p.box[5] + m)) {
        return false;
    }
    if (p.vox < 0) return true;
    int c[3];
    for (int k = 0; k < 3; ++k) {
        c[k] = static_cast<int>(std::floor((at[k] - p.vox_org[k]) / 2.0f));
        if (c[k] < 0 || c[k] >= p.vox_dim[k]) return false;
    }
    const uint32_t bit = (static_cast<uint32_t>(c[2]) * p.vox_dim[1] + static_cast<uint32_t>(c[1])) * p.vox_dim[0] +
                         static_cast<uint32_t>(c[0]);
    return (D::kVox[p.vox + (bit >> 3)] >> (bit & 7)) & 1;
}

bool g_detail_redo = true;  // the next tick works the rules out whatever its inputs (see the signature below)
volatile LONG g_floorlog = 0;  // diagnostics (RemoteControl `floorlog N`): the player's floor changes logged for N s
ULONGLONG g_floorlog_until = 0;

void detail_tick_body() {
    static uint8_t role[kMaxBlocks][kRolePieces];
    static uint8_t objhide[kDetObjTwins];
    if (g_mode == 0 || !g_detail_on) {
        std::memset(role, 0, sizeof(role));
        std::memset(objhide, 0, sizeof(objhide));
        restore_all();
        publish_roles(role, objhide);
        g_detail_redo = true;
        return;
    }
    const uint32_t wcm = *reinterpret_cast<const uint32_t*>(kWorldChrMan);
    const uint32_t chr = wcm ? *reinterpret_cast<const uint32_t*>(wcm + 0x3C) : 0;
    const uint32_t floor = chr ? *reinterpret_cast<const uint32_t*>(chr + 0x29C) : 0;
    if (!floor) return;  // loading or no floor yet: keep what is set
    float at[3] = {};
    const bool at_ok = player_at(chr, at);
    {
        static uint32_t last_floor = 0;
        static unsigned changes = 0;
        if (g_floorlog) {
            const ULONGLONG now = GetTickCount64();
            if (!g_floorlog_until) g_floorlog_until = now + static_cast<ULONGLONG>(g_floorlog) * 1000;
            if (floor != last_floor) {
                ++changes;
                const uint32_t own0 = *reinterpret_cast<const uint32_t*>(floor + 0x20);
                const uint32_t own1 = *reinterpret_cast<const uint32_t*>(floor + 0x24);
                LOG_INFO("[floorlog] floor %08X (block %d) own groups %08X %08X.. at %.2f %.2f %.2f", floor,
                         static_cast<int>(*reinterpret_cast<const int8_t*>(floor + 0xF)), own0, own1, at[0], at[1], at[2]);
            }
            if (now >= g_floorlog_until) {
                LOG_INFO("[floorlog] %u floor changes", changes);
                changes = 0;
                g_floorlog_until = 0;
                InterlockedExchange(&g_floorlog, 0);
            }
        }
        last_floor = floor;
    }
    // Nothing to do while nothing the rules read has changed (2026-11-21 night: every frame on the render thread, 0.5%
    // of it at the Undead Burg bridge, the tables then doubled): the mode, the player's floor and 0.5 m cell (holds():
    // occupancy cells are 2 m on one grid, the boxes' margins anywhere), and every loaded block's piece array and
    // masks (the live one with ours ORed in, the game's as noted). Made again every 30th frame regardless.
    {
        uint32_t h = 2166136261u;
        const auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
        mix(static_cast<uint32_t>(g_mode));
        mix(floor);
        if (at_ok) {
            for (int k = 0; k < 3; ++k) mix(static_cast<uint32_t>(static_cast<int>(std::floor(at[k] * 2.0f))));
        }
        const uint32_t man = *reinterpret_cast<const uint32_t*>(kPieceMan);
        const uint32_t blocks = man ? *reinterpret_cast<const uint32_t*>(man + 0x20) : 0;
        const int count = man ? *reinterpret_cast<const int*>(man + 0x1C) : 0;
        mix(blocks);
        mix(static_cast<uint32_t>(count));
        for (int b = 0; blocks && b < count && b < kMaxBlocks; ++b) {
            const uint32_t blk = blocks + b * 0x5C;
            mix(*reinterpret_cast<const uint32_t*>(blk + 0x30));
            mix(*reinterpret_cast<const uint32_t*>(blk + 0x2C));
            for (int k = 0; k < 4; ++k) mix(reinterpret_cast<const uint32_t*>(blk + 0x0C)[k]);
            if (b < g_game_draw_blocks) {
                for (int k = 0; k < 4; ++k) mix(g_game_draw[b][k]);
            }
        }
        static uint32_t last = 0;
        static int frames = 0;
        if (h == last && !g_detail_redo && ++frames < 30) return;
        last = h;
        frames = 0;
        g_detail_redo = false;
    }
    std::memset(role, 0, sizeof(role));
    // Each piece by its own map's masks: seen (the live draw mask, ours ORed in: what would be drawn) and own (the
    // game's own mask: what the game itself draws from here). The player's floor groups only number the player's map's
    // rooms.
    static uint32_t* live[kDetPieces];
    static bool seen[kDetPieces], own[kDetPieces], drawn[kDetPieces], forced[kDetPieces], covering[kDetPieces];
    static uint8_t level[kDetPieces];
    static uint8_t why[kDetPieces];  // why it is left out: 1 backdrop, 2 stand-in, 3 twin, 4 the same model twice,
                                     // 5 surfaces too far apart for a depth bias
    for (size_t i = 0; i < kDetPieces; ++i) {
        const auto& p = D::kPieces[i];
        live[i] = live_groups(p);
        seen[i] = own[i] = drawn[i] = forced[i] = covering[i] = false;
        level[i] = why[i] = 0;
        if (!live[i]) {
            g_state[i] = kAsIs;  // its map is gone (or not this build's): nothing of ours is left there
            continue;
        }
        if (!groups_are(live[i], written(g_state[i], p))) g_state[i] = kAsIs;  // rewritten by someone else: not ours
        uint32_t bseen[4], bgame[4];
        if (!block_masks(p.block, bseen, bgame)) {
            live[i] = nullptr;
            continue;
        }
        seen[i] = any_of(p.groups, bseen);
        own[i] = any_of(p.groups, bgame);
        // A coarse far backdrop only where the game draws it.
        drawn[i] = seen[i] && (own[i] || !(p.flags & D::kBackdrop));
        if (seen[i] && !drawn[i]) why[i] = 1;
    }
    // Stand-ins: hidden while the finer pieces covering them are drawn in their place (from any room). Not where the
    // game draws all of them itself (its own picture), nor while the player stands in one of those pieces' bounds (the
    // occlusion code leaves such a piece of another room out, as the game does). A piece drawn in place of one may be
    // a stand-in for finer ones in turn.
    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (size_t k = 0; k < kDetStandins; ++k) {
            const auto& st = D::kStandins[k];
            const size_t s = st.piece;
            if (!live[s] || !drawn[s]) continue;
            bool ok = true, all_own = own[s];
            for (int c = 0; c < st.count && ok; ++c) {
                const size_t w = D::kCover[st.first + c];
                const auto& wp = D::kPieces[w];
                ok = live[w] && (drawn[w] || own[w] || !(wp.flags & D::kBackdrop)) &&
                     !(at_ok && !own[w] && holds(wp, at));
                all_own = all_own && own[w];
            }
            if (!ok || all_own) continue;
            drawn[s] = false;
            why[s] = 2;
            changed = true;
            for (int c = 0; c < st.count; ++c) {
                const size_t w = D::kCover[st.first + c];
                covering[w] = true;
                if (!drawn[w]) drawn[w] = forced[w] = true;
            }
        }
        if (!changed) break;
    }
    // The same model placed twice (for rooms that differ in light or fog): one copy stays, the player's room's own if
    // it shows one, else one drawn in place of a stand-in, else the first.
    for (size_t k = 0; k < kDetPairs; ++k) {
        const auto& pr = D::kPairs[k];
        if (pr.winner != 2 || !drawn[pr.a] || !drawn[pr.b]) continue;
        if (own[pr.a] && own[pr.b] && !forced[pr.a] && !forced[pr.b]) continue;  // the game's own picture
        const auto keep = [&](size_t i) { return (own[i] && !forced[i] ? 2 : 0) + (forced[i] ? 1 : 0); };
        const size_t h = keep(pr.a) >= keep(pr.b) ? pr.b : pr.a;
        drawn[h] = forced[h] = false;
        why[h] = 4;
    }
    // Twins: two overlapping pieces with no draw group in common, versions of the same surfaces for different rooms
    // (the game never draws both). Unless a stand-in's whole replacement, they are not a coarse copy and a detailed
    // one but two tessellations of the same shape, which cut through each other where they are not in one plane
    // (user report 2026-11-21: the cliff and wall seen along the Undead Burg bridge, the game's m2000B1 and m2520B1
    // with m2380B1 of another room). One is drawn: the player's room's own, or of two others the finer.
    // The same for two pieces that share a room but whose surfaces run apart (Pair::apart, from the gap pass: within
    // 0.5 m of each other over much more area than they share in one plane): the depth levels below push the coarser
    // back by about 5 cm at 50 m at most, and a coarse version up to tens of centimetres off its detailed one still
    // showed through it, flickering as the camera moved (user report 2026-11-21 under the Undead Burg's bridge: the
    // room's own m2410B1 with m2020B1, m2030B1 and m2012B1 of other rooms; none of it with DrawMapPieces = game). And
    // for pairs only the gap pass found (no surface in one plane: crossing at an angle). In those two cases a piece is
    // left out only when enough of it lies on the other (the pair's bits), so a big piece is not dropped for a small
    // one near it; otherwise the depth levels stay. Only a piece the player's room does not draw is ever left out.
    for (size_t k = 0; k < kDetPairs; ++k) {
        const auto& pr = D::kPairs[k];
        if (pr.winner > 1 || !drawn[pr.a] || !drawn[pr.b]) continue;
        const auto& pa = D::kPieces[pr.a];
        const auto& pb = D::kPieces[pr.b];
        if (g_detail_on == 2 && pa.block != pb.block) continue;
        // Two maps' pieces (a map's coarse copy of its neighbour's structures) never share a room: each map numbers its
        // groups itself. Their pairs always carry bits 4 and 8 (gen_detail.py), so the depth levels order them unless
        // enough of one lies on the other to leave it out.
        const bool shared = pa.block == pb.block && any_of(pa.groups, pb.groups);
        const bool apart = (pr.apart & 4) != 0;
        if (shared && !apart) continue;                          // in one plane: the depth levels below
        if (covering[pr.a] || covering[pr.b]) continue;          // drawn in a stand-in's place: never left out
        const bool oa = own[pr.a] && !forced[pr.a], ob = own[pr.b] && !forced[pr.b];
        if (oa && ob) continue;                                  // the game's own picture
        const size_t h = oa ? pr.b : ob ? pr.a : (pr.winner ? pr.a : pr.b);
        if ((shared || (pr.apart & 8)) && !(pr.apart & (h == pr.a ? 1 : 2))) continue;  // too little of it on the other
        drawn[h] = false;
        why[h] = apart && (shared || (pr.apart & 8)) ? 5 : 3;
    }
    // Objects (DrawMapObjects): an object lying on a map piece it shares no room with (kObjTwins) is another room's
    // version of that structure, a centimetre or two off the piece's, and the two fight over the depth wherever both
    // are drawn (user report 2026-11-22 from the Undead Burg watchtower: the spire towers below, o1290, a room's object
    // brought in on m2380B1, the far version the watchtower's room draws; no flicker with DrawMapPieces = game). As for
    // the pieces, the player's room's own stays: an object it does not draw is left out while the piece is drawn
    // (occl.cpp skips its draws), and a piece it does not draw while the object is.
    std::memset(objhide, 0, sizeof(objhide));
    for (size_t k = 0; k < kDetObjTwins; ++k) {
        const auto& ot = D::kObjTwins[k];
        const size_t pi = ot.piece;
        if (pi >= kDetPieces || !live[pi] || !drawn[pi]) continue;
        const uint32_t obj = live_object(ot);
        if (!obj) continue;
        const uint32_t man = *reinterpret_cast<const uint32_t*>(kObjMan);
        const uint32_t oblk = *reinterpret_cast<const uint32_t*>(man + 0x20) + ot.block * 0xEC;
        uint32_t bseen[4], bgame[4];
        if (!block_masks(ot.block, bseen, bgame)) continue;
        const auto* g = reinterpret_cast<const uint32_t*>(obj + 0x14);
        if (!any_of(g, reinterpret_cast<const uint32_t*>(oblk + 0x0C))) continue;  // the object is not drawn
        if (any_of(g, D::kPieces[pi].groups)) continue;  // one room shows both: made together, the game's picture
        const bool oown = any_of(g, bgame), pown = own[pi] && !forced[pi];
        if (oown && pown) continue;  // the game's own picture
        if (oown) {
            if (covering[pi]) continue;  // drawn in a stand-in's place: never left out
            drawn[pi] = false;
            why[pi] = 6;
        } else {
            objhide[k] = 1;
        }
    }
    // Write what is wanted: hidden (drawn otherwise), drawn from any room, or the piece's own groups.
    for (size_t i = 0; i < kDetPieces; ++i) {
        if (!live[i]) continue;
        const auto& p = D::kPieces[i];
        const uint8_t want = forced[i] ? kForced : (!drawn[i] && seen[i]) ? kHidden : kAsIs;
        if (want == g_state[i]) continue;
        std::memcpy(live[i], written(want, p), 16);
        g_state[i] = want;
        static uint8_t told[kDetPieces] = {};  // the reasons logged (bits)
        if (want == kHidden && why[i] && !(told[i] & (1u << why[i]))) {
            told[i] = static_cast<uint8_t>(told[i] | (1u << why[i]));
            static const char* const kWhy[] = {"", "a far backdrop, left to the rooms the game draws it from",
                                               "hidden: finer pieces of the same surfaces drawn in its place",
                                               "hidden: another room's version of surfaces drawn here (no z-fighting)",
                                               "hidden: the same model drawn for this room",
                                               "hidden: another room's version crossing surfaces drawn here, too far apart "
                                               "for a depth bias",
                                               "hidden: an object drawn here is this room's version of its surfaces"};
            LOG_INFO("DrawMapPieces: %s (map %s), %s", p.name, p.map, kWhy[why[i]]);
        }
    }
    // Depth levels: where two overlapping pieces are both drawn (not both by the game itself), the coarser is drawn one
    // level farther than the finer. Start: the game's own pieces 0, added ones 1 (the generic rule).
    for (size_t i = 0; i < kDetPieces; ++i) level[i] = drawn[i] && (forced[i] || !own[i]) ? 1 : 0;
    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (size_t k = 0; k < kDetPairs; ++k) {
            const auto& pr = D::kPairs[k];
            if (pr.winner > 1 || !drawn[pr.a] || !drawn[pr.b]) continue;
            if (g_detail_on == 2 && D::kPieces[pr.a].block != D::kPieces[pr.b].block) continue;
            if (own[pr.a] && own[pr.b] && !forced[pr.a] && !forced[pr.b]) continue;  // the game's own picture
            const size_t w = pr.winner ? pr.b : pr.a, l = pr.winner ? pr.a : pr.b;
            if (level[l] <= level[w] && level[w] < 7) {
                level[l] = static_cast<uint8_t>(level[w] + 1);
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (size_t i = 0; i < kDetPieces; ++i) {
        const auto& p = D::kPieces[i];
        if (!live[i] || !drawn[i] || p.block >= kMaxBlocks || p.index >= kRolePieces) continue;
        role[p.block][p.index] =
            static_cast<uint8_t>((forced[i] ? 1 : 0) | (covering[i] ? 2 : 0) | ((level[i] + 1) << 4));
    }
    {
        static uint8_t told[kDetObjTwins] = {};  // logged once each
        for (size_t k = 0; k < kDetObjTwins; ++k) {
            if (!objhide[k] || told[k]) continue;
            told[k] = 1;
            const auto& ot = D::kObjTwins[k];
            LOG_INFO("DrawMapObjects: object #%u of map %s (at %.1f %.1f %.1f) left out: another room's version of %s, "
                     "drawn here", static_cast<unsigned>(ot.order), D::kPieces[ot.piece].map, ot.pos[0], ot.pos[1],
                     ot.pos[2], D::kPieces[ot.piece].name);
        }
    }
    publish_roles(role, objhide);
}

bool g_twins_on = false;
bool g_fx_on = false;  // EffectLodDistance: the four level checks call fx_ftol

// EffectLodDistance: all four sites checked before any is changed, and all four changed or none: if one cannot be
// written, the ones already changed get the game's call back.
bool install_fx() {
    for (const uint32_t site : kFxDistSites) {
        if (!calls(site, kFtol)) return false;
    }
    const size_t n = sizeof(kFxDistSites) / sizeof(kFxDistSites[0]);
    size_t done = 0;
    while (done < n && redirect_call(kFxDistSites[done], kFtol, reinterpret_cast<const void*>(&fx_ftol))) ++done;
    if (done == n) return true;
    for (size_t i = 0; i < done; ++i) {
        if (!redirect_call(kFxDistSites[i], reinterpret_cast<uint32_t>(&fx_ftol), reinterpret_cast<const void*>(kFtol))) {
            LOG_ERROR("EffectLodDistance: the level check at %08X could not be put back; its distance stays the game's "
                      "(scaled by 1)", static_cast<unsigned>(kFxDistSites[i]));
        }
    }
    return false;
}

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
        detail_tick_body();
        scenery_tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool drawdist_game_mask(int block, uint32_t out[4]) {
    if (block < 0 || block >= g_game_draw_blocks) return false;
    std::memcpy(out, g_game_draw[block], 16);
    return true;
}
uint32_t drawdist_game_mask_gen() { return g_game_draw_gen; }
uint8_t drawdist_piece_role(int block, int index) {
    if (block < 0 || block >= kMaxBlocks || index < 0 || index >= kRolePieces) return 0;
    return g_role[block][index];
}
uint32_t drawdist_role_gen() { return g_role_gen; }
bool drawdist_object_hidden(int block, int index) {
    if (!g_objhide_any) return false;
    for (size_t k = 0; k < kDetObjTwins; ++k) {
        if (g_objhide[k] && D::kObjTwins[k].block == block && D::kObjTwins[k].order == index) return true;
    }
    return false;
}
void drawdist_floorlog(int seconds) { InterlockedExchange(&g_floorlog, seconds); }
void drawdist_set_detail(int on) {
    InterlockedExchange(&g_detail_on, on);
    g_detail_redo = true;
}
bool drawdist_game_sfx_mask(int block, uint32_t out[4]) {
    if (block < 0 || block >= g_game_sfx_blocks) return false;
    std::memcpy(out, g_game_sfx[block], 16);
    return true;
}

void drawdist_set_scenery(int on) {
    InterlockedExchange(&g_scenery_on, on && calls(kChrTestSite, reinterpret_cast<uint32_t>(&chr_test)) ? (on == 2 ? 2 : 1) : 0);
}

void drawdist_set_mode(int mode) {
    if (mode < 0 || mode > 3) mode = 0;
    InterlockedExchange(&g_mode, mode);
    // The renderer's groups at once (the player's floor record: draw groups +0x10, display groups +0x20); the
    // streaming masks follow on the next frame, the draw masks when the floor is applied again (SetFloor 0xE6E580 sees
    // no floor yet on the next frame, and FieldArea's update rebuilds them).
    const uint32_t rend = g_rend;
    __try {
        const uint32_t wcm = *reinterpret_cast<uint32_t*>(kWorldChrMan);
        const uint32_t chr = wcm ? *reinterpret_cast<uint32_t*>(wcm + 0x3C) : 0;
        const uint32_t floor = chr ? *reinterpret_cast<uint32_t*>(chr + 0x29C) : 0;
        if (rend) {
            auto* groups = reinterpret_cast<uint32_t*>(rend + 0x48);
            if (mode == 2) {
                for (int i = 0; i < 4; ++i) groups[i] = 0xFFFFFFFFu;
            } else if (floor) {
                const auto* disp = reinterpret_cast<const uint32_t*>(floor + 0x20);
                uint32_t w[4] = {};
                if (mode != 0) widen_groups(floor + 8, w);
                for (int i = 0; i < 4; ++i) groups[i] = disp[i] | w[i];
            }
        }
        if (chr) *reinterpret_cast<uint32_t*>(chr + 0x29C) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void drawdist_set_widen(int mask) { InterlockedExchange(&g_widen, mask & 15); }

void drawdist_set_effect_lod(int percent) {
    if (!g_fx_on) {  // not installed: a site that could not be put back multiplies by 1
        g_fx_inv = 1.0f;
        return;
    }
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
    g_fx_on = install_fx();
    drawdist_set_effect_lod(effect_percent);
    if (!g_fx_on) {
        LOG_ERROR("EffectLodDistance: the effect level checks do not match this build; not used");
    } else if (effect_percent != 100) {
        LOG_INFO("EffectLodDistance: fires (torches, bonfires) keep each level of detail %d%% as far away as the "
                 "game's", effect_percent);
    }
    InterlockedExchange(&g_mode, (mode < 0 || mode > 3) ? 0 : mode);
    // All eight sites checked before any is changed: the renderer's groups are widened at two, the floors noted at two
    // and the widened managers' masks written at two.
    bool match = calls(kLoadSite, kGetDisp) && calls(kStepSite, kGetDisp) && calls(kDrawGetSite, kGetDisp) &&
                 calls(kStreamGetSite, kGetDisp) && calls(kBuildDrawSite, kBuildDraw) && calls(kStreamDoneSite, kStreamDone);
    for (const uint32_t site : kStreamFloorSites) match = match && calls(site, kStreamFloor);
    bool on = false;
    if (match) {
        on = redirect_call(kLoadSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_load)) &&
             redirect_call(kStepSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_step)) &&
             redirect_call(kDrawGetSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_draw)) &&
             redirect_call(kStreamGetSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_stream)) &&
             redirect_call(kBuildDrawSite, kBuildDraw, reinterpret_cast<const void*>(&build_draw));
        for (const uint32_t site : kStreamFloorSites) {
            on = on && redirect_call(site, kStreamFloor, reinterpret_cast<const void*>(&stream_floor));
        }
        on = on && redirect_call(kStreamDoneSite, kStreamDone, reinterpret_cast<const void*>(&stream_done));
    }
    g_lod_on = install_lod();
    g_twins_on = on;
    // Scenery characters (see above): the test site redirected whatever the mode (RemoteControl can switch to all).
    if (on && calls(kChrTestSite, kMaskAny) &&
        redirect_call(kChrTestSite, kMaskAny, reinterpret_cast<const void*>(&chr_test))) {
        InterlockedExchange(&g_scenery_on, 1);
    } else if (on) {
        LOG_ERROR("DrawMapPieces: the character test at %08X does not match this build; scenery characters keep the "
                  "game's rooms", static_cast<unsigned>(kChrTestSite));
    }
    if (!on) {
        InterlockedExchange(&g_mode, 0);
        LOG_ERROR("DrawMapPieces: the game's code at %08X/%08X does not match this build; not used",
                  static_cast<unsigned>(kBuildDrawSite), static_cast<unsigned>(kStreamDoneSite));
    } else if (mode == 1) {
        LOG_INFO("DrawMapPieces = adjacent: the map pieces seen from the player's room and from the rooms next to it "
                 "are drawn (the game: this room's only), with their effects and lights (objects: DrawMapObjects); "
                 "characters keep the game's rooms");
    } else if (mode == 2) {
        LOG_INFO("DrawMapPieces = all: every map piece of the loaded maps is drawn wherever the player stands; "
                 "with their effects and lights (objects: DrawMapObjects); characters keep the game's rooms");
    } else if (mode == 3) {
        LOG_INFO("DrawMapPieces = adjacent+: the map pieces seen from the player's room and from the rooms up to %d steps "
                 "away are drawn, with their effects and lights (objects: DrawMapObjects); characters keep the game's "
                 "rooms", kHops);
    }
    if (!g_lod_on) {
        LOG_ERROR("LodDistance: the game's code at %08X does not match this build; not used",
                  static_cast<unsigned>(kLodFn));
    } else if (lod_percent != 100) {
        LOG_INFO("LodDistance: models switch to their lower levels of detail %d%% as far away as the game's", lod_percent);
    }
    return on;
}
