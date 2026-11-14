#include "layout.h"

#include "aspect.h"
#include "resolution.h"
#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// The game lays the HUD and the menus out on a 1280x720 screen. Their vertex shaders take a position in those units
// and the matrix in c0..c3 that maps 0..1280 and 0..720 onto the whole viewport: c0 = (2/1280, 0, 0, -1), c1 = (0,
// -2/720, 0, 1), uploaded as "5 registers from 0" before the pass and left there for all of it. Three shaders draw it:
//   - images (vs_aadb9cd6): 12-byte vertices, the position two 16-bit integers in eighths of a pixel (x / 8 - 0.5);
//   - text (vs_5ba5d9ad): 24-byte vertices, the position two floats (their whole parts are used);
//   - plain coloured boxes (vs_3c71dd1e): 12-byte vertices of three floats, through the 3x4 matrix in c8..c10 first
//     (the gauges' fills, the fades).
// On a screen that is not 16:9 (FixAspect: the 3D picture fills it, a taller or a wider view) that matrix stretches
// the whole layout to the screen's shape: on a 16:10 Steam Deck everything is 11% taller, at 21:9 a third wider.
//
// FixAspectUI: while one of those three shaders is bound, the matrix maps 1280x720 onto the largest 16:9 area that
// fits the middle of the screen instead (c0 and c1 scaled by kx and ky, each 1 or less), so the layout keeps its
// shape; with any other shader bound the device holds the game's own rows (the post-processing passes draw their
// full-screen quads with matrices of the same form). A box of the layout that covers all of 1280x720 (a fade, a
// loading screen's background) is drawn over the whole screen as before. A clipping rectangle the menus set is in
// the same 1280x720 units and is mapped the same way (layout_map_scissor).
//
// Marks that follow something in the world (the lock-on marker, the health bars over enemies and other players, the
// damage numbers on them) get their place from one routine (0x4F4030: a world position through the camera into
// 1280x720 layout units, where 0 and 1280 are the picture's left and right edges). With the layout drawn in the
// middle 16:9 area those units no longer span the picture, so the routine's result is spread out from the layout's
// centre by 1/kx and 1/ky (world_point_fix, from its three callers): the mark is drawn over its target again, at its
// unstretched size. A health bar whose place then lies outside 0..1280 (a target in the side margins of an ultrawide
// picture) is not drawn: the game hides bars outside the layout.
//
// World marks: the game works out where each mark over something in the world goes (0x4F4030, see above) and keeps
// the places in MenuMan: the lock-on point (+0x9D4 x, +0x9D8 y; shown while +0x9E4), seven other players' gauges
// (+0x8F4 + i * 0x20: x, y; in use while +0xD) and eight enemies' gauges (+0xD34 + i * 0x24: handle, x, y; shown
// while +0x20). A draw lying around one of these places (its middle within 110 units across and 90 above to 50 below)
// is that mark's: never part of a HUD corner group (an enemy's bar near the top left corner was pulled into it), and
// with UIScaleWorld drawn smaller about that place. The HUD's own top-left pieces (the bars' start and end caps in
// their fixed rows, the humanity counter) stay HUD even when an enemy's bar passes them. The draw order does not tell
// them apart: an enemy's bar is drawn in the middle of the HUD's own (after the bars' end caps, before the humanity
// counter).
//
// UIScale (percent, below 100): the HUD's corner groups are drawn smaller, each scaled toward its own corner of the
// layout: the health and stamina bars with the status icons (top left), the equipment slots (bottom left) and the
// soul counter (bottom right); a boss's health bar toward the bottom edge. A draw belongs to a group when it lies
// where that group's pieces are (fit_for); everything else (prompts, subtitles, the item pickup window) is left at
// its size, and so is everything
// while a menu or a dialog with buttons is open or for half a second after it closes (its fade), and while no
// character is in the world (the title screen and its menus, whose copyright line and load list reach into those
// areas).
namespace {

using CreateVsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVsConstFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using DrawUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawPrimFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIdxFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawIdxUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                     const void*, UINT);
using SetScissorFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*);
CreateVsFn o_create_vs = nullptr;
SetVsFn o_set_vs = nullptr;
SetVsConstFn o_set_vs_const = nullptr;
DrawUpFn o_draw_up = nullptr;
DrawIdxUpFn o_draw_idx_up = nullptr;
SetScissorFn o_set_scissor = nullptr;

// kPlain: float positions in layout units, for the game's frames of lines whatever their shader (layout_frame; the
// white outline of another player's bar stayed full size with the image, text and box kinds and lines handled).
enum Kind : uint8_t { kOther = 0, kImage = 1, kText = 2, kBox = 3, kPlain = 4 };

bool g_keep_shape = true;
float g_scale = 1.0f;
float g_world_scale = 1.0f;  // UIScaleWorld

// The world marks' places this frame (read_anchors), the render thread's.
struct Anchor {
    float x, y;
    uint8_t kind;  // 0 lock-on, 1 another player, 2 an enemy
};
Anchor g_anchors[16];
int g_anchor_count = 0;
int g_logged_world = 0;
bool g_boss_up = false;  // a boss's health bar is up this frame (read_anchors)
bool g_logged_boss = false;
void read_anchors();
bool g_own_scissor = false;  // no render-resolution module in front: the clipping rectangles are mapped here
bool g_premapped = false;    // the render thread's: a call site of the game mapped the rectangle on its way

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<IDirect3DVertexShader9*, uint8_t> g_kinds;

// The render thread's.
uint8_t g_bound = kOther;
bool g_layout = false;       // the game's c0/c1 are the 1280x720 layout's
float g_rows[2][4] = {};     // the game's c0 and c1 as last set
bool g_shaped = false;       // the device holds the rows scaled by kx/ky
float g_kx = 1.0f, g_ky = 1.0f;
float g_box[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};  // the game's c8..c10 as last set
std::vector<uint8_t> g_copy;
bool g_logged_shape = false, g_logged_scale = false, g_logged_full = false;

constexpr float kLayoutW = 1280.0f, kLayoutH = 720.0f;
constexpr uintptr_t kMenuManVar = 0x013786D0;

bool near_f(float a, float b, float tol) { return std::fabs(a - b) < tol; }

// The shaders by their code: the image shader's 0.125 scale, the text shader's leading frc, the box shader's dp4s
// with c8..c10.
uint8_t classify(const DWORD* code) {
    if (!code || (code[0] & 0xFFFF0000u) != 0xFFFE0000u) return kOther;
    bool eighth = false, frc_first = false, first = true, c8 = false, c0 = false;
    const DWORD* p = code + 1;
    for (int guard = 0; guard < 4000; ++guard) {
        const DWORD t = *p;
        const DWORD op = t & 0xFFFF;
        if (op == 0xFFFF) break;
        if (op == 0xFFFE) {
            p += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (t >> 24) & 0xF;
        if (op == 0x51 && len >= 5) {  // def: a 0.125 first component
            float f;
            std::memcpy(&f, &p[2], 4);
            if (f == 0.125f) eighth = true;
        } else if (op != 0x1F && op != 0x30 && op != 0x2F) {
            if (first) {
                frc_first = op == 0x13;  // frc
                first = false;
            }
            for (DWORD i = 2; i <= len; ++i) {
                const DWORD a = p[i];
                if (!(a & 0x80000000u) || (((a >> 28) & 7) | ((a >> 8) & 0x18)) != 2) continue;
                const DWORD num = a & 0x7FF;
                if (num == 8 && op == 9) c8 = true;                // dp4 with c8
                if (num == 0 && (op == 8 || op == 9)) c0 = true;   // dp3 / dp4 with c0
            }
        }
        p += 1 + len;
    }
    if (!c0) return kOther;
    if (eighth) return kImage;
    if (frc_first) return kText;
    if (c8) return kBox;
    return kOther;
}

void send_rows(IDirect3DDevice9* dev, bool shaped) {
    float rows[8];
    std::memcpy(rows, g_rows, sizeof(rows));
    if (shaped) {
        rows[0] *= g_kx;
        rows[3] *= g_kx;
        rows[5] *= g_ky;
        rows[7] *= g_ky;
    }
    o_set_vs_const(dev, 0, rows, 2);
    g_shaped = shaped;
    if (shaped && !g_logged_shape) {
        g_logged_shape = true;
        LOG_INFO("FixAspectUI: the HUD and the menus keep their 16:9 shape in the middle of the %.3f picture (drawn at "
                 "%.0f%% of its width, %.0f%% of its height)",
                 static_cast<double>(aspect_value()), g_kx * 100.0, g_ky * 100.0);
    }
}

// kx, ky for the picture's shape now.
void shape_now() {
    g_kx = g_ky = 1.0f;
    if (!g_keep_shape || !aspect_is_active()) return;
    const float a = aspect_value(), v = 16.0f / 9.0f;
    if (a > v) g_kx = v / a;
    if (a < v) g_ky = a / v;
}

// The place the game worked out for a mark on something in the world (x, y in layout units), on the game's thread.
void __cdecl world_point_fix(float* out) {
    if (!g_keep_shape || !aspect_is_active()) return;
    const float a = aspect_value(), v = 16.0f / 9.0f;
    if (a > v) {
        out[0] = kLayoutW * 0.5f + (out[0] - kLayoutW * 0.5f) * (a / v);
    } else if (a < v) {
        out[1] = kLayoutH * 0.5f + (out[1] - kLayoutH * 0.5f) * (v / a);
    }
}

// 0x4F4030 takes the camera in EAX, the world position in EDI and the result's address on the stack (and removes
// it), and returns that address. tools/test_rendermath/test_worldpoint.cpp runs these instructions against a
// stand-in with that convention.
void* g_world_to_layout = reinterpret_cast<void*>(0x004F4030);

__declspec(naked) void world_point_thunk() {
    __asm {
        push dword ptr [esp + 4]
        call dword ptr [g_world_to_layout]
        push ecx
        push edx
        push eax
        call world_point_fix
        pop eax
        pop edx
        pop ecx
        ret 4
    }
}

constexpr uintptr_t kWorldToLayoutCalls[] = {0x01012B5F, 0x010132D4, 0x010139DA};
bool g_world_patched = false;

// Only once the picture is not 16:9: on a 16:9 screen the game's calls stay as they are.
void patch_world_points() {
    if (g_world_patched || !aspect_is_active()) return;
    g_world_patched = true;
    const uintptr_t target = reinterpret_cast<uintptr_t>(g_world_to_layout);
    for (uintptr_t at : kWorldToLayoutCalls) {
        const auto* p = reinterpret_cast<const uint8_t*>(at);
        int32_t rel;
        std::memcpy(&rel, p + 1, 4);
        if (p[0] != 0xE8 || at + 5 + rel != target) {
            LOG_ERROR("FixAspectUI: the call at %08X is not the one this build knows; marks on targets (lock-on, "
                      "health bars) keep the stretched layout's places", static_cast<unsigned>(at));
            return;
        }
    }
    for (uintptr_t at : kWorldToLayoutCalls) {
        auto* p = reinterpret_cast<uint8_t*>(at);
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&world_point_thunk) - (at + 5));
        DWORD old = 0;
        if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
        std::memcpy(p + 1, &rel, 4);
        DWORD ignored = 0;
        VirtualProtect(p, 5, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    LOG_INFO("FixAspectUI: marks on targets in the world (lock-on, health bars, damage numbers) are placed for the "
             "unstretched layout (3 calls of 004F4030)");
}

bool wants_shape() { return g_layout && (g_kx != 1.0f || g_ky != 1.0f) && g_bound != kOther; }

HRESULT WINAPI hk_create_vs(IDirect3DDevice9* dev, const DWORD* code, IDirect3DVertexShader9** out) {
    const HRESULT hr = o_create_vs(dev, code, out);
    if (SUCCEEDED(hr) && out && *out) {
        const uint8_t kind = classify(code);
        AcquireSRWLockExclusive(&g_lock);
        if (kind != kOther) {
            g_kinds[*out] = kind;
        } else {
            g_kinds.erase(*out);
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    return hr;
}

HRESULT WINAPI hk_set_vs(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    const HRESULT hr = o_set_vs(dev, vs);
    uint8_t kind = kOther;
    if (vs) {
        AcquireSRWLockShared(&g_lock);
        const auto it = g_kinds.find(vs);
        if (it != g_kinds.end()) kind = it->second;
        ReleaseSRWLockShared(&g_lock);
    }
    g_bound = kind;
    if (g_layout) {
        const bool want = wants_shape();
        if (want != g_shaped) send_rows(dev, want);
    }
    return hr;
}

HRESULT WINAPI hk_set_vs_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    const HRESULT hr = o_set_vs_const(dev, start, v, count);
    if (!v) return hr;
    if (start <= 10 && start + count > 8) {
        for (UINT r = 8; r <= 10; ++r) {
            if (r >= start && r < start + count) std::memcpy(g_box[r - 8], v + (r - start) * 4, 16);
        }
    }
    if (start <= 1 && start + count > 0) {
        if (start == 0 && count >= 2 && count <= 8 && near_f(v[0], 2.0f / kLayoutW, 1e-7f) && v[1] == 0.0f &&
            v[2] == 0.0f && v[3] == -1.0f && v[4] == 0.0f && near_f(v[5], -2.0f / kLayoutH, 1e-7f) && v[6] == 0.0f &&
            v[7] == 1.0f) {
            std::memcpy(g_rows, v, sizeof(g_rows));
            g_layout = true;
            g_shaped = false;
            shape_now();
            read_anchors();
            if (wants_shape()) send_rows(dev, true);
        } else {
            g_layout = false;
            g_shaped = false;
        }
    }
    return hr;
}

// The player's character is in the world ([[[WorldChrMan] + 4]], as remote.cpp's `pos`): there is none on the title
// screen and its menus (the load list, character creation, the settings), which MenuMan's flags do not show.
bool player_in_world() {
    __try {
        const uintptr_t wcm = *reinterpret_cast<const uintptr_t*>(0x0137DC70);
        if (wcm < 0x10000) return false;
        const uintptr_t first = *reinterpret_cast<const uintptr_t*>(wcm + 4);
        if (first < 0x10000) return false;
        return *reinterpret_cast<const uintptr_t*>(first) >= 0x10000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A menu is open (MenuMan: its request, its open flags) or a dialog with buttons ([MenuMan+0x9E8]: +0 its text, +0xC
// 7 or 8; 9 is a message without buttons, whose text stays behind after it, as co-op's world::menu_open reads it).
bool menu_flags() {
    __try {
        const uintptr_t mm = *reinterpret_cast<const uintptr_t*>(kMenuManVar);
        if (mm < 0x10000) return true;  // no menu manager yet: the title screens
        // +0x3C: the menu shown (0 none, 8 the start menu's tabs, 2 the inventory, ...); +0x60: a request for one
        if (*reinterpret_cast<const int32_t*>(mm + 0x3C) != 0) return true;
        if (*reinterpret_cast<const int32_t*>(mm + 0x60) != 0) return true;
        static const uint32_t kOpen[] = {0x40, 0x4C, 0x78, 0x84, 0x80, 0x50, 0xAC};
        for (uint32_t off : kOpen) {
            if (*reinterpret_cast<const uint8_t*>(mm + off) != 0) return true;
        }
        const uintptr_t dialog = *reinterpret_cast<const uintptr_t*>(mm + 0x9E8);
        if (dialog > 0x10000 && *reinterpret_cast<const uint32_t*>(dialog) != 0) {
            const uint32_t kind = *reinterpret_cast<const uint32_t*>(dialog + 0xC);
            if (kind == 7 || kind == 8) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// A menu or dialog is open, or closed less than half a second ago (its last frames fade out after the flags clear:
// the start menu's tabs were pulled into the top left group as it closed), or no game is loaded (the title screens).
bool menu_open() {
    if (!player_in_world()) return true;
    static ULONGLONG open_until = 0;
    const ULONGLONG now = GetTickCount64();
    if (menu_flags()) {
        open_until = now + 500;
        return true;
    }
    return now < open_until;
}

bool sane(float v, float lo, float hi) { return v == v && v > lo && v < hi; }

void add_anchor(float x, float y, uint8_t kind) {
    // 0,0: a gauge whose place is not set yet (other players' gauges in the first frames, a Steam Deck log).
    if (x == 0.0f && y == 0.0f) return;
    if (g_anchor_count < 16 && sane(x, -2000.0f, 3280.0f) && sane(y, -2000.0f, 2720.0f)) {
        g_anchors[g_anchor_count++] = Anchor{x, y, kind};
    }
}

#if FPSU_DIAGNOSTICS
std::atomic<int> g_dump_pending{0};  // a boss gauge came up: dump its frame's layout draws (read_anchors)
#endif

// The world marks' places (see the top of this file).
void read_anchors() {
    g_anchor_count = 0;
    g_boss_up = false;
    __try {
        const uintptr_t mm = *reinterpret_cast<const uintptr_t*>(kMenuManVar);
        if (mm < 0x10000) return;
        // A boss's health bar: its handle at MenuMan+0xF64 (a second boss's at +0xF78), -1 none.
        const bool boss = *reinterpret_cast<const uint32_t*>(mm + 0xF64) != 0xFFFFFFFFu ||
                          *reinterpret_cast<const uint32_t*>(mm + 0xF78) != 0xFFFFFFFFu;
        g_boss_up = boss;
#if FPSU_DIAGNOSTICS
        // The first two times one comes up with UIScale on, the layout's draws are logged (a second boss's bar has
        // not been seen yet).
        static int boss_dumps = 0;
        static bool boss_was = false;
        if (boss && !boss_was && boss_dumps < 2 && g_scale < 0.999f) {
            ++boss_dumps;
            g_dump_pending = 1;
        }
        boss_was = boss;
        // The first time another player's gauge is up with UIScaleWorld on, the same (its pieces, 2026-11-14).
        static bool player_dumped = false;
        if (!player_dumped && g_world_scale < 0.999f) {
            for (uintptr_t i = 0; i < 7; ++i) {
                const uintptr_t e = mm + 0x8F4 + i * 0x20;
                const float x = *reinterpret_cast<const float*>(e), y = *reinterpret_cast<const float*>(e + 4);
                if (*reinterpret_cast<const uint8_t*>(e + 0xD) && x > 200.0f && x < 1080.0f && y > 100.0f &&
                    y < 620.0f) {
                    player_dumped = true;
                    g_dump_pending = 2;
                    break;
                }
            }
        }
#endif
        if (*reinterpret_cast<const uint8_t*>(mm + 0x9E4)) {
            add_anchor(*reinterpret_cast<const float*>(mm + 0x9D4), *reinterpret_cast<const float*>(mm + 0x9D8), 0);
        }
        for (uintptr_t i = 0; i < 7; ++i) {
            const uintptr_t e = mm + 0x8F4 + i * 0x20;
            if (*reinterpret_cast<const uint8_t*>(e + 0xD)) {
                add_anchor(*reinterpret_cast<const float*>(e), *reinterpret_cast<const float*>(e + 4), 1);
            }
        }
        for (uintptr_t i = 0; i < 8; ++i) {
            const uintptr_t e = mm + 0xD34 + i * 0x24;
            if (*reinterpret_cast<const uint32_t*>(e) != 0xFFFFFFFFu && *reinterpret_cast<const uint8_t*>(e + 0x20)) {
                add_anchor(*reinterpret_cast<const float*>(e + 4), *reinterpret_cast<const float*>(e + 8), 2);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_anchor_count = 0;
        g_boss_up = false;
    }
}

// The HUD's own top-left pieces, at the game's fixed places: the humanity counter and the bars' left part (x0 up to
// 180, rows 50..146), and the bars' end caps (8 wide, in the two bar rows 71.5..91.5 and 91.5..111.5; the stamina
// bar's cap ends at 110.5 on a Steam Deck, 2026-11-14, so a point either way).
bool hud_top_left_piece(float x0, float y0, float x1, float y1) {
    if (x0 >= 70.0f && x0 <= 180.0f && y0 >= 50.0f && y1 <= 146.0f) return true;
    const bool cap = x1 - x0 <= 8.5f && x0 > 180.0f && x1 <= 1000.0f;
    return cap && ((std::fabs(y0 - 71.5f) < 0.75f && std::fabs(y1 - 91.5f) < 1.25f) ||
                   (std::fabs(y0 - 91.5f) < 0.75f && std::fabs(y1 - 111.5f) < 1.25f));
}

// The world mark a draw belongs to, or null.
// Another player's bar is about 300 units wide (Steam Deck logs, 2026-11-14: its frame 815.8..1115.8 x 234.6..291.6
// about the gauge at 966.3,262.2, the name above it in 243..261, the fills from 830.8 in 268.6..276.6; the gauge's
// place moves a few units against the bar from frame to frame): a fill starts at the bar's left end and shortens with
// the health, so at low health its middle lies more than 110 units from the gauge's place. A player's bar is
// therefore every draw lying wholly in that box about its gauge (160 across, 36 above, 38 below), and nothing else:
// the middle rule of the other marks took the game's multiplayer notices near a player (an icon box at 292..372 x
// 119..223 and its line of text from 390 x 127..155) and drew them shrunk about the player. Not the HUD's own pieces
// (the stamina bar's end cap was taken for one), and nothing larger than a mark (wider than 340 or taller than 100).
constexpr float kPlayerBarHalf = 160.0f;
constexpr float kPlayerBarAbove = 36.0f;
constexpr float kPlayerBarBelow = 38.0f;
int g_logged_wide = 0;
int g_logged_near_miss = 0;

const Anchor* world_mark(float x0, float y0, float x1, float y1) {
    if (!g_anchor_count || hud_top_left_piece(x0, y0, x1, y1)) return nullptr;
    if (x1 - x0 > 340.0f || y1 - y0 > 100.0f) return nullptr;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const Anchor* best = nullptr;
    float best_d = 1e9f;
    bool best_wide = false;
    for (int i = 0; i < g_anchor_count; ++i) {
        const Anchor& a = g_anchors[i];
        const float dx = cx - a.x, dy = cy - a.y;
        const bool near_middle = a.kind != 1 && std::fabs(dx) <= 110.0f && dy >= -90.0f && dy <= 50.0f;
        const bool on_player_bar = a.kind == 1 && x0 >= a.x - kPlayerBarHalf && x1 <= a.x + kPlayerBarHalf &&
                                   y0 >= a.y - kPlayerBarAbove && y1 <= a.y + kPlayerBarBelow;
        if (!near_middle && !on_player_bar) {
            // A draw near another player's gauge that is not taken: which (for the next look at a bar's pieces).
            if (a.kind == 1 && g_logged_near_miss < 12 && std::fabs(dx) <= 260.0f && dy >= -130.0f && dy <= 90.0f) {
                ++g_logged_near_miss;
                LOG_INFO("UIScale: a draw %.1f,%.1f-%.1f,%.1f near another player's gauge at %.1f,%.1f is not its",
                         x0, y0, x1, y1, a.x, a.y);
            }
            continue;
        }
        const float d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = &a;
            best_wide = on_player_bar;
        }
    }
    if (best && best_wide && g_logged_wide < 12) {
        ++g_logged_wide;
        LOG_INFO("UIScale: a draw %.1f,%.1f-%.1f,%.1f is part of another player's bar (gauge at %.1f,%.1f)", x0, y0,
                 x1, y1, best->x, best->y);
    }
    return best;
}

struct Fit {  // x' = ax + (x - ax) * sx, the same in y
    float ax, ay, sx, sy;
    bool any;
};

// The middle of a boss's health bar across (its ends at 375.5 and 1099.1).
constexpr float kBossMiddle = 737.3f;

// What to do with a draw whose corners in the layout are x0..x1, y0..y1.
Fit fit_for(float x0, float y0, float x1, float y1, bool menu) {
    Fit f{0, 0, 1, 1, false};
    if (g_shaped && x0 <= 2.0f && y0 <= 2.0f && x1 >= kLayoutW - 2.0f && y1 >= kLayoutH - 2.0f) {
        // the whole layout: over the whole screen
        f = Fit{kLayoutW * 0.5f, kLayoutH * 0.5f, 1.0f / g_kx, 1.0f / g_ky, true};
        if (!g_logged_full) {
            g_logged_full = true;
            LOG_INFO("FixAspectUI: a box over the whole layout (a fade or a background) is drawn over the whole screen");
        }
        return f;
    }
    if (const Anchor* a = world_mark(x0, y0, x1, y1)) {
        if (g_logged_world < 12) {
            ++g_logged_world;
            static const char* const kKinds[] = {"the lock-on point", "another player's gauge", "an enemy's gauge"};
            LOG_INFO("UIScale: a draw %.1f,%.1f-%.1f,%.1f is a mark of %s at %.1f,%.1f (middle %+.1f, %+.1f from it)%s",
                     x0, y0, x1, y1, kKinds[a->kind < 3 ? a->kind : 0], a->x, a->y, (x0 + x1) * 0.5f - a->x,
                     (y0 + y1) * 0.5f - a->y, g_world_scale < 0.999f ? ", drawn at UIScaleWorld about it" : "");
        }
        if (g_world_scale < 0.999f) f = Fit{a->x, a->y, g_world_scale, g_world_scale, true};
        return f;
    }
    if (g_scale >= 0.999f || menu) return f;
    // The groups, from where the HUD's own pieces are (a dump in the world, 2026-11-14): the humanity counter, the
    // bars and the status icons start at the left (x 70..330, y 30..210) and a long bar's end caps lie further right
    // in the bars' two rows (hud_top_left_piece); the equipment slots, their counts and the item and spell names start
    // at x 86..336 and lie within y 375..677; the soul counter's box is x 1028..1186, y 631..672. A wider band took the
    // start menu's tab icons as it closed (y 60..110 at x 850); a looser bottom left area took the item pickup window's
    // icon (x 395..453, y 497..550). Prompts, boss bars and the pickup window are outside; marks over things in the
    // world are taken out above.
    if (hud_top_left_piece(x0, y0, x1, y1) ||
        (x0 >= 70.0f && x0 <= 330.0f && y0 >= 30.0f && y1 <= 210.0f && x1 <= 1000.0f)) {
        f = Fit{0.0f, 0.0f, g_scale, g_scale, true};
    } else if (x0 >= 80.0f && x0 <= 345.0f && y0 >= 370.0f && y1 <= 685.0f && x1 <= 520.0f) {
        f = Fit{0.0f, kLayoutH, g_scale, g_scale, true};
    } else if (x0 >= 1015.0f && y0 >= 620.0f && y1 <= 685.0f) {
        f = Fit{kLayoutW, kLayoutH, g_scale, g_scale, true};
    } else if (g_boss_up && x0 >= 370.0f && x1 <= 1105.0f && y0 >= 440.0f && y1 <= 632.0f) {
        // A boss's health bar (a dump on the Steam Deck, 2026-11-14): its left end with the name 375..452 x 551..611,
        // the name 400.. x 555..581, the frame 452..1009 x 581..604, the fills from 393 in 586..598, the right end
        // 1007..1099 x 571..626; a second boss's bar above it (not seen yet; the band reaches up to 440). Toward the
        // bottom edge below the bar's middle (x 737), as the bottom groups go toward their corners. Only while a boss
        // gauge is up: the item pickup window and the prompts use the same area (they shrink too if they come up
        // during a boss fight).
        f = Fit{kBossMiddle, kLayoutH, g_scale, g_scale, true};
        if (!g_logged_boss) {
            g_logged_boss = true;
            LOG_INFO("UIScale: a boss's health bar is drawn at %.0f%% (a draw %.1f,%.1f-%.1f,%.1f)", g_scale * 100.0,
                     x0, y0, x1, y1);
        }
    }
    if (f.any && !g_logged_scale) {
        g_logged_scale = true;
        LOG_INFO("UIScale: the HUD's corner groups (bars, equipment slots, soul counter) are drawn at %.0f%%",
                 g_scale * 100.0);
    }
    return f;
}

// The draw's vertices with the fit applied, in g_copy; false when nothing changes (the game's data is drawn).
bool refit(const void* data, UINT stride, UINT count, const float* box_rows, IDirect3DDevice9* dev, bool* rows_changed) {
    *rows_changed = false;
    if (!data || count == 0 || count > 4096 || stride < 8) return false;
    const auto* src = static_cast<const uint8_t*>(data);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    UINT placed = 0;
    for (UINT i = 0; i < count; ++i) {
        const uint8_t* v = src + i * stride;
        float x, y;
        if (g_bound == kImage) {
            int16_t s[2];
            std::memcpy(s, v, 4);
            x = s[0] * 0.125f - 0.5f;
            y = s[1] * 0.125f - 0.5f;
        } else {
            float f[3] = {0, 0, 0};
            std::memcpy(f, v, g_bound == kBox ? 12 : 8);
            if (g_bound == kBox) {
                x = box_rows[0] * f[0] + box_rows[1] * f[1] + box_rows[2] * f[2] + box_rows[3];
                y = box_rows[4] * f[0] + box_rows[5] * f[1] + box_rows[6] * f[2] + box_rows[7];
            } else if (g_bound == kPlain) {
                x = f[0];
                y = f[1];
                // Not layout units after all (another vertex layout): left as the game drew it.
                if (!(x > -5000.0f && x < 5000.0f && y > -5000.0f && y < 5000.0f)) return false;
            } else {
                x = std::floor(f[0]);
                y = std::floor(f[1]);
            }
        }
        // A vertex far off the layout is a placeholder of the game's (another player's bar draws its outline as ten
        // vertices, some at -2596.4,30: a box 3160 wide, which no rule took, so the outline stayed full size); the
        // draw's place is that of the others. It moves with them.
        if (x < -1000.0f || x > kLayoutW + 1000.0f || y < -1000.0f || y > kLayoutH + 1000.0f) continue;
        ++placed;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (placed == 0) return false;
    const Fit f = fit_for(x0, y0, x1, y1, menu_open());
#if FPSU_DIAGNOSTICS
    // The plain kind's draws, for the record (what the outline turned out to be).
    static int plain_logged = 0;
    if (g_bound == kPlain && plain_logged < 16) {
        ++plain_logged;
        uint32_t w[4] = {};
        std::memcpy(w, src, stride >= 16 ? 16 : stride);
        LOG_INFO("UIScale: a plain draw (%u vertices, stride %u) %.1f,%.1f-%.1f,%.1f, first vertex %08X %08X %08X %08X: %s",
                 count, stride, x0, y0, x1, y1, w[0], w[1], w[2], w[3], f.any ? "refit" : "left as it is");
    }
#endif
    if (!f.any) return false;
    if (g_bound == kBox) {
        // through the box's own matrix: X' = ax + (X - ax) * sx on rows c8 and c9
        float rows[8];
        for (int k = 0; k < 4; ++k) {
            rows[k] = box_rows[k] * f.sx;
            rows[4 + k] = box_rows[4 + k] * f.sy;
        }
        rows[3] += f.ax * (1.0f - f.sx);
        rows[7] += f.ay * (1.0f - f.sy);
        o_set_vs_const(dev, 8, rows, 2);
        *rows_changed = true;
        return false;
    }
    g_copy.assign(src, src + static_cast<size_t>(count) * stride);
    for (UINT i = 0; i < count; ++i) {
        uint8_t* v = g_copy.data() + i * stride;
        if (g_bound == kImage) {
            int16_t s[2];
            std::memcpy(s, v, 4);
            const float x = f.ax + (s[0] * 0.125f - 0.5f - f.ax) * f.sx;
            const float y = f.ay + (s[1] * 0.125f - 0.5f - f.ay) * f.sy;
            const float sx = std::floor((x + 0.5f) * 8.0f + 0.5f), sy = std::floor((y + 0.5f) * 8.0f + 0.5f);
            s[0] = static_cast<int16_t>(sx < -32768.0f ? -32768.0f : (sx > 32767.0f ? 32767.0f : sx));
            s[1] = static_cast<int16_t>(sy < -32768.0f ? -32768.0f : (sy > 32767.0f ? 32767.0f : sy));
            std::memcpy(v, s, 4);
        } else {
            float p[2];
            std::memcpy(p, v, 8);
            p[0] = f.ax + (p[0] - f.ax) * f.sx;
            p[1] = f.ay + (p[1] - f.ay) * f.sy;
            std::memcpy(v, p, 8);
        }
    }
    return true;
}

bool layout_draw() { return g_layout && g_bound != kOther && (g_shaped || g_scale < 0.999f || g_world_scale < 0.999f); }

#if FPSU_DIAGNOSTICS
// Draws in the layout's pass that are not refit at all: from a vertex buffer, or with a shader of none of the kinds
// (logged, the first few; what the player bar's outline is if the plain kind is not it).
DrawPrimFn o_draw_prim = nullptr;
DrawIdxFn o_draw_idx = nullptr;
int g_logged_unfit = 0;

void note_unfit(const char* call, D3DPRIMITIVETYPE type, UINT prims) {
    if (!g_layout || g_logged_unfit >= 24) return;
    if (g_scale >= 0.999f && g_world_scale >= 0.999f) return;
    if (!player_in_world()) return;  // the title screen's own full-screen quads
    // Each kind once: the call, the primitive type, the shader kind and the count.
    static uint32_t seen[24] = {};
    const uint32_t sig = (static_cast<uint32_t>(std::strlen(call)) << 24) ^ (static_cast<uint32_t>(type) << 16) ^
                         (static_cast<uint32_t>(g_bound) << 12) ^ (prims & 0xFFF);
    for (int i = 0; i < g_logged_unfit; ++i) {
        if (seen[i] == sig) return;
    }
    seen[g_logged_unfit++] = sig;
    LOG_INFO("UIScale: a %s in the layout's pass is not refit (primitive type %d, %u primitives, shader kind %u)", call,
             static_cast<int>(type), prims, g_bound);
}

HRESULT WINAPI hk_draw_prim(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
    note_unfit("DrawPrimitive", type, prims);
    return o_draw_prim(dev, type, start, prims);
}

HRESULT WINAPI hk_draw_idx(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT verts,
                           UINT start, UINT prims) {
    note_unfit("DrawIndexedPrimitive", type, prims);
    return o_draw_idx(dev, type, base, min_index, verts, start, prims);
}
#endif

#if FPSU_DIAGNOSTICS
// RemoteControl `uidump N`: the next N draws of the layout, with their place, vertex data, texture and the game code
// on the stack (which part of the game drew them).
std::atomic<int> g_dump{0};

bool dump_read(const uintptr_t* p, uintptr_t* v) {
    __try {
        *v = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void dump_draw(IDirect3DDevice9* dev, const void* data, UINT stride, UINT count, const float* box_rows,
               const void* from) {
    if (const int why = g_dump_pending.exchange(0)) {
        LOG_INFO("UIScale: %s came up; the next 300 draws of the layout follow (uidump)",
                 why == 2 ? "another player's health bar" : "a boss's health bar");
        read_anchors();
        for (int i = 0; i < g_anchor_count; ++i) {
            LOG_INFO("uidump: world mark %d (kind %u) at %.1f,%.1f", i, g_anchors[i].kind, g_anchors[i].x,
                     g_anchors[i].y);
        }
        g_dump = 300;
    }
    if (g_dump.load() <= 0) return;
    g_dump.fetch_sub(1);
    const auto* src = static_cast<const uint8_t*>(data);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (UINT i = 0; src && i < count && i < 4096; ++i) {
        const uint8_t* v = src + i * stride;
        float x, y;
        if (g_bound == kImage) {
            int16_t s[2];
            std::memcpy(s, v, 4);
            x = s[0] * 0.125f - 0.5f;
            y = s[1] * 0.125f - 0.5f;
        } else {
            float f[3] = {0, 0, 0};
            std::memcpy(f, v, g_bound == kBox ? 12 : 8);
            if (g_bound == kBox) {
                x = box_rows[0] * f[0] + box_rows[1] * f[1] + box_rows[2] * f[2] + box_rows[3];
                y = box_rows[4] * f[0] + box_rows[5] * f[1] + box_rows[6] * f[2] + box_rows[7];
            } else {
                x = f[0];
                y = f[1];
            }
        }
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    IDirect3DBaseTexture9* tex = nullptr;
    dev->GetTexture(0, &tex);
    if (tex) tex->Release();
    std::string st;
    char b[16];
    const auto* sp = static_cast<const uintptr_t*>(from);
    int found = 0;
    for (int i = 0; i < 600 && found < 14; ++i) {
        uintptr_t a = 0;
        if (!dump_read(sp + i, &a)) break;
        if (a >= 0x00401000 && a < 0x010CC000) {
            std::snprintf(b, sizeof(b), " %X", static_cast<unsigned>(a));
            st += b;
            ++found;
        }
    }
    static const char* const kNames[] = {"other", "image", "text", "box", "plain"};
    LOG_INFO("uidump %s n%u box %.1f,%.1f-%.1f,%.1f data %p tex %p menu %d | stack%s", kNames[g_bound], count, x0, y0,
             x1, y1, data, static_cast<void*>(tex), menu_open() ? 1 : 0, st.c_str());
}
#define DUMP_DRAW(dev, data, stride, count, box) dump_draw(dev, data, stride, count, box, _AddressOfReturnAddress())
#else
#define DUMP_DRAW(dev, data, stride, count, box) ((void)0)
#endif

// Lines too: another player's bar has a white outline drawn as lines, which stayed full size around the shrunk bar
// while lines counted no vertices here (Steam Deck and PC screenshots, 2026-11-14).
UINT vertex_count(D3DPRIMITIVETYPE type, UINT prims) {
    switch (type) {
        case D3DPT_TRIANGLELIST: return prims * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return prims + 2;
        case D3DPT_LINELIST: return prims * 2;
        case D3DPT_LINESTRIP: return prims + 1;
        case D3DPT_POINTLIST: return prims;
        default: return 0;
    }
}

// The game's single-colour rectangles (0xF5A340, 0xF5B000: filled as a triangle fan, framed as a 4-segment line strip;
// vertices of 0x20 bytes, x and y the first two floats, in layout units): another player's bar has its white outline
// so. A line draw of that layout in the layout's pass is refit as plain positions whatever its shader's kind.
bool layout_frame(D3DPRIMITIVETYPE type, UINT stride) {
    return g_layout && g_bound == kOther && stride == 0x20 && (type == D3DPT_LINESTRIP || type == D3DPT_LINELIST) &&
           (g_shaped || g_scale < 0.999f || g_world_scale < 0.999f);
}

HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prims, const void* data, UINT stride) {
#if FPSU_DIAGNOSTICS
    // Every frame of lines of that layout, the first few in the world: in the layout's pass or not, its first corner.
    static int frames_logged = 0;
    if (frames_logged < 8 && data && stride == 0x20 && (type == D3DPT_LINESTRIP || type == D3DPT_LINELIST) &&
        player_in_world()) {
        ++frames_logged;
        float v[4];
        std::memcpy(v, data, sizeof(v));
        LOG_INFO("UIScale: a frame of lines (type %d, %u segments): layout's pass %d, shader kind %u, first corner %.1f, "
                 "%.1f, %.1f, %.1f", static_cast<int>(type), prims, g_layout ? 1 : 0, g_bound, v[0], v[1], v[2], v[3]);
    }
#endif
    if (layout_frame(type, stride)) {
        g_bound = kPlain;
        bool rows = false;
        float box[8];
        std::memcpy(box, g_box, sizeof(box));
        const bool edited = refit(data, stride, vertex_count(type, prims), box, dev, &rows);
        g_bound = kOther;
        static int logged = 0;
        if (logged < 4) {
            ++logged;
            LOG_INFO("UIScale: a frame of lines (%u segments) in the layout's pass, with a shader of no known kind: %s",
                     prims, edited ? "refit" : "left as it is");
        }
        return o_draw_up(dev, type, prims, edited ? g_copy.data() : data, stride);
    }
#if FPSU_DIAGNOSTICS
    if (g_bound == kOther) note_unfit("DrawPrimitiveUP", type, prims);
#endif
    if (!layout_draw()) return o_draw_up(dev, type, prims, data, stride);
    float box[8];
    std::memcpy(box, g_box, sizeof(box));
    DUMP_DRAW(dev, data, stride, vertex_count(type, prims), box);
    bool rows = false;
    const bool edited = refit(data, stride, vertex_count(type, prims), box, dev, &rows);
    const HRESULT hr = o_draw_up(dev, type, prims, edited ? g_copy.data() : data, stride);
    if (rows) o_set_vs_const(dev, 8, box, 2);
    return hr;
}

HRESULT WINAPI hk_draw_idx_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT min_index, UINT verts, UINT prims,
                              const void* indices, D3DFORMAT index_format, const void* data, UINT stride) {
#if FPSU_DIAGNOSTICS
    if (g_bound == kOther || min_index != 0) note_unfit("DrawIndexedPrimitiveUP", type, prims);
#endif
    if (!layout_draw() || min_index != 0) {
        return o_draw_idx_up(dev, type, min_index, verts, prims, indices, index_format, data, stride);
    }
    float box[8];
    std::memcpy(box, g_box, sizeof(box));
    DUMP_DRAW(dev, data, stride, verts, box);
    bool rows = false;
    const bool edited = refit(data, stride, verts, box, dev, &rows);
    const HRESULT hr =
        o_draw_idx_up(dev, type, min_index, verts, prims, indices, index_format, edited ? g_copy.data() : data, stride);
    if (rows) o_set_vs_const(dev, 8, box, 2);
    return hr;
}

HRESULT WINAPI hk_set_scissor(IDirect3DDevice9* dev, const RECT* r) {
    RECT m;
    if (g_own_scissor && r && !g_premapped && layout_map_scissor(dev, r, 1280, 720, &m)) return o_set_scissor(dev, &m);
    return o_set_scissor(dev, r);
}

// The game's menu clipping calls (layout_patch_scissor_sites): `push rect; push device; call [vtable+0x12C]`, the
// call replaced by `call [g_site_fn]` (stdcall, the same stack).
HRESULT WINAPI site_set_scissor(IDirect3DDevice9* dev, const RECT* r) {
    int fw = 0, fh = 0;
    resolution_frame_size(&fw, &fh);
    if (fw <= 0 || fh <= 0) {
        fw = 1280;
        fh = 720;
    }
    RECT m;
    const bool mapped = r && layout_map_scissor(dev, r, fw, fh, &m);
    static int logged = 0;  // what the first calls brought, for the log
    if (logged < 6 && r) {
        ++logged;
        void* slot = (*reinterpret_cast<void***>(dev))[75];
        LOG_INFO("Menu clipping (the game's call): (%ld,%ld-%ld,%ld) %s %ld,%ld-%ld,%ld on a %dx%d frame; the device's "
                 "SetScissorRect %p (ours %p, %p)", r->left, r->top, r->right, r->bottom,
                 mapped ? "mapped to" : "kept as",
                 mapped ? m.left : r->left, mapped ? m.top : r->top, mapped ? m.right : r->right,
                 mapped ? m.bottom : r->bottom, fw, fh, slot, reinterpret_cast<void*>(&hk_set_scissor),
                 o_set_scissor ? reinterpret_cast<void*>(o_set_scissor) : nullptr);
    }
    g_premapped = true;
    const HRESULT hr = dev->SetScissorRect(mapped ? &m : r);
    g_premapped = false;
    return hr;
}
void* g_site_fn = reinterpret_cast<void*>(&site_set_scissor);

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

}  // namespace

void layout_configure(bool keep_shape, int scale_percent) {
    g_keep_shape = keep_shape;
    if (keep_shape) patch_world_points();
    const int p = scale_percent < 50 ? 50 : (scale_percent > 100 ? 100 : scale_percent);
    g_scale = p / 100.0f;
}

void layout_set_scale(int scale_percent) { layout_configure(g_keep_shape, scale_percent); }

void layout_set_world_scale(int scale_percent) {
    const int p = scale_percent < 50 ? 50 : (scale_percent > 100 ? 100 : scale_percent);
    g_world_scale = p / 100.0f;
}

bool layout_scissor_premapped() { return g_premapped; }

void layout_patch_scissor_sites() {
    static bool done = false;
    if (done) return;
    done = true;
    // mov eax, [edx|ecx + 0x12C]; call eax
    struct Site {
        uintptr_t at;
        uint8_t modrm;
    };
    static const Site kSites[] = {{0x00F4A286, 0x82}, {0x00F4A388, 0x81}, {0x00F4A416, 0x82}, {0x00F4A518, 0x81}};
    for (const Site& s : kSites) {
        const auto* p = reinterpret_cast<const uint8_t*>(s.at);
        const uint8_t want[8] = {0x8B, s.modrm, 0x2C, 0x01, 0x00, 0x00, 0xFF, 0xD0};
        if (std::memcmp(p, want, 8) != 0) {
            LOG_ERROR("Menu clipping: the game's call at %08X is not the one this build knows; menu clipping "
                      "rectangles are mapped by the device hook alone", static_cast<unsigned>(s.at));
            return;
        }
    }
    for (const Site& s : kSites) {
        auto* p = reinterpret_cast<uint8_t*>(s.at);
        DWORD old = 0;
        if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) continue;
        uint8_t code[8] = {0xFF, 0x15, 0, 0, 0, 0, 0x90, 0x90};  // call [g_site_fn]; nop; nop
        const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_site_fn));
        std::memcpy(code + 2, &addr, 4);
        std::memcpy(p, code, 8);
        DWORD ignored = 0;
        VirtualProtect(p, 8, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), p, 8);
    }
    LOG_INFO("Menu clipping: the game's 4 clipping calls (0xF4A270..0xF4A450) map their rectangles for the frame "
             "buffer themselves");
}

void layout_dump(int draws) {
#if FPSU_DIAGNOSTICS
    uintptr_t wcm = 0, first = 0, chr = 0;
    const uintptr_t* base = reinterpret_cast<const uintptr_t*>(0x0137DC70);
    if (dump_read(base, &wcm) && wcm && dump_read(reinterpret_cast<const uintptr_t*>(wcm + 4), &first) && first) {
        dump_read(reinterpret_cast<const uintptr_t*>(first), &chr);
    }
    uintptr_t mm = 0;
    dump_read(reinterpret_cast<const uintptr_t*>(kMenuManVar), &mm);
    char fields[400] = "";
    if (mm >= 0x10000) {
        int n = 0;
        for (uint32_t off = 0; off < 0xC0 && n < 380; off += 4) {
            uintptr_t v = 0;
            dump_read(reinterpret_cast<const uintptr_t*>(mm + off), &v);
            if (v) n += std::snprintf(fields + n, sizeof(fields) - n, " +%X=%X", off, static_cast<unsigned>(v));
        }
    }
    LOG_INFO("uidump: WorldChrMan %X first %X player %X; MenuMan %X:%s", static_cast<unsigned>(wcm),
             static_cast<unsigned>(first), static_cast<unsigned>(chr), static_cast<unsigned>(mm), fields);
    read_anchors();
    for (int i = 0; i < g_anchor_count; ++i) {
        LOG_INFO("uidump: world mark %d (kind %u) at %.1f,%.1f", i, g_anchors[i].kind, g_anchors[i].x, g_anchors[i].y);
    }
    g_dump = draws;
#else
    (void)draws;
#endif
}

bool layout_map_scissor(IDirect3DDevice9* dev, const RECT* r, int frame_w, int frame_h, RECT* out) {
    if (!r || r->left < 0 || r->top < 0 || r->right > 1280 || r->bottom > 720) return false;
    shape_now();
    const bool shaped = g_kx != 1.0f || g_ky != 1.0f;
    if (!shaped && frame_w == 1280 && frame_h == 720) return false;
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC d{};
    const bool frame = SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt && SUCCEEDED(rt->GetDesc(&d)) &&
                       static_cast<int>(d.Width) == frame_w && static_cast<int>(d.Height) == frame_h;
    if (rt) rt->Release();
    // The menu's viewport spans the frame buffer (its full width or height). A small pass's own viewport (the eye
    // adaptation's 64x64 and 16x16, which can be set while the frame buffer is still bound) and a rectangle that is
    // just that viewport are left alone.
    D3DVIEWPORT9 vp{};
    if (!frame || FAILED(dev->GetViewport(&vp)) || vp.Width == 0 || vp.Height == 0 ||
        (static_cast<int>(vp.Width) != frame_w && static_cast<int>(vp.Height) != frame_h) ||
        (r->left == static_cast<LONG>(vp.X) && r->top == static_cast<LONG>(vp.Y) &&
         r->right == static_cast<LONG>(vp.X + vp.Width) && r->bottom == static_cast<LONG>(vp.Y + vp.Height))) {
        static int left_alone = 0;  // the first few, for the log
        if (left_alone < 8 && (frame_w != 1280 || frame_h != 720 || shaped) && r->right - r->left > 64) {
            ++left_alone;
            LOG_INFO("Resolution: a clipping rectangle (%ld,%ld-%ld,%ld) is left as it is: the target is %ux%u (the "
                     "frame is %dx%d), the viewport %lu,%lu %lux%lu", r->left, r->top, r->right, r->bottom, d.Width,
                     d.Height, frame_w, frame_h, vp.X, vp.Y, vp.Width, vp.Height);
        }
        return false;
    }
    // 1280x720 onto the viewport, about its middle by kx and ky
    const double sx = vp.Width / 1280.0 * g_kx, sy = vp.Height / 720.0 * g_ky;
    const double ox = vp.X + vp.Width * (1.0 - g_kx) * 0.5, oy = vp.Y + vp.Height * (1.0 - g_ky) * 0.5;
    out->left = static_cast<LONG>(ox + r->left * sx);
    out->top = static_cast<LONG>(oy + r->top * sy);
    out->right = static_cast<LONG>(ox + r->right * sx + 0.999);
    out->bottom = static_cast<LONG>(oy + r->bottom * sy + 0.999);
    static int logged = 0;  // the first few, for the log
    if (logged < 6) {
        ++logged;
        LOG_INFO("Resolution: a menu clipping rectangle (%ld,%ld-%ld,%ld in 1280x720) is mapped to %ld,%ld-%ld,%ld",
                 r->left, r->top, r->right, r->bottom, out->left, out->top, out->right, out->bottom);
    }
    return true;
}

void layout_hook_device(IDirect3DDevice9* dev, bool own_scissor) {
    if (!dev) return;
    g_own_scissor = own_scissor;
    void* p = patch_slot(dev, 91, reinterpret_cast<void*>(&hk_create_vs));
    if (p && !o_create_vs) o_create_vs = reinterpret_cast<CreateVsFn>(p);
    p = patch_slot(dev, 92, reinterpret_cast<void*>(&hk_set_vs));
    if (p && !o_set_vs) o_set_vs = reinterpret_cast<SetVsFn>(p);
    p = patch_slot(dev, 94, reinterpret_cast<void*>(&hk_set_vs_const));
    if (p && !o_set_vs_const) o_set_vs_const = reinterpret_cast<SetVsConstFn>(p);
    p = patch_slot(dev, 83, reinterpret_cast<void*>(&hk_draw_up));
    if (p && !o_draw_up) o_draw_up = reinterpret_cast<DrawUpFn>(p);
    p = patch_slot(dev, 84, reinterpret_cast<void*>(&hk_draw_idx_up));
    if (p && !o_draw_idx_up) o_draw_idx_up = reinterpret_cast<DrawIdxUpFn>(p);
#if FPSU_DIAGNOSTICS
    p = patch_slot(dev, 81, reinterpret_cast<void*>(&hk_draw_prim));
    if (p && !o_draw_prim) o_draw_prim = reinterpret_cast<DrawPrimFn>(p);
    p = patch_slot(dev, 82, reinterpret_cast<void*>(&hk_draw_idx));
    if (p && !o_draw_idx) o_draw_idx = reinterpret_cast<DrawIdxFn>(p);
#endif
    if (own_scissor) {
        p = patch_slot(dev, 75, reinterpret_cast<void*>(&hk_set_scissor));
        if (p && !o_set_scissor) o_set_scissor = reinterpret_cast<SetScissorFn>(p);
    }
    g_layout = false;
    g_shaped = false;
    g_bound = kOther;
}
