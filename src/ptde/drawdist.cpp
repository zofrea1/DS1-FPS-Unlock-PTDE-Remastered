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
// The two options widen what the player's floor adds to its block's two masks, for what is only seen: the floor's
// contribution is noted as the game reads it (0xDE2C9F, 0xDE2AFA) and ORed into the masks of the map pieces, the map
// effects (WorldSfxManImp [0x137D6E4]: torches' fire, mist, embers) and the map's point lights
// (WorldMapPointLightManImp [0x137D6EC]) after the game has written them (after 0xDE2C60 returns; before 0xE9BD70 at
// 0xDE8198, right after the streaming masks are written); the renderer's own groups (GameRend+0x48, see kLoadSite) are
// widened the same way. Characters, objects (with their physics and events), collisions and sounds keep the game's
// own masks: the renderer draws only what is in the scene, so they are drawn where the game has them.
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
// Stand-ins. In nine maps some surfaces exist twice: a detailed piece drawn from the rooms that look at it up close
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
};
constexpr uint32_t kLodFn = 0x00F61CD0;
// EffectLodDistance: the effect level checks' call to _ftol on the camera distance (CheckLevel 0xA33600,
// ResetLevelSingle 0xA33720, ResetLevel 0xA33890, DistanceFromCamera 0xA48A00).
constexpr uint32_t kFtol = 0x00ABF1A0;
constexpr uint32_t kFxDistSites[] = {0x00A33700, 0x00A33820, 0x00A339C2, 0x00A48AAA};
constexpr uint8_t kLodHead[9] = {0x51, 0xF3, 0x0F, 0x10, 0x81, 0x50, 0x02, 0x00, 0x00};  // push ecx; movss xmm0,[ecx+250h]

volatile LONG g_mode = 0;  // DrawMapPieces: 0 game, 1 adjacent, 2 all
volatile LONG g_widen = 7;  // diagnostics (RemoteControl drawwiden): the widened managers, bit i = kWidened[i]
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
uint32_t g_extra_stream[kMaxBlocks][4] = {};
bool g_any_draw = false;
bool g_any_stream = false;
volatile LONG g_in_stream = 0;  // inside the streaming update's floor calls (0xDE2AE0 has other callers: load checks)
// 0x4569D0 leaves ecx (the floor record + 8) as it was: its draw groups (the neighbouring rooms) are at ecx + 8.
uint32_t g_rend = 0;  // GameRend ([ebx+0x10] at kLoadSite, [ebx+0x38] at kStepSite): +0x48 the groups it draws

// The renderer's groups widened in place (eax = the answer of 0x4569D0): the floor's draw groups added (adjacent), or
// every group (all). Only GameRend takes these two answers.
__declspec(naked) void get_disp_load() {
    __asm {
        push ecx
        mov ecx, dword ptr [ebx + 0x10]
        mov g_rend, ecx
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
        mov g_rend, ecx
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

void __cdecl apply_draw() {
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
        twins_tick_body();
        scenery_tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void drawdist_set_scenery(int on) {
    InterlockedExchange(&g_scenery_on, on && calls(kChrTestSite, reinterpret_cast<uint32_t>(&chr_test)) ? (on == 2 ? 2 : 1) : 0);
}

void drawdist_set_mode(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
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
                const auto* nbr = reinterpret_cast<const uint32_t*>(floor + 0x10);
                for (int i = 0; i < 4; ++i) groups[i] = disp[i] | (mode == 1 ? nbr[i] : 0u);
            }
        }
        if (chr) *reinterpret_cast<uint32_t*>(chr + 0x29C) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void drawdist_set_widen(int mask) { InterlockedExchange(&g_widen, mask & 7); }

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
    InterlockedExchange(&g_mode, (mode < 0 || mode > 2) ? 0 : mode);
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
                 "are drawn (the game: this room's only), with their effects and lights; characters and objects keep the "
                 "game's rooms");
    } else if (mode == 2) {
        LOG_INFO("DrawMapPieces = all: every map piece of the loaded maps is drawn wherever the player stands; "
                 "with their effects and lights; characters and objects keep the game's rooms");
    }
    if (!g_lod_on) {
        LOG_ERROR("LodDistance: the game's code at %08X does not match this build; not used",
                  static_cast<unsigned>(kLodFn));
    } else if (lod_percent != 100) {
        LOG_INFO("LodDistance: models switch to their lower levels of detail %d%% as far away as the game's", lod_percent);
    }
    return on;
}
