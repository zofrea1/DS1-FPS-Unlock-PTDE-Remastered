#include "occl.h"

#include "d3dperf.h"
#include "drawdist.h"
#include "passcull.h"

#include "fpsu_buildcfg.h"
#include "log.h"
#include "shaders/occl_box_ps.h"
#include "shaders/occl_box_vs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// OcclusionCulling: the map pieces DrawMapPieces adds are skipped while something in front hides them completely.
//
// The game decides which map pieces to draw by rooms: the floor underfoot names the groups of pieces seen from it, and
// that is its occlusion culling. DrawMapPieces adjacent/all add other rooms' pieces, and most of them are behind walls:
// at the Demon Ruins bonfire (all, facing Lost Izalith) 463 of the 518 opaque draws had no visible sample, and the
// pieces all adds were drawn twice (depth pre-pass and colour) for nothing, 1.3 ms of the render thread's 4.7.
//
// The render thread draws the scene from ordering tables: FFD3C0 walks a table's lists (the calls at 0xFFD46D and
// 0xFFD49D), FCB990 a depth-sorted one (0xFCBA03, 0xFCBA87). Each node is (next, object, arg); the object's slot 10
// (+0x28) draws it, thiscall(context, arg), ret 8. A model is a FrpgModelDrawEntity (vtable 0x1171A54): its world box
// at +0xA0 (min) and +0xB0 (max), its FlverModel at +0x40 (for a map piece, the one its MapModel holds: piece entry
// +0x34 MapModel, +0x14 FlverModel). The context (HGDrawContext) holds the pass's camera at +0x100 (HGCameraEntity:
// +0x40 its position, the origin the scene is drawn relative to; +0x90 the projection). The main camera draws the depth
// pre-pass (with the game's anti-aliasing), then the colour pass (opaque, then translucent) and a low-resolution pass;
// the shadow cascades (orthographic) and the water's mirror have cameras of their own and are left alone.
//
// The four call sites go through a stub. For a map piece drawn by the main camera that DrawMapPieces adds (its draw
// groups do not meet the player's room's), the stub:
//  - hides it (skips its draws) while it is known hidden;
//  - otherwise, in the colour pass, wraps its draws in an occlusion query: when its own drawing showed no visible
//    sample in two frames in a row, it is hidden from then on. Its real geometry decides, so a piece the game draws
//    through walls, or whose geometry leaves its box (swaying foliage), is never hidden while seen;
//  - while hidden, its world box (grown for the camera's movement) is drawn against the depth right after the depth
//    pre-pass (which then holds everything drawn), colour writes off, in an occlusion query: any visible sample shows
//    the piece again; and every eighth frame (staggered) it is drawn for real once to check (a box can be wrong).
// Results come back one frame later with Direct3D 9, three with DXVK, so a piece the camera reveals by moving can
// appear that much late; the box grows by how far the edge of the nearest occluder (the player, ~2.5 m from the
// camera) sweeps across the piece meanwhile. A box that found nothing in the frame its piece's own drawing was seen
// does not hold it (foliage swaying past it...): that piece is never hidden again this session (remembered by its
// box, so also after the map loads again). Never hidden either: with the camera in or within a metre of its grown box
// (from inside, only the far faces are drawn, behind the piece's own walls), in the frame the camera jumped (a cut, a
// warp), or without fresh evidence (four frames).
// Checked with RemoteControl `occl 5` (audit: what would be skipped is drawn and its samples counted) while walking,
// strafing and turning, 2026-11-20: ~1.9 million skipped draws in Blighttown, Demon Ruins, Quelaag's Domain and
// Firelink had 33,000 visible samples in all (DXVK at 4K with 4x anti-aliasing: under 10,000 pixels, single frames).
//
// Pieces from inside (DrawMapPieces adjacent/all, with or without OcclusionCulling): a piece the player's room does
// not show, whose world box holds the player, is not drawn by any camera. Such a piece is part of another room that
// reaches into this one: at the bottom of Havel's tower in the Undead Burg, Darkroot Basin's ground (m12_00 m1790B0,
// 57,000 vertices, its box -15..137 x -96..27 x -196..-28) passes through the tower at waist height, and the game
// draws it only from Darkroot's side (user report with screenshots, 2026-11-20; reproduced: drawn with adjacent, not
// with game). Leaving such pieces out only ever brings the picture back toward the game's own: what the player's room
// shows is never touched. A model that is also drawn by a piece of the player's room is never left out.
//
// The map effects DrawMapPieces adds (adjacent/all widen the map effects' groups too: torches' fire, mist, embers of
// other rooms) are skipped the same way while hidden: on the Undead Burg bridge with all, 62 of the 79 effects drawn
// were added ones, none of them visible from there (frozen frames with and without them identical), and they cost
// about 0.9 ms of the render thread a frame (the effects library updates and builds an effect when it draws it). An
// effect counts as added when its box holds the position of a map effect whose groups do not meet the game's own mask
// of its map (WorldSfxManImp entries: 0x34 bytes, groups at +0, [+0x20]+0 the position) and no position of one that
// does: effects of characters and the game's own map effects are never skipped (an effect can light what is around
// it; a hidden torch of the player's room lighting a visible wall stays as it is). The evidence is its box alone,
// grown by 1 m and half its largest side (its particles' quads), at most 10 m, and for the camera's movement as a
// piece's: the effects library draws its particles in batches after the effect's own draw call, so a query around
// that call counts nothing (8,700 counted, none with samples, visible effects included); hidden after two box tests in
// a row found nothing (tested in the next frame's tests: effects are drawn after them), shown at the first that finds
// something, drawn for real every eighth frame. An effect not drawn is not updated either, as when the game itself
// does not draw it (out of its rooms). Checked 2026-11-21: frozen frames with and without it in 8 directions at the
// bridge, the Demon Ruins and Blighttown no further apart than the same picture twice; bridge 180.7 -> 204.9 FPS.
//
// Depth bias (DrawMapPieces adjacent/all, with or without OcclusionCulling): a piece only DrawMapPieces adds is drawn a
// little farther in depth (D3DRS_DEPTHBIAS / SLOPESCALEDEPTHBIAS for its draw, perspective cameras only: not the
// shadow maps), so where it covers the same surfaces as a piece the game draws from this room, the game's wins instead
// of the two fighting over the depth. Under the Undead Burg's bridge (user report with screenshot, 2026-11-20) the game
// draws its coarse bridge m2410B1 and all added the detailed m2010B1: 721 m^2 of their faces lie in the same planes
// (and more within centimetres), and they flickered from below. They share a draw group, so the stand-in table (which
// pairs pieces with none in common) did not pair them; the bias covers every such overlap without hiding the added
// piece's other surfaces. The two are not exactly coplanar: the detailed one weaves a few centimetres in and out of the
// coarse one, so the bias is sized per draw to move the piece about 10 cm back at its nearest point (depth bias =
// near * 0.1 / distance^2, the near plane 5 cm), at least 4 steps of a 24-bit depth buffer and at most 2e-6: a bias in
// depth moves a surface by an amount that grows with the square of its distance, so one value for all (2e-6, which
// cleared the bridge at 30-60 m) moved Lost Izalith's backdrop seen from the Demon Ruins by metres. Measured at the
// bridge (frozen frame, pixels differing from the game's own picture by more than 4 of 255): 748,000 without the bias,
// 106,000 with it (the added piece's real extra surfaces: its outline, the window openings). The values before the
// draw are put back after it, unless the draw itself set others.
// Since the detail rules (drawdist.cpp, 2026-11-21) the bias comes in levels: where the tables know two overlapping
// pieces, the coarser is drawn one level farther than the finer whoever's room shows it (under the bridge the detailed
// m2010B1 now wins over the game's coarse m2410B1); a level multiplies both biases. Pieces the tables do not know keep
// the rule above (added ones one level back).
// Tried 2026-11-21 (night), kept for diagnostics only (RemoteControl `zvp C [STEP]`, off by default): a level moving
// the draw's depth range (the viewport's MinZ = C + STEP * (level - 1), DEPTHBIAS 4 steps of a 24-bit buffer a
// level): depth = z + MinZ * (1 - z), about MinZ of the distance at every pixel, the same for every piece of a level
// wherever its box is. Against the DEPTHBIAS way (frozen frames at 14 Undead Burg spots): no fewer fighting pixels,
// and at 0.1% to 0.3% of the distance a doorway's recess in a detailed wall 120 m away was covered by the coarse wall
// drawn with it, which the DEPTHBIAS way (growing with the square of the distance, about 1 m there) pushes far enough
// back. The DEPTHBIAS way stays.
namespace {

constexpr uint32_t kEntityVt = 0x01171A54;
constexpr uint32_t kPieceManVar = 0x0137D5F4;  // WorldMapManImp: +0x1C blocks, +0x20 entries 0x5C (+0x2C n, +0x30 pieces)
constexpr uint32_t kWorldChrMan = 0x0137D644;  // +0x3C the player, +0x29C the floor record (+0x20 its display groups)

struct Site {
    uint32_t at;       // mov edx, [ecx] / mov edx, [edx+28h] / push eax / push CTX / call edx
    uint8_t ctx_push;  // 0x57 push edi, 0x55 push ebp
};
constexpr Site kSites[] = {{0x00FFD466, 0x57}, {0x00FFD496, 0x57}, {0x00FCB9FC, 0x55}, {0x00FCBA80, 0x55}};

volatile LONG g_mode = 0;  // 0 off, 1 the pieces DrawMapPieces adds, 2 every map piece; +4 audit (diagnostics)
bool g_installed = false;
volatile LONG g_inside = 0;  // DrawMapPieces' pieces holding the player are left out (see the top)
// The player's position (game thread, each frame), read by the render thread: x, y + 1 m (the body, not the feet), z.
volatile float g_player[3] = {};
volatile LONG g_player_ok = 0;
volatile LONG g_st_inside = 0;  // draws left out so (the per-frame line)
volatile LONG g_st_inside_logged = 0;

// ---- the map pieces' models (game thread builds, render thread reads) ----
struct ModelSlot {
    uint32_t key;    // the FlverModel
    uint32_t flags;  // 1 a map piece, 2 a piece of it not shown by the player's own room, 4 one shown by it
};
constexpr int kModelBits = 14;
constexpr int kModelSlots = 1 << kModelBits;
ModelSlot g_models[2][kModelSlots];
volatile LONG g_models_cur = -1;
int g_game_ticks = 0;
uint32_t g_built_gen = 0;  // drawdist_game_mask_gen() the table was built from
uint32_t g_built_role_gen = 0;  // drawdist_role_gen() likewise
// Model flag: drawn in place of a stand-in (DrawMapPieces' detail rules), whose test of the player inside it was made
// against its surfaces there (drawdist.cpp's holds()), not its box.
constexpr uint32_t kNoInside = 0x10000u;
// Model flag: an object's model (WorldObjManImp), not a map piece's: culled like one, but never given the depth bias
// (an added ladder against a wall the game draws must not sink into it).
constexpr uint32_t kObject = 0x20000u;
// While the table is built only: an added piece of this model without kNoInside (one that still needs the inside rule).
constexpr uint32_t kNeedsInside = 0x40000u;
// Model flag of objects: an added object of this model that the detail rules leave out (another room's version of a
// map piece or object drawn here, drawdist_object_hidden). Its draws are told from the model's other objects by their
// world box holding that object's position (g_hidden_objs).
constexpr uint32_t kObjTwin = 0x80000u;
// The objects left out so (positions, with the model table: double-buffered the same way).
constexpr int kMaxHiddenObjs = 32;
struct HiddenObjs {
    int n;
    uint32_t model[kMaxHiddenObjs];
    float pos[kMaxHiddenObjs][3];
};
HiddenObjs g_hidden_objs[2] = {};
constexpr uint32_t kObjManVar = 0x0137D5F8;  // WorldObjManImp: +0x1C blocks, +0x20 entries 0xEC (+0x3C n, +0x40 objects)

inline uint32_t hash_ptr(uint32_t p, int bits) { return ((p >> 3) * 2654435761u) >> (32 - bits); }

void build_fx_positions();

void build_models() {
    const int back = g_models_cur == 0 ? 1 : 0;
    ModelSlot* t = g_models[back];
    std::memset(t, 0, sizeof(g_models[0]));
    HiddenObjs& hid = g_hidden_objs[back];
    hid.n = 0;
    const uint32_t gen = drawdist_game_mask_gen();
    const uint32_t role_gen = drawdist_role_gen();
    __try {
        const uint32_t man = *reinterpret_cast<const uint32_t*>(kPieceManVar);
        const int blocks = man ? *reinterpret_cast<const int*>(man + 0x1C) : 0;
        const uint32_t barr = man ? *reinterpret_cast<const uint32_t*>(man + 0x20) : 0;
        for (int b = 0; b < blocks && b < 64 && barr; ++b) {
            const uint32_t blk = barr + b * 0x5C;
            const int n = *reinterpret_cast<const int*>(blk + 0x2C);
            const uint32_t arr = *reinterpret_cast<const uint32_t*>(blk + 0x30);
            if (!arr || n <= 0 || n > 8192) continue;
            // Shown by the player's room: by the game's own draw mask of this piece's map (each map numbers its
            // groups itself; the floor's groups only fit the player's map). The live mask when none was noted.
            uint32_t own[4];
            if (!drawdist_game_mask(b, own)) std::memcpy(own, reinterpret_cast<const void*>(blk + 0x0C), 16);
            for (int i = 0; i < n; ++i) {
                const uint32_t e = arr + i * 0x50;
                const uint32_t mm = *reinterpret_cast<const uint32_t*>(e + 0x34);
                const uint32_t fl = mm ? *reinterpret_cast<const uint32_t*>(mm + 0x14) : 0;
                if (!fl) continue;
                const auto* g = reinterpret_cast<const uint32_t*>(e + 0x08);
                // DrawMapPieces' detail rules: a piece drawn in place of a stand-in counts as added (its groups are
                // all ones then), and the depth level + 1 they chose (bits 8-11 of the flags; 0: the generic rule).
                const uint8_t role = drawdist_piece_role(b, i);
                // A piece with no groups is never drawn (one the detail rules hide: a stand-in, the other copy of a
                // model placed twice): it says nothing about the model's draws (Grok 1aa07756: the hidden copy of a
                // same-model pair marked the kept one, the game's own, as added).
                if (!(role & 1) && !(g[0] | g[1] | g[2] | g[3])) continue;
                const bool extra = (role & 1) || !((g[0] & own[0]) | (g[1] & own[1]) | (g[2] & own[2]) | (g[3] & own[3]));
                // The model's flags hold every drawn piece of it (several pieces can share a model): its depth level +
                // 1 is the lowest any of them needs (the generic rule's: an added piece 1, the game's own 0; ORing the
                // codes made level 0 and 1 read as 2), and kNoInside only while no added piece of it needs the rule.
                const uint32_t code = (role >> 4) ? static_cast<uint32_t>(role >> 4) : (extra ? 2u : 1u);
                uint32_t h = hash_ptr(fl, kModelBits);
                for (int probe = 0; probe < kModelSlots; ++probe, h = (h + 1) & (kModelSlots - 1)) {
                    if (t[h].key == 0 || t[h].key == fl) {
                        t[h].key = fl;
                        const uint32_t old = (t[h].flags >> 8) & 0xF;
                        t[h].flags |= 1u | (extra ? 2u : 4u) | ((role & 2) ? kNoInside : (extra ? kNeedsInside : 0u));
                        if (old == 0 || code < old) t[h].flags = (t[h].flags & ~0xF00u) | (code << 8);
                        break;
                    }
                }
            }
        }
        // The objects (DrawMapPieces adjacent/all load and draw the objects of the rooms they add, see drawdist.cpp):
        // WorldObjManImp's blocks (0xEC each, the map piece manager's order) list their ObjIns (+0x3C count, +0x40
        // the array, 0xC0 each, in the MSB's order): +0x14 its draw groups as live, +0x90 its ObjModel, whose +0x14 is
        // the FlverModel its draws carry (as a map piece's MapModel). Added when its groups do not meet the game's own
        // mask of its map (the same mask the game gives every manager of a block).
        const uint32_t oman = *reinterpret_cast<const uint32_t*>(kObjManVar);
        const int oblocks = oman ? *reinterpret_cast<const int*>(oman + 0x1C) : 0;
        const uint32_t oarr = oman ? *reinterpret_cast<const uint32_t*>(oman + 0x20) : 0;
        for (int b = 0; b < oblocks && b < 64 && oarr; ++b) {
            const uint32_t blk = oarr + b * 0xEC;
            const int n = *reinterpret_cast<const int*>(blk + 0x3C);
            const uint32_t arr = *reinterpret_cast<const uint32_t*>(blk + 0x40);
            if (!arr || n <= 0 || n > 4096) continue;
            uint32_t own[4];
            if (!drawdist_game_mask(b, own)) std::memcpy(own, reinterpret_cast<const void*>(blk + 0x0C), 16);
            for (int i = 0; i < n; ++i) {
                const uint32_t o = arr + i * 0xC0;
                const uint32_t om = *reinterpret_cast<const uint32_t*>(o + 0x90);
                const uint32_t fl = om ? *reinterpret_cast<const uint32_t*>(om + 0x14) : 0;
                if (!fl) continue;
                const auto* g = reinterpret_cast<const uint32_t*>(o + 0x14);
                const bool extra = !((g[0] & own[0]) | (g[1] & own[1]) | (g[2] & own[2]) | (g[3] & own[3]));
                uint32_t twin = 0;
                if (extra && drawdist_object_hidden(b, i) && hid.n < kMaxHiddenObjs) {
                    const auto* f = reinterpret_cast<const float*>(o);
                    hid.model[hid.n] = fl;
                    hid.pos[hid.n][0] = f[0x6C / 4];
                    hid.pos[hid.n][1] = f[0x7C / 4];
                    hid.pos[hid.n][2] = f[0x8C / 4];
                    ++hid.n;
                    twin = kObjTwin;
                }
                uint32_t h = hash_ptr(fl, kModelBits);
                for (int probe = 0; probe < kModelSlots; ++probe, h = (h + 1) & (kModelSlots - 1)) {
                    if (t[h].key == 0 || t[h].key == fl) {
                        t[h].key = fl;
                        // Objects keep level 0 (never pushed back): code 1.
                        t[h].flags = (t[h].flags & ~0xF00u) | 1u | (extra ? 2u : 4u) | kObject | twin | (1u << 8);
                        break;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;  // keep the table in use
    }
    for (int k = 0; k < kModelSlots; ++k) {
        if (t[k].flags & kNeedsInside) t[k].flags &= ~(kNoInside | kNeedsInside);
    }
    g_built_gen = gen;
    g_built_role_gen = role_gen;
    InterlockedExchange(&g_models_cur, back);
    build_fx_positions();
}

// ---- the map effects' positions (game thread builds with the model table, render thread reads) ----
struct FxPos {
    float p[3];
    uint8_t added;  // groups not meeting the game's own mask of its map (DrawMapPieces' widening shows it)
};
constexpr int kFxPosMax = 1024;
FxPos g_fxpos[2][kFxPosMax];
int g_fxpos_n[2] = {};
volatile LONG g_fxpos_cur = -1;

void build_fx_positions() {
    const int back = g_fxpos_cur == 0 ? 1 : 0;
    int n = 0;
    __try {
        const uint32_t man = *reinterpret_cast<const uint32_t*>(0x0137D6E4);
        const int blocks = man ? *reinterpret_cast<const int*>(man + 0x1C) : 0;
        const uint32_t barr = man ? *reinterpret_cast<const uint32_t*>(man + 0x20) : 0;
        for (int b = 0; b < blocks && b < 64 && barr; ++b) {
            const uint32_t blk = barr + b * 0x3C;
            const int cnt = *reinterpret_cast<const int*>(blk + 0x2C);
            const uint32_t arr = *reinterpret_cast<const uint32_t*>(blk + 0x30);
            if (!arr || cnt <= 0 || cnt > 4096) continue;
            const auto* live = reinterpret_cast<const uint32_t*>(blk + 0x1C);
            uint32_t game[4];
            if (!drawdist_game_sfx_mask(b, game)) std::memcpy(game, live, 16);
            for (int i = 0; i < cnt && n < kFxPosMax; ++i) {
                const uint32_t e = arr + i * 0x34;
                const auto* g = reinterpret_cast<const uint32_t*>(e);
                if (!((g[0] & live[0]) | (g[1] & live[1]) | (g[2] & live[2]) | (g[3] & live[3]))) continue;
                const uint32_t obj = *reinterpret_cast<const uint32_t*>(e + 0x20);
                if (!obj) continue;
                const auto* p = reinterpret_cast<const float*>(obj);
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
                if (std::fabs(p[0]) < 1.5f && std::fabs(p[1]) < 1.5f && std::fabs(p[2]) < 1.5f) continue;  // no place
                FxPos& f = g_fxpos[back][n++];
                std::memcpy(f.p, p, 12);
                f.added = ((g[0] & game[0]) | (g[1] & game[1]) | (g[2] & game[2]) | (g[3] & game[3])) ? 0 : 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    g_fxpos_n[back] = n;
    InterlockedExchange(&g_fxpos_cur, back);
}

// Diagnostics (`occlfx 100+N`): the effects taken for added map effects, each logged once with the map effect matched.
volatile LONG g_fxlog_left = 0;
uint32_t g_fxlog_ents[256] = {};
int g_fxlog_n = 0;
void fxlog(uint32_t entity, const float* bb, const float* box) {
    for (int i = 0; i < g_fxlog_n; ++i) {
        if (g_fxlog_ents[i] == entity) return;
    }
    if (g_fxlog_n >= 256) return;
    g_fxlog_ents[g_fxlog_n++] = entity;
    InterlockedDecrement(&g_fxlog_left);
    const LONG cur = g_fxpos_cur;
    float near_d = -1.0f, mp[3] = {};
    if (cur >= 0) {
        const float c[3] = {(bb[0] + bb[4]) * 0.5f, (bb[1] + bb[5]) * 0.5f, (bb[2] + bb[6]) * 0.5f};
        for (int i = 0, n = g_fxpos_n[cur]; i < n; ++i) {
            const float* q = g_fxpos[cur][i].p;
            if (q[0] < box[0] || q[0] > box[3] || q[1] < box[1] || q[1] > box[4] || q[2] < box[2] || q[2] > box[5]) continue;
            const float d = std::sqrt((q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) +
                                      (q[2] - c[2]) * (q[2] - c[2]));
            if (near_d < 0.0f || d < near_d) {
                near_d = d;
                std::memcpy(mp, q, 12);
            }
        }
    }
    LOG_INFO("[occlfx] added effect %08X (fx %08X): box %.1f %.1f %.1f .. %.1f %.1f %.1f; map effect at %.1f %.1f %.1f, "
             "%.1f m from its centre", entity, *reinterpret_cast<const uint32_t*>(entity + 0x30), bb[0], bb[1], bb[2],
             bb[4], bb[5], bb[6], mp[0], mp[1], mp[2], near_d);
}

// An effect's (grown) box holds an added map effect's position and none of the game's own.
bool fx_added(const float* box) {
    const LONG cur = g_fxpos_cur;
    if (cur < 0) return false;
    bool added = false;
    const FxPos* t = g_fxpos[cur];
    for (int i = 0, n = g_fxpos_n[cur]; i < n; ++i) {
        const float* p = t[i].p;
        if (p[0] < box[0] || p[0] > box[3] || p[1] < box[1] || p[1] > box[4] || p[2] < box[2] || p[2] > box[5]) continue;
        if (!t[i].added) return false;
        added = true;
    }
    return added;
}

// Whether this draw is of an object the detail rules leave out: its model's, its world box holding the object's position
// (within half a metre).
bool hidden_object(uint32_t entity) {
    const LONG cur = g_models_cur;
    if (cur < 0) return false;
    const HiddenObjs& hid = g_hidden_objs[cur];
    const uint32_t fl = *reinterpret_cast<const uint32_t*>(entity + 0x40);
    const auto* b = reinterpret_cast<const float*>(entity + 0xA0);  // min x y z w, max x y z w
    for (int k = 0; k < hid.n; ++k) {
        if (hid.model[k] != fl) continue;
        const float* p = hid.pos[k];
        if (p[0] > b[0] - 0.5f && p[0] < b[4] + 0.5f && p[1] > b[1] - 0.5f && p[1] < b[5] + 0.5f && p[2] > b[2] - 0.5f &&
            p[2] < b[6] + 0.5f) {
            return true;
        }
    }
    return false;
}

uint32_t model_flags(uint32_t fl) {
    const LONG cur = g_models_cur;
    if (cur < 0 || !fl) return 0;
    const ModelSlot* t = g_models[cur];
    uint32_t h = hash_ptr(fl, kModelBits);
    for (int probe = 0; probe < 64; ++probe, h = (h + 1) & (kModelSlots - 1)) {
        if (t[h].key == fl) return t[h].flags;
        if (t[h].key == 0) return 0;
    }
    return 0;
}

// ---- render thread ----
struct Ent {
    uint32_t ent;
    float box[6];
    int seen;        // the last frame the main camera drew (or skipped) it
    int evidence;    // the frame of the latest result that kept it hidden (or of the one that hid it)
    int shown;       // the frame of the latest result that showed it (a test or its own drawing)
    int zero_run;    // frames in a row whose own drawing in the colour pass showed nothing
    int box_zero;    // the frame of the latest box test that found nothing (a piece about to be hidden is tested too)
    int box_frame;   // the frame of the latest box test's result, and whether it found anything
    uint8_t box_seen;
    uint8_t unreliable;  // its box said nothing while its own drawing was seen in the same frame: never hidden
    int acc_frame;   // the frame `acc` sums its own drawing's samples for (results in)
    uint32_t acc;
    uint8_t hidden;
    uint8_t extra;   // diagnostics: not the player's room's
    uint8_t boxonly; // its box tests alone hide it (no query around its own drawing): effects, mirror readers
    uint8_t pad;
};
constexpr int kEntBits = 13;
constexpr int kEntSlots = 1 << kEntBits;
Ent g_ents[kEntSlots];
int g_ent_used = 0;

Ent* find_ent(uint32_t e) {
    uint32_t h = hash_ptr(e, kEntBits);
    for (int probe = 0; probe < kEntSlots; ++probe, h = (h + 1) & (kEntSlots - 1)) {
        if (g_ents[h].ent == e) return &g_ents[h];
        if (g_ents[h].ent == 0) {
            if (g_ent_used > kEntSlots * 3 / 4) {  // full: start over (everything is drawn until known hidden again)
                std::memset(g_ents, 0, sizeof(g_ents));
                g_ent_used = 0;
                h = hash_ptr(e, kEntBits);
            }
            ++g_ent_used;
            std::memset(&g_ents[h], 0, sizeof(Ent));
            g_ents[h].ent = e;
            g_ents[h].seen = g_ents[h].evidence = g_ents[h].shown = g_ents[h].acc_frame = -1000;
            g_ents[h].box_zero = g_ents[h].box_frame = -1000;
            return &g_ents[h];
        }
    }
    return nullptr;
}

constexpr float kMaxSide = 150.0f;
volatile LONG g_vista_cap = 0;  // diagnostics (occlvista 1): vistas left alone
constexpr int kStale = 4;       // frames a hidden piece's evidence stays good
constexpr int kProbeEvery = 8;  // a hidden piece is drawn for real every this many frames
constexpr int kBatches = 6;
constexpr int kMaxRecs = 8192;
enum : uint8_t { kBoxTest = 0, kOwnDraw = 1, kAuditDraw = 2 };
struct Batch {
    int frame;
    int n;
    bool pending;
    uint8_t kind[kMaxRecs];
    uint32_t ent[kMaxRecs];
    Ent* slot[kMaxRecs];
    float dist[kMaxRecs];
    IDirect3DQuery9* q[kMaxRecs];
};
Batch g_batches[kBatches];

IDirect3DDevice9* g_dev = nullptr;
IDirect3DVertexShader9* g_vs = nullptr;
IDirect3DPixelShader9* g_ps = nullptr;
IDirect3DVertexDeclaration9* g_decl = nullptr;
bool g_dev_failed = false;
using DrawUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
DrawUpFn g_draw_up = nullptr;  // the device's own DrawPrimitiveUP, from before any hook

// What the device is drawing into (the game's surfaces, as the game set them).
using SetRtFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*);
SetRtFn o_set_rt = nullptr;
SetDsFn o_set_ds = nullptr;
IDirect3DSurface9* g_rt0 = nullptr;
uint32_t g_rt0_area = 0;
IDirect3DSurface9* g_ds = nullptr;

int g_frame = 0;
uint32_t g_main_cam = 0;  // the main camera (the perspective camera of the largest target, last frame)
uint32_t g_best_cam = 0, g_best_area = 0;
IDirect3DSurface9* g_first_rt = nullptr;  // the main camera's first target this frame (the depth pre-pass)
IDirect3DSurface9* g_first_ds = nullptr;
IDirect3DSurface9* g_color_rt = nullptr;  // its target after the depth pre-pass (the colour pass)
bool g_tested = false;                    // the depth pre-pass is over this frame (boxes tested, or no depth to test)
bool g_color_ok = false;                  // the colour pass draws into the depth the pre-pass made
float g_cam_pos[3] = {};
float g_last_pos[3] = {};
float g_cam_step = 0.0f;  // how far the camera moved since the previous frame (m)
int g_jump_frame = -1000;
Ent* g_hidden_cands[kMaxRecs];  // hidden pieces seen in the depth pre-pass (to box-test)
// Effects are drawn after the frame's box tests: their candidates wait for the next frame's (two lists, by parity).
constexpr int kFxCandMax = 1024;
Ent* g_fx_cands[2][kFxCandMax];
int g_fx_ncands[2] = {};
int g_fx_list = 0;
volatile LONG g_st_fx_skips = 0, g_st_fx_added = 0;
volatile LONG g_refl_unused = 1, g_st_refl_unused = 0;
LONG g_st_objtwin = 0;  // added objects' draws left out by the detail rules (another room's version of a piece)
unsigned long g_st_readers = 0;
bool g_wrap_active = false;  // one of ours is counting (render thread)
uint32_t g_cur_ent = 0, g_cur_ctx = 0, g_cur_flags = 0;
volatile LONG g_readlog = 0;
Ent* g_show_ents[kMaxRecs];     // diagnostics (occlshow 1): every candidate seen in the pre-pass, boxes drawn
int g_nshow = 0;
volatile LONG g_show = 0;
int g_nhidden = 0;
IDirect3DQuery9* g_wrap_q = nullptr;  // the query the stub wraps the next draw in
volatile LONG g_zbias = 0;            // the depth bias for the pieces only DrawMapPieces adds (see the top)
float g_zbias_world = 0.1f;           // metres the piece moves back at its nearest point (RemoteControl `zbias 1 M MAX`)
float g_zbias_max = 2.0e-6f;          // the most D3DRS_DEPTHBIAS added
constexpr float kZbiasMin = 4.0f / 16777216.0f;  // the least: 4 steps of a 24-bit depth buffer
constexpr float kZbiasSlope = 1.0f;   // D3DRS_SLOPESCALEDEPTHBIAS added
float g_wrap_bias_val = 0.0f;         // this draw's
float g_wrap_slope_val = 0.0f;        // this draw's slope-scaled bias added
bool g_wrap_bias = false;             // the next draw the stub wraps gets the bias
// Diagnostics (RemoteControl `zvp C [STEP]`, see the top; 0 = off, the default): a level's depth range starts at
// C + STEP * (level - 1) of it (the viewport's MinZ), about that share of the distance farther at every pixel.
float g_zvp = 0.0f;
float g_zvp_step = 0.0f;
float g_wrap_vp_minz = 0.0f;          // this draw's (0: none)
bool g_vp_set = false;
D3DVIEWPORT9 g_saved_vp{}, g_set_vp{};
// Diagnostics (`zprobe SEED [EPS [WHICH]]`): every map piece's draws (WHICH 2: objects' too) seen by a perspective camera
// get +EPS or -EPS of depth bias on top of their own, by a hash of their model and SEED (a negative SEED swaps the two
// halves). Frozen frames with a few seeds differ exactly where two surfaces lie within about EPS in depth: where they
// would fight. 0: off.
volatile LONG g_zprobe = 0;
float g_zprobe_eps = 1.2e-7f;
volatile LONG g_zprobe_which = 1;
DWORD g_saved_bias = 0, g_saved_slope = 0, g_set_bias = 0, g_set_slope = 0;

// Statistics for the log (every 5 s).
unsigned long g_st_frames = 0, g_st_pieces = 0, g_st_skips = 0, g_st_boxes = 0, g_st_owndraws = 0;
unsigned long g_st_hid = 0, g_st_shown_box = 0, g_st_shown_probe = 0, g_st_probes = 0, g_st_notest = 0;
unsigned long g_st_unreliable = 0;
unsigned long g_st_audited = 0, g_st_wrong = 0, g_st_wrong_samples = 0;
float g_st_wrong_near = 1e9f;
unsigned long g_st_lat[5] = {};
int g_wrong_logged = 0;
bool g_told_states = false;
ULONGLONG g_st_next = 0;

// ---- dump (diagnostics: entdump) ----
volatile LONG g_dump_req = 0;
int g_dump_frame = 0;
uint32_t g_pass_cam = 0;
int g_pass_draws = 0;

bool write_bytes(uint32_t at, const void* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, n);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

bool perspective(uint32_t cam) {
    const auto* c = reinterpret_cast<const float*>(cam);
    return c[0xBC / 4] == 1.0f && c[0xCC / 4] == 0.0f;
}

float distance_to(const float* box) {
    const float x = (box[0] + box[3]) * 0.5f - g_cam_pos[0], y = (box[1] + box[4]) * 0.5f - g_cam_pos[1],
                z = (box[2] + box[5]) * 0.5f - g_cam_pos[2];
    return std::sqrt(x * x + y * y + z * z);
}

// How much a test box grows: the camera's movement over the results' delay (four frames) moves the edge of the
// nearest occluder (the player, ~2.5 m from the camera) across the piece by step x distance / 2.5 m a frame; plus a
// little always (a surface flush with a wall, or a decal drawn with a depth bias, has box faces in that wall's depth).
float grow_for(const float* box) {
    const float dist = distance_to(box);
    return g_cam_step * dist * 1.6f + 0.05f + dist * 0.005f;
}

// The camera in (or within a metre of) the grown box: from inside, only the box's far faces are drawn, behind the
// piece's own walls, and the test would find nothing while the piece is in plain view. Never tested nor hidden.
bool near_camera(const float* box) {
    const float m = 1.0f + grow_for(box);
    return g_cam_pos[0] > box[0] - m && g_cam_pos[0] < box[3] + m && g_cam_pos[1] > box[1] - m &&
           g_cam_pos[1] < box[4] + m && g_cam_pos[2] > box[2] - m && g_cam_pos[2] < box[5] + m;
}

// Boxes found not to hold their piece (see apply_batch), remembered for the session by the box itself (a map's
// pieces are made again when it loads again, at other addresses, with the same boxes).
constexpr int kBadBits = 10;
uint32_t g_bad_boxes[1 << kBadBits];
uint32_t box_key(const float* box) {
    uint32_t h = 2166136261u;
    const auto* w = reinterpret_cast<const uint32_t*>(box);
    for (int i = 0; i < 6; ++i) h = (h ^ w[i]) * 16777619u;
    return h | 1u;
}
bool bad_box(const float* box) {
    const uint32_t k = box_key(box);
    uint32_t h = k >> (32 - kBadBits);
    for (int probe = 0; probe < (1 << kBadBits); ++probe, h = (h + 1) & ((1 << kBadBits) - 1)) {
        if (g_bad_boxes[h] == k) return true;
        if (g_bad_boxes[h] == 0) return false;
    }
    return false;
}
void remember_bad_box(const float* box) {
    const uint32_t k = box_key(box);
    uint32_t h = k >> (32 - kBadBits);
    for (int probe = 0; probe < (1 << kBadBits) * 3 / 4; ++probe, h = (h + 1) & ((1 << kBadBits) - 1)) {
        if (g_bad_boxes[h] == k) return;
        if (g_bad_boxes[h] == 0) {
            g_bad_boxes[h] = k;
            return;
        }
    }
}

Batch& cur_batch() {
    Batch& b = g_batches[g_frame % kBatches];
    if (b.frame != g_frame) {
        b.frame = g_frame;
        b.n = 0;
        b.pending = false;
    }
    return b;
}

// A query for this frame's batch (made once per slot, reused). Null when the batch is full or the device refuses.
IDirect3DQuery9* new_record(uint8_t kind, Ent* s, float dist) {
    Batch& b = cur_batch();
    if (b.n >= kMaxRecs) return nullptr;
    IDirect3DQuery9*& q = b.q[b.n];
    if (!q && FAILED(g_dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) {
        q = nullptr;
        return nullptr;
    }
    b.kind[b.n] = kind;
    b.ent[b.n] = s->ent;
    b.slot[b.n] = s;
    b.dist[b.n] = dist;
    ++b.n;
    b.pending = true;
    return q;
}

bool make_device_objects() {
    if (g_vs) return true;
    if (g_dev_failed || !g_dev) return false;
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    D3DDECL_END()};
    if (FAILED(g_dev->CreateVertexShader(reinterpret_cast<const DWORD*>(kOcclBoxVs), &g_vs)) ||
        FAILED(g_dev->CreatePixelShader(reinterpret_cast<const DWORD*>(kOcclBoxPs), &g_ps)) ||
        FAILED(g_dev->CreateVertexDeclaration(el, &g_decl))) {
        g_dev_failed = true;
        LOG_ERROR("OcclusionCulling: could not make its shaders; not used");
        return false;
    }
    return true;
}

// Each box's twelve triangles, corners numbered x | y << 1 | z << 2 (0 = min, 1 = max).
constexpr uint8_t kBoxIdx[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                                 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};

// Right after the depth pre-pass: the boxes of the hidden pieces against the depth it made.
void run_tests() {
    IDirect3DDevice9* d = g_dev;
    static const D3DRENDERSTATETYPE kStates[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_FILLMODE, D3DRS_ALPHABLENDENABLE,
        D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_SCISSORTESTENABLE,
        D3DRS_CLIPPLANEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS};
    static const DWORD kValues[] = {D3DZB_TRUE, FALSE, D3DCMP_LESSEQUAL, D3DCULL_NONE, D3DFILL_SOLID, FALSE,
                                    FALSE, FALSE, FALSE, FALSE, 0, 0, 0, 0};
    constexpr int kN = sizeof(kStates) / sizeof(kStates[0]);
    DWORD saved[kN];
    for (int i = 0; i < kN; ++i) d->GetRenderState(kStates[i], &saved[i]);
    if (!g_told_states) {
        g_told_states = true;
        LOG_INFO("OcclusionCulling: the colour pass starts with depth test %lu, depth writes %lu, compare %lu",
                 static_cast<unsigned long>(saved[0]), static_cast<unsigned long>(saved[1]),
                 static_cast<unsigned long>(saved[2]));
    }
    {
        const int prev = g_fx_list ^ 1;
        for (int i = 0; i < g_fx_ncands[prev] && g_nhidden < kMaxRecs; ++i) {
            Ent* s = g_fx_cands[prev][i];
            if (s && s->ent && !s->unreliable) g_hidden_cands[g_nhidden++] = s;
        }
        g_fx_ncands[prev] = 0;
    }
    if ((!g_nhidden && !g_show) || !make_device_objects() || !g_draw_up) return;
    // The game's view-projection (relative to the camera) is in c0..c3: the fourth row is the view direction (unit
    // length, no translation). Anything else is not the state this was made for: no tests (the hidden show again).
    float rows[16] = {};
    if (FAILED(d->GetVertexShaderConstantF(0, rows, 4))) return;
    const float len = rows[12] * rows[12] + rows[13] * rows[13] + rows[14] * rows[14];
    if (!(len > 0.9f && len < 1.1f) || std::fabs(rows[15]) > 1e-3f) {
        static bool told = false;
        if (!told) {
            told = true;
            LOG_INFO("OcclusionCulling: the camera constants are not as expected (%.3f %.3f %.3f %.3f); not testing",
                     rows[12], rows[13], rows[14], rows[15]);
        }
        return;
    }
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT vb_off = 0, vb_stride = 0;
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    d->GetVertexDeclaration(&decl);
    d->GetStreamSource(0, &vb, &vb_off, &vb_stride);
    for (int i = 0; i < kN; ++i) d->SetRenderState(kStates[i], kValues[i]);
    d->SetVertexShader(g_vs);
    d->SetPixelShader(g_ps);
    d->SetVertexDeclaration(g_decl);

    float v[36 * 3];
    for (int i = 0; i < g_nhidden; ++i) {
        Ent* s = g_hidden_cands[i];
        if (near_camera(s->box)) {
            s->hidden = 0;
            s->zero_run = 0;
            s->shown = g_frame;
            continue;
        }
        const float dist = distance_to(s->box);
        IDirect3DQuery9* q = new_record(kBoxTest, s, dist);
        if (!q) break;
        const float grow = grow_for(s->box);
        float c[8][3];
        for (int k = 0; k < 8; ++k) {
            c[k][0] = (k & 1 ? s->box[3] + grow : s->box[0] - grow) - g_cam_pos[0];
            c[k][1] = (k & 2 ? s->box[4] + grow : s->box[1] - grow) - g_cam_pos[1];
            c[k][2] = (k & 4 ? s->box[5] + grow : s->box[2] - grow) - g_cam_pos[2];
        }
        for (int k = 0; k < 36; ++k) std::memcpy(&v[k * 3], c[kBoxIdx[k]], 12);
        q->Issue(D3DISSUE_BEGIN);
        g_draw_up(d, D3DPT_TRIANGLELIST, 12, v, 12);
        q->Issue(D3DISSUE_END);
        ++g_st_boxes;
    }

    if (g_show) {  // diagnostics: every candidate's box as black lines over the picture (no query)
        d->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        for (int i = 0; i < g_nshow; ++i) {
            const float* x = g_show_ents[i]->box;
            float c[8][3];
            for (int k = 0; k < 8; ++k) {
                c[k][0] = (k & 1 ? x[3] : x[0]) - g_cam_pos[0];
                c[k][1] = (k & 2 ? x[4] : x[1]) - g_cam_pos[1];
                c[k][2] = (k & 4 ? x[5] : x[2]) - g_cam_pos[2];
            }
            for (int k = 0; k < 36; ++k) std::memcpy(&v[k * 3], c[kBoxIdx[k]], 12);
            g_draw_up(d, D3DPT_TRIANGLELIST, 12, v, 12);
        }
    }
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetVertexDeclaration(decl);
    d->SetStreamSource(0, vb, vb_off, vb_stride);
    for (int i = 0; i < kN; ++i) d->SetRenderState(kStates[i], saved[i]);
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (decl) decl->Release();
    if (vb) vb->Release();
}

// A frame's results, once all are back: its own drawing's samples summed per piece first, then the decisions.
void apply_batch(Batch& b) {
    const bool usable = b.frame >= g_jump_frame;
    auto valid = [&](int i) { return b.slot[i] && b.slot[i]->ent == b.ent[i]; };
    for (int i = 0; i < b.n; ++i) {
        if (b.kind[i] == kOwnDraw && valid(i)) {
            b.slot[i]->acc_frame = b.frame;
            b.slot[i]->acc = 0;
        }
    }
    for (int i = 0; i < b.n; ++i) {
        DWORD samples = 0;
        const HRESULT hr = b.q[i]->GetData(&samples, sizeof(samples), 0);
        if (hr != S_OK) samples = 1;  // unknown: as seen
        if (!valid(i)) continue;
        Ent* s = b.slot[i];
        if (b.kind[i] == kAuditDraw) {
            ++g_st_audited;
            if (samples > 0) {
                ++g_st_wrong;
                g_st_wrong_samples += samples;
                if (b.dist[i] < g_st_wrong_near) g_st_wrong_near = b.dist[i];
                if (g_wrong_logged < 12 && samples >= 1000) {
                    ++g_wrong_logged;
                    const float* x = s->box;
                    LOG_INFO("[occl] wrong: ent %08X %s samples %lu dist %.1f evidence age %d box %.1f %.1f %.1f .. "
                             "%.1f %.1f %.1f (size %.0f x %.0f x %.0f)", s->ent,
                             s->extra == 2 ? "effect" : (s->extra ? "extra" : "own"),
                             static_cast<unsigned long>(samples), b.dist[i], b.frame - s->evidence, x[0], x[1], x[2],
                             x[3], x[4], x[5], x[3] - x[0], x[4] - x[1], x[5] - x[2]);
                }
            }
        } else if (b.kind[i] == kOwnDraw) {
            if (s->acc_frame == b.frame) s->acc += samples;
        } else {  // a box test (of a hidden piece, or of one about to be hidden)
            s->box_frame = b.frame;
            s->box_seen = samples > 0;
            if (!usable || b.frame < s->shown) continue;
            if (samples > 0) {
                if (s->hidden) ++g_st_shown_box;
                s->hidden = 0;
                s->zero_run = 0;
                s->box_zero = -1000;
                s->shown = b.frame;
            } else {
                if (s->extra == 2 || s->boxonly) {  // its box is the evidence (effects: see the top), two in a row
                    s->zero_run = s->box_zero == b.frame - 1 ? s->zero_run + 1 : 1;
                    if (s->zero_run >= 2 && !s->hidden) {
                        s->hidden = 1;
                        s->evidence = b.frame;
                        ++g_st_hid;
                    }
                }
                if (b.frame > s->box_zero) s->box_zero = b.frame;
                if (s->hidden && b.frame > s->evidence) s->evidence = b.frame;
            }
        }
    }
    // Its own drawing, once per piece: seen shows it at once; nothing seen in two frames in a row hides it.
    for (int i = 0; i < b.n; ++i) {
        if (b.kind[i] != kOwnDraw || !valid(i)) continue;
        Ent* s = b.slot[i];
        if (s->acc_frame != b.frame) continue;
        s->acc_frame = -1000;
        if (s->acc > 0 && s->box_frame == b.frame && !s->box_seen && !s->unreliable) {
            // Its box found nothing in the very frame its own drawing was seen: the box does not hold it (foliage
            // swaying past it, a surface drawn with a depth bias...). Never hidden from now on.
            s->unreliable = 1;
            remember_bad_box(s->box);
            ++g_st_unreliable;
        }
        if (!usable || b.frame < s->shown) continue;
        if (s->acc > 0) {
            if (s->hidden) ++g_st_shown_probe;
            s->hidden = 0;
            s->zero_run = 0;
            s->shown = b.frame;
        } else if (s->hidden) {
            if (b.frame > s->evidence) s->evidence = b.frame;
        } else if (++s->zero_run >= 2 && s->box_zero >= b.frame - 1 && !s->unreliable) {
            // Its own drawing showed nothing twice, and its box (grown for the camera's movement) neither: hidden.
            // The box keeps a piece the moving camera is about to reveal from being hidden on old evidence.
            s->hidden = 1;
            s->evidence = b.frame;
            ++g_st_hid;
        }
    }
    for (int i = 0; i < b.n; ++i) b.slot[i] = nullptr;
}

void collect() {
    // Oldest first, so a later frame's result decides last.
    for (int k = 1; k <= kBatches; ++k) {
        Batch& b = g_batches[(g_frame + k) % kBatches];
        if (!b.pending || b.frame >= g_frame) continue;
        bool all = true;
        for (int i = 0; i < b.n && all; ++i) {
            DWORD samples = 0;
            if (b.slot[i] && b.q[i]->GetData(&samples, sizeof(samples), 0) == S_FALSE) all = false;
        }
        if (!all && g_frame - b.frame < kBatches - 1) continue;  // not back yet (taken as seen when about to be reused)
        const int lat = g_frame - b.frame;
        ++g_st_lat[lat < 1 ? 0 : lat > 5 ? 4 : lat - 1];
        apply_batch(b);
        b.pending = false;
    }
}

// Effect draw entities (vtable 0x11E7BC4: slot 10 = 0x558500 -> 0xBA4FD0, the FFX instance at +0x30): diagnostics
// for now (entdump lists them with their first 0x100 bytes as floats).
constexpr uint32_t kFxEntityVt = 0x011E7BC4;
volatile LONG g_objshadow = 0;  // diagnostics: RemoteControl `occlobj N`
volatile LONG g_fx_skip = 1;  // added map effects skipped while hidden (1); RemoteControl `occlfx 0|1`, 9: all out
constexpr float kFxMargin = 1.0f;  // metres the effect's box grows by (its particles' size)
int fx_decide_body(uint32_t entity, uint32_t ctx);
int __cdecl decide_fx(uint32_t entity, uint32_t ctx, uint32_t) {
    if (g_fx_skip == 9) return 1;
    const int r = g_fx_skip == 1 ? fx_decide_body(entity, ctx) : 0;
    if (r) return r;
    if (g_dump_frame) {
        const uint32_t cam = *reinterpret_cast<const uint32_t*>(ctx + 0x100);
        const auto* f = reinterpret_cast<const float*>(entity);
        const auto* u = reinterpret_cast<const uint32_t*>(entity);
        char line[1400];
        int n = std::snprintf(line, sizeof(line), "[entdump] fx entity %08X cam %08X fx %08X |", entity, cam, u[0x30 / 4]);
        for (int k = 0; k < 64 && n < static_cast<int>(sizeof(line)) - 24; ++k) {
            const uint32_t v = u[k];
            if (v > 0x00100000u && v < 0x7FFF0000u && (v & 3) == 0) {
                n += std::snprintf(line + n, sizeof(line) - n, " %X:p", k * 4);
            } else {
                n += std::snprintf(line + n, sizeof(line) - n, " %X:%.2f", k * 4, f[k]);
            }
        }
        LOG_INFO("%s", line);
    }
    return 0;
}

// entwatch (diagnostics): every main-camera model draw whose world box centre lies within R of a point, each frame:
// time, entity, box, written to entwatch.txt in the game folder after N frames (pose changes of an animated model
// show in its box: the Hellkite's landing, 2026-11-21).
volatile LONG g_watch_left = 0;
float g_watch_pt[3] = {}, g_watch_r = 0.0f;
std::string g_watch_buf;
uint32_t g_watch_lastcam = 0;
void watch_entity(uint32_t entity, uint32_t cam) {
    if (cam != g_main_cam || !cam) return;
    const auto* b = reinterpret_cast<const float*>(entity + 0xA0);
    const float cx = (b[0] + b[4]) * 0.5f - g_watch_pt[0], cy = (b[1] + b[5]) * 0.5f - g_watch_pt[1],
                cz = (b[2] + b[6]) * 0.5f - g_watch_pt[2];
    if (cx * cx + cy * cy + cz * cz > g_watch_r * g_watch_r) return;
    LARGE_INTEGER now{}, f{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    char line[200];
    std::snprintf(line, sizeof(line), "%d %.3f %08X %.4f %.4f %.4f %.4f %.4f %.4f\n", g_frame,
                  1000.0 * static_cast<double>(now.QuadPart) / static_cast<double>(f.QuadPart), entity, b[0], b[1], b[2],
                  b[4], b[5], b[6]);
    g_watch_buf += line;
}

// An effect draw by the main camera: skipped while hidden, its own drawing counted (see the top) when added.
int fx_decide_body(uint32_t entity, uint32_t ctx) {
    if (g_mode == 0) return 0;
    const uint32_t cam = *reinterpret_cast<const uint32_t*>(ctx + 0x100);
    if (!cam || cam != g_main_cam) return 0;
    const auto* bb = reinterpret_cast<const float*>(entity + 0x50);  // min x y z w, max x y z w
    // Grown for its particles' size: 1 m, and half its largest side more (smoke and mist quads are large), at most 10.
    float side = bb[4] - bb[0];
    if (bb[5] - bb[1] > side) side = bb[5] - bb[1];
    if (bb[6] - bb[2] > side) side = bb[6] - bb[2];
    float m = kFxMargin + 0.5f * side;
    if (!(m <= 10.0f)) m = 10.0f;
    float box[6] = {bb[0] - m, bb[1] - m, bb[2] - m, bb[4] + m, bb[5] + m, bb[6] + m};
    for (float v : box) {
        if (!std::isfinite(v)) return 0;
    }
    if (!(box[3] >= box[0] && box[4] >= box[1] && box[5] >= box[2]) || box[3] - box[0] > kMaxSide ||
        box[4] - box[1] > kMaxSide || box[5] - box[2] > kMaxSide) {
        return 0;
    }
    if (!fx_added(box)) return 0;
    if (g_fxlog_left > 0) fxlog(entity, bb, box);
    Ent* s = find_ent(entity);
    if (!s) return 0;
    // Its particles move its box every frame: kept current, the evidence not reset (a model's box change means another
    // piece in that memory); a new entry starts as a model's does.
    if (s->seen == -1000) {
        s->unreliable = bad_box(box) ? 1 : 0;
        s->shown = g_frame;
    }
    std::memcpy(s->box, box, sizeof(box));
    s->extra = 2;
    ++g_st_fx_added;
    const bool probe = ((hash_ptr(entity, 16) + g_frame) % kProbeEvery) == 0;
    const bool hide = s->hidden && !s->unreliable && s->evidence >= g_frame - kStale && g_frame != g_jump_frame &&
                      !near_camera(box) && !probe;
    if (s->seen != g_frame) {
        s->seen = g_frame;
        if (!s->unreliable && g_fx_ncands[g_fx_list] < kFxCandMax) g_fx_cands[g_fx_list][g_fx_ncands[g_fx_list]++] = s;
    }
    if (hide) {
        ++g_st_fx_skips;
        return 1;
    }
    return 0;
}

int __cdecl decide_inner(uint32_t entity, uint32_t ctx, uint32_t mflags) {
    const uint32_t cam = *reinterpret_cast<const uint32_t*>(ctx + 0x100);
    if (g_watch_left > 0) watch_entity(entity, cam);
    if (g_dump_frame && cam) {
        if (cam != g_pass_cam) {
            if (g_pass_draws) LOG_INFO("[entdump]   %d entity draws", g_pass_draws);
            g_pass_draws = 0;
            g_pass_cam = cam;
            const auto* c = reinterpret_cast<const float*>(cam);
            LOG_INFO("[entdump] pass cam %08X rt %p area %u | pos %.2f %.2f %.2f | proj %.4f %.4f %.4f %.4f / %.4f %.4f",
                     cam, static_cast<void*>(g_rt0), g_rt0_area, c[0x40 / 4], c[0x44 / 4], c[0x48 / 4], c[0x90 / 4],
                     c[0xA4 / 4], c[0xB8 / 4], c[0xBC / 4], c[0xC8 / 4], c[0xCC / 4]);
        }
        ++g_pass_draws;
    }
    if (g_inside && g_player_ok) {
        const uint32_t f = mflags;
        if ((f & 6) == 2 && !(f & kNoInside)) {
            const auto* b = reinterpret_cast<const float*>(entity + 0xA0);  // min x y z w, max x y z w
            const float px = g_player[0], py = g_player[1], pz = g_player[2];
            if (px > b[0] && px < b[4] && py > b[1] && py < b[5] && pz > b[2] && pz < b[6]) {
                if (!g_st_inside_logged) {
                    g_st_inside_logged = 1;
                    LOG_INFO("DrawMapPieces: a piece of another room that holds the player (box %.0f %.0f %.0f .. %.0f %.0f "
                             "%.0f) is left out, as the game does from this room (first time)", b[0], b[1], b[2], b[4],
                             b[5], b[6]);
                }
                ++g_st_inside;
                return 1;
            }
        }
    }
    const LONG mode = g_mode;
    if (mode == 0) return 0;
    if (cam != g_main_cam) {
        if (cam && g_rt0_area > g_best_area && perspective(cam)) {
            g_best_area = g_rt0_area;
            g_best_cam = cam;
        }
        return 0;
    }
    if (g_rt0_area > g_best_area) {
        g_best_area = g_rt0_area;
        g_best_cam = cam;
    }
    if (!g_first_rt) {
        g_first_rt = g_rt0;
        g_first_ds = g_ds;
        const auto* c = reinterpret_cast<const float*>(cam);
        g_cam_pos[0] = c[0x40 / 4];
        g_cam_pos[1] = c[0x44 / 4];
        g_cam_pos[2] = c[0x48 / 4];
        const float dx = g_cam_pos[0] - g_last_pos[0], dy = g_cam_pos[1] - g_last_pos[1],
                    dz = g_cam_pos[2] - g_last_pos[2];
        const float step2 = dx * dx + dy * dy + dz * dz;
        if (step2 > 1.0f) g_jump_frame = g_frame;  // a cut or a warp: everything is drawn this frame
        g_cam_step = step2 > 1.0f ? 0.0f : std::sqrt(step2);
        std::memcpy(g_last_pos, g_cam_pos, sizeof(g_cam_pos));
    } else if (!g_tested && g_rt0 != g_first_rt) {
        g_tested = true;
        g_color_rt = g_rt0;
        g_color_ok = g_ds == g_first_ds;
        if (g_color_ok) {
            run_tests();
        } else {
            ++g_st_notest;
        }
    }
    // Mode 3 (diagnostics only): every model, characters and objects too.
    const uint32_t flags = (mode & 3) == 3 ? 3u : mflags;
    // OcclusionCulling = true: only a model whose every drawn piece is an added one (a model the player's room also
    // draws, a crate or a ladder in both, is never skipped: the game's own draws stay; Grok 1aa07756).
    if (!(flags & 1) || ((mode & 3) == 1 && (flags & 6) != 2)) return 0;
    const auto* bx = reinterpret_cast<const float*>(entity + 0xA0);
    const float box[6] = {bx[0], bx[1], bx[2], bx[4], bx[5], bx[6]};
    if (g_vista_cap && (box[3] - box[0] > kMaxSide || box[4] - box[1] > kMaxSide || box[5] - box[2] > kMaxSide)) return 0;
    Ent* s = find_ent(entity);
    if (!s) return 0;
    if (std::memcmp(box, s->box, sizeof(box)) != 0) {  // another entity in this memory, or the piece moved
        std::memcpy(s->box, box, sizeof(box));
        s->hidden = 0;
        s->zero_run = 0;
        s->unreliable = bad_box(box) ? 1 : 0;
        s->box_frame = s->box_zero = -1000;
        s->shown = g_frame;
    }
    s->extra = (flags & 2) ? 1 : 0;
    const bool in_color = g_tested && g_color_ok && g_rt0 == g_color_rt;
    const bool probe = ((hash_ptr(entity, 16) + g_frame) % kProbeEvery) == 0;
    const bool hide = s->hidden && !s->unreliable && s->evidence >= g_frame - kStale && g_frame != g_jump_frame && !near_camera(box) &&
                      !probe;
    if (s->seen != g_frame) {
        s->seen = g_frame;
        ++g_st_pieces;
        if (g_show && !g_tested && g_nshow < kMaxRecs) g_show_ents[g_nshow++] = s;
        if ((s->hidden || s->zero_run >= 1 || s->boxonly) && !s->unreliable && !g_tested && g_nhidden < kMaxRecs) {
            g_hidden_cands[g_nhidden++] = s;
        }
        if (s->hidden && probe) ++g_st_probes;
    }
    if (hide) {
        ++g_st_skips;
        if (!(mode & 4) || !in_color) return 1;
        g_wrap_q = new_record(kAuditDraw, s, distance_to(box));  // audit: drawn after all, counting its samples
        return g_wrap_q ? 2 : 1;
    }
    if (in_color && g_frame != g_jump_frame && !s->boxonly) {
        g_wrap_q = new_record(kOwnDraw, s, 0.0f);
        ++g_st_owndraws;
        if (g_wrap_q) return 2;
    }
    return 0;
}

// Shadow casters that cannot shade anything in view (2026-11-22): the shadow maps' cameras draw whatever lies in their
// slices, also what lies beside or behind the main camera where its shadow, which falls along the light's direction,
// can never reach what the main camera sees. A model's box is left out of a shadow map's draws when, for one plane of
// the main camera's frustum (its sides and near plane), the whole box lies outside it and the light's direction does
// not point inward through it: then every point the box shades (the box moved along the light) stays outside that
// plane. What the main camera shows is the same; only the water's small mirrored view (256x180, its own camera) could
// have shown such a shadow. The main camera of the last frame (the same object every frame), its world rows at +0x10
// (right, up, forward, position) and projection at +0x90; a shadow map's camera's forward row is the light's direction.
volatile LONG g_shadowcull = 0;  // 0 off (the default: no gain measured), 1 on, 2 counted only (RemoteControl `shadowcull`)
unsigned long g_st_sh_draws = 0, g_st_sh_cull = 0;

bool shadow_cannot_reach(uint32_t cam, uint32_t entity) {
    const uint32_t mc = g_main_cam;
    if (!mc || cam == mc) return false;
    const auto* m = reinterpret_cast<const float*>(mc);
    const float* R = m + 4;
    const float* U = m + 8;
    const float* F = m + 12;
    const float* P = m + 16;
    const float px = m[0x90 / 4], py = m[0xA4 / 4];
    if (!(px > 0.05f && px < 100.0f && py > 0.05f && py < 100.0f)) return false;
    // The frustum's sides widened (tangents x 1.15 + 0.05: about 8 degrees more each way at the game's 43 degree field
    // of view), so a turn between the shadow maps and the picture never meets a caster left out.
    const float tx = 1.15f / px + 0.05f, ty = 1.15f / py + 0.05f;
    const float* L = reinterpret_cast<const float*>(cam) + 12;  // the shadow camera's forward row
    const float l2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
    if (!(l2 > 0.9f && l2 < 1.1f)) return false;
    const auto* b = reinterpret_cast<const float*>(entity + 0xA0);  // min x y z w, max x y z w
    float c[3], e[3];
    for (int k = 0; k < 3; ++k) {
        if (!(b[k] <= b[4 + k]) || !(b[4 + k] - b[k] < 20000.0f)) return false;  // NaN, inverted or unbounded
        c[k] = (b[k] + b[4 + k]) * 0.5f - P[k];
        e[k] = (b[4 + k] - b[k]) * 0.5f;
    }
    const float sides[4][2] = {{1.0f, tx}, {-1.0f, tx}, {1.0f, ty}, {-1.0f, ty}};
    for (int i = 0; i < 5; ++i) {
        float n[3];
        for (int k = 0; k < 3; ++k) {
            n[k] = i == 4 ? F[k] : (i < 2 ? R[k] : U[k]) * sides[i][0] + F[k] * sides[i][1];
        }
        const float nl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        if (nl > 0.0f) continue;  // the light carries the shadow inward through this plane
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float far_in = n[0] * c[0] + n[1] * c[1] + n[2] * c[2] + std::fabs(n[0]) * e[0] + std::fabs(n[1]) * e[1] +
                             std::fabs(n[2]) * e[2];
        if (far_in < -0.5f * len) return true;  // the whole box at least half a metre outside
    }
    return false;
}

// decide_inner's answer (0 draw, 1 skip, 2 draw inside the query g_wrap_q), and 2 also for a drawn piece that only
// DrawMapPieces adds, seen by a perspective camera: wrapped, with the depth bias.
int __cdecl decide(uint32_t entity, uint32_t ctx, uint32_t) {
    const uint32_t mflags = model_flags(*reinterpret_cast<const uint32_t*>(entity + 0x40));
    g_cur_ent = entity;
    g_cur_ctx = ctx;
    g_cur_flags = mflags;
    // An added object the detail rules leave out (another room's version of a map piece or object drawn here): not drawn
    // by any camera, as with DrawMapPieces = game.
    if ((mflags & (kObject | kObjTwin)) == (kObject | kObjTwin) && hidden_object(entity)) {
        ++g_st_objtwin;
        return 1;
    }
    if ((mflags & 1) && g_refl_unused && passcull_skipping()) {
        ++g_st_refl_unused;
        return 1;
    }
    if (g_shadowcull) {
        const uint32_t sc = *reinterpret_cast<const uint32_t*>(ctx + 0x100);
        if (sc && !perspective(sc) && g_main_cam && sc != g_main_cam) {
            ++g_st_sh_draws;
            if (shadow_cannot_reach(sc, entity)) {
                ++g_st_sh_cull;
                if (g_shadowcull == 1) return 1;
            }
        }
    }
    if (g_objshadow && (mflags & kObject) && (mflags & 6) == 2) {
        // Diagnostics (`occlobj N`): added objects left out of the shadow maps (1) or of every other camera (2).
        const uint32_t cam = *reinterpret_cast<const uint32_t*>(ctx + 0x100);
        if (cam && (g_objshadow == 2 ? cam != g_main_cam : !perspective(cam))) return 1;
    }
    const int r = decide_inner(entity, ctx, mflags);
    g_wrap_bias = false;
    g_wrap_vp_minz = 0.0f;
    const LONG zprobe = g_zprobe;
    if (r == 1 || (!g_zbias && !zprobe)) return r;
    const uint32_t cam = *reinterpret_cast<const uint32_t*>(ctx + 0x100);
    if (!cam || !perspective(cam)) return r;
    float probe_bias = 0.0f;
    if (zprobe && (mflags & 1) && (!(mflags & kObject) || g_zprobe_which >= 2)) {
        const uint32_t key = *reinterpret_cast<const uint32_t*>(entity + 0x40);
        const uint32_t seed = static_cast<uint32_t>(zprobe < 0 ? -zprobe : zprobe);
        uint32_t hh = (key >> 4) ^ (seed * 0x9E3779B9u);
        hh ^= hh >> 15;
        hh *= 2654435761u;
        hh ^= hh >> 13;
        const bool back = ((hh >> 31) != 0) != (zprobe < 0);
        probe_bias = back ? g_zprobe_eps : -g_zprobe_eps;
    }
    // The depth level: bits 8-11, level + 1 (the lowest of the model's drawn pieces: the detail rules' or the generic
    // rule's, an added piece 1, the game's 0); 0 only in a table from before (the generic rule then).
    const uint32_t lv = (mflags >> 8) & 0xF;
    const uint32_t level = !g_zbias ? 0u : lv ? lv - 1 : ((mflags & 6) == 2 && !(mflags & kObject) ? 1u : 0u);
    if (level == 0 && probe_bias == 0.0f) return r;
    if (level == 0) {
        g_wrap_bias_val = probe_bias;
        g_wrap_slope_val = 0.0f;
        g_wrap_bias = true;
        return 2;
    }
    // The camera's nearest distance to the piece's world box, and the near plane (projection +0xC8 = -near * q).
    const auto* c = reinterpret_cast<const float*>(cam);
    const auto* b = reinterpret_cast<const float*>(entity + 0xA0);  // min x y z w, max x y z w
    float d2 = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float p = c[0x40 / 4 + k];
        const float e = p < b[k] ? b[k] - p : (p > b[4 + k] ? p - b[4 + k] : 0.0f);
        d2 += e * e;
    }
    const float q = c[0xB8 / 4];
    const float nearp = q > 0.0f ? -c[0xC8 / 4] / q : 0.05f;
    float bias = d2 > 1e-6f ? nearp * g_zbias_world / d2 : g_zbias_max;
    if (bias > g_zbias_max) bias = g_zbias_max;
    if (bias < kZbiasMin) bias = kZbiasMin;
    g_wrap_bias_val = bias * static_cast<float>(level) + probe_bias;
    g_wrap_slope_val = kZbiasSlope * static_cast<float>(level);
    const float zvp = g_zvp;
    if (zvp > 0.0f) {
        float m = zvp + g_zvp_step * static_cast<float>(level - 1);
        if (m > 0.25f) m = 0.25f;
        g_wrap_vp_minz = m;
        g_wrap_bias_val = kZbiasMin * static_cast<float>(level) + probe_bias;
    }
    g_wrap_bias = true;
    return 2;
}

void __cdecl wrap_begin() {
    if (g_wrap_q) {
        g_wrap_q->Issue(D3DISSUE_BEGIN);
        g_wrap_active = true;
    }
    if (g_wrap_bias && g_dev) {
        unsigned long v = 0;
        if (d3dperf_render_state(D3DRS_DEPTHBIAS, &v)) {
            g_saved_bias = v;
        } else {
            g_dev->GetRenderState(D3DRS_DEPTHBIAS, &g_saved_bias);
        }
        if (d3dperf_render_state(D3DRS_SLOPESCALEDEPTHBIAS, &v)) {
            g_saved_slope = v;
        } else {
            g_dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &g_saved_slope);
        }
        float b = 0.0f, s = 0.0f;
        std::memcpy(&b, &g_saved_bias, 4);
        std::memcpy(&s, &g_saved_slope, 4);
        b += g_wrap_bias_val;
        s += g_wrap_slope_val;
        std::memcpy(&g_set_bias, &b, 4);
        std::memcpy(&g_set_slope, &s, 4);
        g_dev->SetRenderState(D3DRS_DEPTHBIAS, g_set_bias);
        g_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, g_set_slope);
        if (g_wrap_vp_minz > 0.0f && SUCCEEDED(g_dev->GetViewport(&g_saved_vp))) {
            g_set_vp = g_saved_vp;
            g_set_vp.MinZ = g_saved_vp.MinZ + (g_saved_vp.MaxZ - g_saved_vp.MinZ) * g_wrap_vp_minz;
            g_vp_set = SUCCEEDED(g_dev->SetViewport(&g_set_vp));
        }
    }
}
void __cdecl wrap_end() {
    if (g_wrap_q) g_wrap_q->Issue(D3DISSUE_END);
    g_wrap_q = nullptr;
    g_wrap_active = false;
    if (g_wrap_bias && g_dev) {
        unsigned long v = 0;
        DWORD now = 0;
        if (d3dperf_render_state(D3DRS_DEPTHBIAS, &v)) {
            now = v;
        } else {
            g_dev->GetRenderState(D3DRS_DEPTHBIAS, &now);
        }
        if (now == g_set_bias) g_dev->SetRenderState(D3DRS_DEPTHBIAS, g_saved_bias);
        if (d3dperf_render_state(D3DRS_SLOPESCALEDEPTHBIAS, &v)) {
            now = v;
        } else {
            g_dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &now);
        }
        if (now == g_set_slope) g_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, g_saved_slope);
        if (g_vp_set) {
            D3DVIEWPORT9 vp{};
            if (SUCCEEDED(g_dev->GetViewport(&vp)) && std::memcmp(&vp, &g_set_vp, sizeof(vp)) == 0) {
                g_dev->SetViewport(&g_saved_vp);
            }
            g_vp_set = false;
        }
    }
    g_wrap_bias = false;
}

// ecx = the object, [esp] = return (site + 7), [esp+4] = context, [esp+8] = arg.
// decide: 0 draw, 1 skip, 2 draw inside the query g_wrap_q.
__declspec(naked) void draw_stub() {
    __asm {
        cmp dword ptr [ecx], 0x01171A54  // kEntityVt
        je model
        cmp dword ptr [ecx], 0x011E7BC4  // kFxEntityVt
        jne pass
        push ecx
        push dword ptr [esp + 4]
        push dword ptr [esp + 12]
        push ecx
        call decide_fx
        add esp, 12
        pop ecx
        test eax, eax
        jnz other
        jmp pass
    model:
        push ecx
        push dword ptr [esp + 4]
        push dword ptr [esp + 12]
        push ecx
        call decide
        add esp, 12
        pop ecx
        test eax, eax
        jnz other
    pass:
        mov edx, dword ptr [ecx]
        jmp dword ptr [edx + 0x28]
    other:
        cmp eax, 2
        je wrap
        ret 8
    wrap:
        push ecx
        call wrap_begin
        pop ecx
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        mov edx, dword ptr [ecx]
        call dword ptr [edx + 0x28]
        call wrap_end
        ret 8
    }
}

HRESULT WINAPI hk_set_rt(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* rt) {
    if (index == 0 && rt != g_rt0) {
        g_rt0 = rt;
        D3DSURFACE_DESC d{};
        g_rt0_area = rt && SUCCEEDED(rt->GetDesc(&d)) ? d.Width * d.Height : 0;
    }
    return o_set_rt(dev, index, rt);
}

HRESULT WINAPI hk_set_ds(IDirect3DDevice9* dev, IDirect3DSurface9* ds) {
    g_ds = ds;
    return o_set_ds(dev, ds);
}

void* patch_slot(void* object, int slot, void* replacement) {
    void** vtable = *reinterpret_cast<void***>(object);
    void* original = vtable[slot];
    if (original == replacement) return nullptr;
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    vtable[slot] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &ignored);
    return original;
}

void release_queries() {
    for (Batch& b : g_batches) {
        for (int i = 0; i < kMaxRecs; ++i) {
            if (b.q[i]) b.q[i]->Release();
            b.q[i] = nullptr;
            b.slot[i] = nullptr;
        }
        b.n = 0;
        b.pending = false;
        b.frame = -1;
    }
}

}  // namespace

bool occl_install() {
    if (g_installed) return true;
    for (const Site& s : kSites) {
        const uint8_t want[9] = {0x8B, 0x11, 0x8B, 0x52, 0x28, 0x50, s.ctx_push, 0xFF, 0xD2};
        if (std::memcmp(reinterpret_cast<const void*>(s.at), want, 9) != 0) {
            LOG_ERROR("OcclusionCulling: the scene draw loop at %08X does not match this build; not used", s.at);
            return false;
        }
    }
    for (const Site& s : kSites) {
        uint8_t code[9] = {0x50, s.ctx_push, 0xE8, 0, 0, 0, 0, 0x90, 0x90};
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&draw_stub) - (s.at + 7));
        std::memcpy(code + 3, &rel, 4);
        write_bytes(s.at, code, 9);
    }
    for (Batch& b : g_batches) b.frame = -1;
    g_installed = true;
    return true;
}

void occl_set_mode(int mode) {
    if (mode < 0 || mode > 7) mode = 0;
    InterlockedExchange(&g_mode, mode);
}

int occl_mode() { return g_mode; }
void occl_set_inside(bool on) { InterlockedExchange(&g_inside, on ? 1 : 0); }
void occl_set_objshadow(int mode) { InterlockedExchange(&g_objshadow, mode); }
void occl_set_fx(int mode) {
    if (mode >= 100) {  // RemoteControl `occlfx 100+N`: log the next N effects taken for added map effects
        g_fxlog_n = 0;
        InterlockedExchange(&g_fxlog_left, mode - 100);
        return;
    }
    InterlockedExchange(&g_fx_skip, mode);
}
bool occl_query_active() { return g_wrap_active; }

void occl_note_reflection_read(unsigned stages) {
    if ((stages & 0x100u) && g_cur_ent && g_wrap_active) {
        // Inside our query around a piece's drawing: from now on its box alone hides it, and the mirror's code counts
        // its reading draws with queries of its own (they do not nest).
        Ent* s = find_ent(g_cur_ent);
        if (s && !s->boxonly) {
            s->boxonly = 1;
            s->zero_run = 0;
            ++g_st_readers;
        }
    }
    if (g_readlog <= 0) return;
    --g_readlog;
    const uint32_t e = g_cur_ent;
    const auto* b = e ? reinterpret_cast<const float*>(e + 0xA0) : nullptr;
    const uint32_t cam = g_cur_ctx ? *reinterpret_cast<const uint32_t*>(g_cur_ctx + 0x100) : 0;
    LOG_INFO("[occl] mirror read: stages %X, last entity %08X (vt %08X, model %08X, flags %u) cam %08X main %d area %u box "
             "%.1f %.1f %.1f .. %.1f %.1f %.1f", stages, e, e ? *reinterpret_cast<const uint32_t*>(e) : 0,
             e ? *reinterpret_cast<const uint32_t*>(e + 0x40) : 0, g_cur_flags, cam, cam == g_main_cam, g_rt0_area,
             b ? b[0] : 0, b ? b[1] : 0, b ? b[2] : 0, b ? b[4] : 0, b ? b[5] : 0, b ? b[6] : 0);
}

void occl_set_refl(int on) {
    if (on >= 100) {
        InterlockedExchange(&g_readlog, on - 100);
    } else {
        InterlockedExchange(&g_refl_unused, on ? 1 : 0);
    }
}
void occl_watch(float x, float y, float z, float r, int frames) {
    g_watch_pt[0] = x;
    g_watch_pt[1] = y;
    g_watch_pt[2] = z;
    g_watch_r = r;
    g_watch_buf.clear();
    InterlockedExchange(&g_watch_left, frames);
}
void occl_set_zbias(bool on, float world, float max_bias) {
    if (world >= 0.0f) g_zbias_world = world;
    if (max_bias >= 0.0f) g_zbias_max = max_bias;
    InterlockedExchange(&g_zbias, on ? 1 : 0);
}
void occl_set_shadowcull(int mode) { InterlockedExchange(&g_shadowcull, mode); }
void occl_set_zvp(float c, float step) {
    if (c >= 0.0f) g_zvp = c;
    if (step >= 0.0f) g_zvp_step = step;
}
float occl_zvp() { return g_zvp; }
void occl_set_zprobe(int seed, float eps, int which) {
    if (eps > 0.0f) g_zprobe_eps = eps;
    if (which > 0) InterlockedExchange(&g_zprobe_which, which);
    InterlockedExchange(&g_zprobe, seed);
}
void occl_set_vista_cap(int on) { InterlockedExchange(&g_vista_cap, on); }
void occl_set_show(int on) { InterlockedExchange(&g_show, on); }

void occl_capture_device(IDirect3DDevice9* device) {
    if (!device || g_draw_up) return;
    g_draw_up = reinterpret_cast<DrawUpFn>((*reinterpret_cast<void***>(device))[83]);
}

void occl_set_device(IDirect3DDevice9* device) {
    if (g_dev && device != g_dev) {  // a new device: nothing made on the old one is used again
        release_queries();
        g_vs = nullptr;
        g_ps = nullptr;
        g_decl = nullptr;
        g_dev_failed = false;
    }
    g_dev = device;
    if (!device || !g_installed) return;
    void* p = patch_slot(device, 37, reinterpret_cast<void*>(&hk_set_rt));
    if (p && !o_set_rt) o_set_rt = reinterpret_cast<SetRtFn>(p);
    p = patch_slot(device, 39, reinterpret_cast<void*>(&hk_set_ds));
    if (p && !o_set_ds) o_set_ds = reinterpret_cast<SetDsFn>(p);
}

void occl_reset() {
    release_queries();
    std::memset(g_ents, 0, sizeof(g_ents));
    g_ent_used = 0;
    g_rt0 = nullptr;
    g_rt0_area = 0;
    g_ds = nullptr;
    g_main_cam = 0;
    g_wrap_q = nullptr;
}

void occl_game_frame() {
#if FPSU_DIAGNOSTICS
    // Ctrl+Shift+O (with the game in front): off, the pieces DrawMapPieces adds, every map piece, off...
    if (g_installed) {
        static bool was_down = false;
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                          (GetAsyncKeyState('O') & 0x8000);
        if (down && !was_down) {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid == GetCurrentProcessId()) {
                const int next = (g_mode & 3) == 0 ? 1 : (g_mode & 3) == 1 ? 2 : 0;
                occl_set_mode(next);
                LOG_INFO("OcclusionCulling (Ctrl+Shift+O): %s", next == 0   ? "off"
                                                                : next == 1 ? "the pieces DrawMapPieces adds"
                                                                            : "every map piece");
            }
        }
        was_down = down;
    }
#endif
    if (!g_installed || (g_mode == 0 && !g_inside && !g_zbias && !(g_refl_unused && passcull_mode() == 1))) return;
    // The table of the map pieces' models every ten frames (and when the player's room changes).
    bool room_changed = false;
    __try {
        const uint32_t wcm = *reinterpret_cast<const uint32_t*>(kWorldChrMan);
        const uint32_t chr = wcm ? *reinterpret_cast<const uint32_t*>(wcm + 0x3C) : 0;
        const uint32_t floor = chr ? *reinterpret_cast<const uint32_t*>(chr + 0x29C) : 0;
        room_changed = drawdist_game_mask_gen() != g_built_gen ||  // the game wrote its draw masks again
                       drawdist_role_gen() != g_built_role_gen;     // or the detail rules changed
        // The player's position: its physics (+0x28 controller, +0x1C physics, +0x10 x y z).
        const uint32_t ctrl = chr ? *reinterpret_cast<const uint32_t*>(chr + 0x28) : 0;
        const uint32_t phys = ctrl ? *reinterpret_cast<const uint32_t*>(ctrl + 0x1C) : 0;
        if (phys && floor) {
            const auto* p = reinterpret_cast<const float*>(phys + 0x10);
            g_player[0] = p[0];
            g_player[1] = p[1] + 1.0f;
            g_player[2] = p[2];
            g_player_ok = 1;
        } else {
            g_player_ok = 0;  // loading, or no player: nothing left out
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_player_ok = 0;
    }
    if (room_changed || ++g_game_ticks >= 10) {
        g_game_ticks = 0;
        build_models();
    }
}

void occl_render_frame() {
    g_fx_list ^= 1;
    g_fx_ncands[g_fx_list] = 0;
    if (g_watch_left > 0 && InterlockedDecrement(&g_watch_left) == 0) {
        if (FILE* f = std::fopen("entwatch.txt", "wb")) {
            std::fwrite(g_watch_buf.data(), 1, g_watch_buf.size(), f);
            std::fclose(f);
        }
        LOG_INFO("[entwatch] written (%u bytes)", static_cast<unsigned>(g_watch_buf.size()));
        g_watch_buf.clear();
    }
    if (g_dump_frame) {
        if (g_pass_draws) LOG_INFO("[entdump]   %d entity draws", g_pass_draws);
        g_dump_frame = 0;
        g_pass_cam = 0;
        g_pass_draws = 0;
        LOG_INFO("[entdump] end of frame");
    }
    if (InterlockedExchange(&g_dump_req, 0) != 0) {
        g_dump_frame = 1;
        LOG_INFO("[entdump] passes of a frame (main camera %08X)", g_main_cam);
    }
    if (!g_installed) return;
    ++g_frame;
    g_main_cam = g_mode != 0 ? g_best_cam : 0;
    g_best_cam = 0;
    g_best_area = 0;
    g_first_rt = nullptr;
    g_first_ds = nullptr;
    g_color_rt = nullptr;
    g_tested = false;
    g_color_ok = false;
    g_nhidden = 0;
    g_nshow = 0;
    g_wrap_q = nullptr;
    if (g_mode != 0) collect();
    ++g_st_frames;
    const ULONGLONG now = GetTickCount64();
    if (now >= g_st_next) {
        if (g_st_next && g_mode != 0 && g_st_frames) {
            const double f = static_cast<double>(g_st_frames);
            LOG_INFO("[occl] per frame: %.1f pieces, %.1f draws skipped, %.1f own draws counted, %.1f boxes, %.2f probes;"
                     " hidden %.2f, shown by box %.2f / by probe %.2f; %lu boxes found unreliable%s", g_st_pieces / f,
                     g_st_skips / f, g_st_owndraws / f, g_st_boxes / f, g_st_probes / f, g_st_hid / f,
                     g_st_shown_box / f, g_st_shown_probe / f, g_st_unreliable,
                     g_st_notest ? " (some frames had no depth to test against)" : "");
            LOG_INFO("[occl] results back after 1/2/3/4/5+ frames: %lu %lu %lu %lu %lu", g_st_lat[0], g_st_lat[1],
                     g_st_lat[2], g_st_lat[3], g_st_lat[4]);
            if (g_mode & 4) {
                LOG_INFO("[occl] audit: %lu colour draws of hidden pieces checked, %lu had visible samples (%lu samples, "
                         "nearest %.1f m)", g_st_audited, g_st_wrong, g_st_wrong_samples,
                         g_st_wrong ? static_cast<double>(g_st_wrong_near) : 0.0);
            }
        }
        if (g_st_next && g_st_inside && g_st_frames) {
            LOG_INFO("[occl] DrawMapPieces: %.1f draws a frame of other rooms' pieces holding the player left out",
                     static_cast<double>(g_st_inside) / static_cast<double>(g_st_frames));
        }
        if (g_st_next && g_st_fx_added && g_st_frames) {
            LOG_INFO("[occl] DrawMapPieces' map effects: %.1f added effect draws a frame, %.1f skipped while hidden",
                     static_cast<double>(g_st_fx_added) / static_cast<double>(g_st_frames),
                     static_cast<double>(g_st_fx_skips) / static_cast<double>(g_st_frames));
        }
        if (g_st_next && g_st_refl_unused && g_st_frames) {
            LOG_INFO("[occl] unused reflection: %.1f map piece draws a frame not made",
                     static_cast<double>(g_st_refl_unused) / static_cast<double>(g_st_frames));
        }
        if (g_st_next && g_st_objtwin && g_st_frames) {
            LOG_INFO("[occl] DrawMapObjects: %.1f draws a frame of other rooms' objects lying on pieces drawn here left out",
                     static_cast<double>(g_st_objtwin) / static_cast<double>(g_st_frames));
        }
        if (g_st_next && g_st_sh_draws && g_st_frames) {
            LOG_INFO("[occl] shadow maps: %.1f of %.1f model draws a frame %s (their shadow cannot reach the view)",
                     static_cast<double>(g_st_sh_cull) / static_cast<double>(g_st_frames),
                     static_cast<double>(g_st_sh_draws) / static_cast<double>(g_st_frames),
                     g_shadowcull == 1 ? "left out" : "could be left out");
        }
        g_st_sh_draws = g_st_sh_cull = 0;
        g_st_refl_unused = 0;
        g_st_objtwin = 0;
        g_st_fx_added = g_st_fx_skips = 0;
        g_st_inside = 0;
        g_st_next = now + 5000;
        g_st_frames = g_st_pieces = g_st_skips = g_st_boxes = g_st_owndraws = g_st_hid = g_st_shown_box = 0;
        g_st_shown_probe = g_st_probes = g_st_notest = g_st_audited = g_st_wrong = g_st_wrong_samples = 0;
        g_st_unreliable = 0;
        g_st_wrong_near = 1e9f;
        g_wrong_logged = 0;
        std::memset(g_st_lat, 0, sizeof(g_st_lat));
    }
}

void occl_request_dump(int) { InterlockedExchange(&g_dump_req, 1); }
