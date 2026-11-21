#include "occl.h"

#include "drawdist.h"

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

inline uint32_t hash_ptr(uint32_t p, int bits) { return ((p >> 3) * 2654435761u) >> (32 - bits); }

void build_models() {
    const int back = g_models_cur == 0 ? 1 : 0;
    ModelSlot* t = g_models[back];
    std::memset(t, 0, sizeof(g_models[0]));
    const uint32_t gen = drawdist_game_mask_gen();
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
                const bool extra = !((g[0] & own[0]) | (g[1] & own[1]) | (g[2] & own[2]) | (g[3] & own[3]));
                uint32_t h = hash_ptr(fl, kModelBits);
                for (int probe = 0; probe < kModelSlots; ++probe, h = (h + 1) & (kModelSlots - 1)) {
                    if (t[h].key == 0 || t[h].key == fl) {
                        t[h].key = fl;
                        t[h].flags |= 1u | (extra ? 2u : 4u);
                        break;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;  // keep the table in use
    }
    g_built_gen = gen;
    InterlockedExchange(&g_models_cur, back);
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
    uint8_t pad[2];
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
bool g_wrap_bias = false;             // the next draw the stub wraps gets the bias
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
                             "%.1f %.1f %.1f (size %.0f x %.0f x %.0f)", s->ent, s->extra ? "extra" : "own",
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

int __cdecl decide_inner(uint32_t entity, uint32_t ctx) {
    const uint32_t cam = *reinterpret_cast<const uint32_t*>(ctx + 0x100);
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
        const uint32_t f = model_flags(*reinterpret_cast<const uint32_t*>(entity + 0x40));
        if ((f & 6) == 2) {
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
    const uint32_t flags = (mode & 3) == 3 ? 3u : model_flags(*reinterpret_cast<const uint32_t*>(entity + 0x40));
    if (!(flags & 1) || ((mode & 3) == 1 && !(flags & 2))) return 0;
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
        if ((s->hidden || s->zero_run >= 1) && !s->unreliable && !g_tested && g_nhidden < kMaxRecs) {
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
    if (in_color && g_frame != g_jump_frame) {
        g_wrap_q = new_record(kOwnDraw, s, 0.0f);
        ++g_st_owndraws;
        if (g_wrap_q) return 2;
    }
    return 0;
}

// decide_inner's answer (0 draw, 1 skip, 2 draw inside the query g_wrap_q), and 2 also for a drawn piece that only
// DrawMapPieces adds, seen by a perspective camera: wrapped, with the depth bias.
int __cdecl decide(uint32_t entity, uint32_t ctx, uint32_t) {
    const int r = decide_inner(entity, ctx);
    g_wrap_bias = false;
    if (r == 1 || !g_zbias) return r;
    const uint32_t cam = *reinterpret_cast<const uint32_t*>(ctx + 0x100);
    if (!cam || !perspective(cam)) return r;
    if ((model_flags(*reinterpret_cast<const uint32_t*>(entity + 0x40)) & 6) != 2) return r;
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
    g_wrap_bias_val = bias;
    g_wrap_bias = true;
    return 2;
}

void __cdecl wrap_begin() {
    if (g_wrap_q) g_wrap_q->Issue(D3DISSUE_BEGIN);
    if (g_wrap_bias && g_dev) {
        g_dev->GetRenderState(D3DRS_DEPTHBIAS, &g_saved_bias);
        g_dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &g_saved_slope);
        float b = 0.0f, s = 0.0f;
        std::memcpy(&b, &g_saved_bias, 4);
        std::memcpy(&s, &g_saved_slope, 4);
        b += g_wrap_bias_val;
        s += kZbiasSlope;
        std::memcpy(&g_set_bias, &b, 4);
        std::memcpy(&g_set_slope, &s, 4);
        g_dev->SetRenderState(D3DRS_DEPTHBIAS, g_set_bias);
        g_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, g_set_slope);
    }
}
void __cdecl wrap_end() {
    if (g_wrap_q) g_wrap_q->Issue(D3DISSUE_END);
    g_wrap_q = nullptr;
    if (g_wrap_bias && g_dev) {
        DWORD now = 0;
        g_dev->GetRenderState(D3DRS_DEPTHBIAS, &now);
        if (now == g_set_bias) g_dev->SetRenderState(D3DRS_DEPTHBIAS, g_saved_bias);
        g_dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &now);
        if (now == g_set_slope) g_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, g_saved_slope);
    }
    g_wrap_bias = false;
}

// ecx = the object, [esp] = return (site + 7), [esp+4] = context, [esp+8] = arg.
// decide: 0 draw, 1 skip, 2 draw inside the query g_wrap_q.
__declspec(naked) void draw_stub() {
    __asm {
        cmp dword ptr [ecx], 0x01171A54  // kEntityVt
        jne pass
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
void occl_set_zbias(bool on, float world, float max_bias) {
    if (world >= 0.0f) g_zbias_world = world;
    if (max_bias >= 0.0f) g_zbias_max = max_bias;
    InterlockedExchange(&g_zbias, on ? 1 : 0);
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
    if (!g_installed || (g_mode == 0 && !g_inside && !g_zbias)) return;
    // The table of the map pieces' models every ten frames (and when the player's room changes).
    bool room_changed = false;
    __try {
        const uint32_t wcm = *reinterpret_cast<const uint32_t*>(kWorldChrMan);
        const uint32_t chr = wcm ? *reinterpret_cast<const uint32_t*>(wcm + 0x3C) : 0;
        const uint32_t floor = chr ? *reinterpret_cast<const uint32_t*>(chr + 0x29C) : 0;
        room_changed = drawdist_game_mask_gen() != g_built_gen;  // the game wrote its draw masks again
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
