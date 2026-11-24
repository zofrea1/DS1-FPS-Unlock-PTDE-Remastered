// AlphaToCoverage. Grass, leaves, hair and fences are alpha-tested: each pixel is either drawn or not by its
// texture's alpha, so their edges are stair-stepped and thin blades far away break up into dots. Multisampling (the
// game's own anti-aliasing: a 2x multisampled scene, switched on by AntialiasingSamples, 4x or 8x) smooths the edges of
// geometry but not these: the test is made once per pixel. Alpha to coverage turns the alpha into the share of the
// pixel's samples covered instead, so those edges come out as smooth as any other.
//
// Direct3D 9 has no switch for it; the graphics drivers added one each: NVIDIA's render state ADAPTIVETESS_Y =
// 'ATOC' (alpha-tested draws use coverage while it is set), AMD's POINTSIZE = 'A2M1' (on) / 'A2M0' (off). DXVK
// understands both. Before each draw, coverage is on when the draw is alpha-tested, not blended, and goes into a
// multisampled target, and off otherwise (the shadow maps, the effects and the HUD are drawn as the game drew them).
//
// Coverage straight from the texture's alpha makes the blades see-through: textures made for an alpha test have
// alpha well below 1 inside the shape (anything above the test's reference was drawn solid). So for such a draw the
// pixel shader is swapped for a copy whose alpha is sharpened around the reference first (Ben Golus, "Anti-aliased
// Alpha Test: The Esoteric Alpha To Coverage", 2017): alpha' = saturate((alpha - ref) / fwidth(alpha) + 0.5). Solid
// inside, the test's own outline, and a ramp one pixel wide across it that the samples turn into a smooth edge. The
// copy is the game's shader with its colour output (oC0) written to a spare temporary register and, at the end, those
// instructions and the move to oC0 added (c210 = ref, a floor for fwidth, 0.5). A shader that cannot be copied this
// way (subroutines, no spare register) gets plain coverage.
#include "atoc.h"

#include "d3d.h"
#include "d3dperf.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <emmintrin.h>
#include <d3d9.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr DWORD kNvOn = MAKEFOURCC('A', 'T', 'O', 'C');
constexpr DWORD kAmdOn = MAKEFOURCC('A', '2', 'M', '1');
constexpr DWORD kAmdOff = MAKEFOURCC('A', '2', 'M', '0');
constexpr UINT kSharpenConst = 210;  // no game pixel shader uses a register above c183; ours use c200-c203

using SetRTFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetPSFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using DrawFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUPFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedUPFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                         const void*, UINT);
SetRTFn o_set_rt = nullptr;
SetPSFn o_set_ps = nullptr;
DrawFn o_draw = nullptr;
DrawIndexedFn o_draw_indexed = nullptr;
DrawUPFn o_draw_up = nullptr;
DrawIndexedUPFn o_draw_indexed_up = nullptr;

enum class Method { None, Nvidia, Amd };
Method g_method = Method::None;
volatile LONG g_mode = 2;     // 0 off, 1 plain coverage, 2 sharpened (RemoteControl `atoc`)
bool g_multisampled = false;  // render target 0 is multisampled
bool g_amd_on = false;        // AMD's switch as we last set it (POINTSIZE does not read back as the switch)
IDirect3DPixelShader9* g_game_ps = nullptr;  // the pixel shader the game bound
bool g_logged_target = false;
bool g_logged_on = false;
// FixAntialiasingDepth (see below): the packed depth's targets are a float (R32F) and the shaders drawing into them
// are given converted copies; render target 0 is one of them.
bool g_depth_float = false;
bool g_depth_target = false;
// RemoteControl `aadepth N` (diagnostics): bit 1 leaves the depth pre-pass's shaders the game's, bit 2 leaves the depth
// copy the game's (comparisons only: either alone gives a wrong depth); bits 4 / 8 raise / lower the alpha test's
// reference by one for the pre-pass's alpha-tested draws; bit 16 leaves the pre-pass's alpha test DXVK's own (below).
volatile LONG g_depth_diag = 0;
// The pre-pass's alpha test made the colour pass's (under DXVK, see pre_pass_kind): DXVK is in use, the alpha test's
// precision of the colour pass's target as DXVK picks it (-1 not seen yet), render target 0 as set last and the depth
// target's size, and whether the colour pass's first alpha-tested draw is still to be seen.
bool g_alpha_exact = false;
int g_colour_prec = -1;
D3DFORMAT g_rt_format = D3DFMT_UNKNOWN;
UINT g_rt_w = 0, g_rt_h = 0, g_depth_w = 0, g_depth_h = 0;
bool g_watch_colour = false;
bool g_logged_exact = false;
constexpr UINT kDepthConst = 211;  // the conversion's constants (a def in the converted copy)
constexpr UINT kAlphaConst = 214;  // the colour pass's alpha test: its factor, threshold, 0.5, 1 (set for each draw)
constexpr UINT kEncodeConst = 212, kDecodeConst = 213;  // the depth copy's re-encoding (defs in its copy)
IDirect3DPixelShader9* g_copy_swap = nullptr;  // the bound shader's re-encoding copy, when it is the depth copy
// The depth resolved into an 8-bit texture of the game's (see hk_stretch_depth).
using StretchFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                   D3DTEXTUREFILTERTYPE);
using SetTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
StretchFn o_stretch = nullptr;
SetTextureFn o_set_texture = nullptr;
IDirect3DTexture9* g_alias = nullptr;           // ours: R32F, the size of the last such copy
IDirect3DSurface9* g_alias_surface = nullptr;   // its level 0
UINT g_alias_w = 0, g_alias_h = 0;
IDirect3DBaseTexture9* g_aliased = nullptr;     // the game's 8-bit texture whose depth is in ours (not held)
IDirect3DSurface9* g_aliased_surface = nullptr; // its level 0 (not held)
LONG g_alias_copies = 0;

// The copies of a game shader (held, so its address is not reused for another), by kind: 1 the float depth, 2 the
// sharpened alpha, 3 both, 4 the depth copy re-encoding the float depth, 5 the float depth with the colour pass's
// alpha test. nullptr: not copyable (or, for 4, not the
// depth copy).
struct Variants {
    IDirect3DPixelShader9* made[6] = {};
    bool tried[6] = {};
};
std::mutex g_lock;
std::unordered_map<IDirect3DPixelShader9*, Variants> g_variants;
// variant_of's answers without the lock: (shader | kind - 1, variant) in one aligned 8-byte word per slot. An answer
// never changes once worked out (g_variants keeps a reference to the shader and is never cleared), so a slot's
// entry stays right until another shader takes the slot. 2026-11-18: variant_of runs on every SetPixelShader while
// FixAntialiasingDepth is on, and before draws into the depth.
__declspec(align(64)) uint64_t g_variant_cache[8192];
LONG g_made = 0, g_refused = 0, g_depth_made = 0, g_depth_refused = 0;

// --- ps_3_0 bytecode -----------------------------------------------------------------------------------------------
constexpr DWORD kEnd = 0x0000FFFF;
constexpr DWORD kRegTemp = 0, kRegConst = 2, kRegColorOut = 8;

DWORD reg_type(DWORD p) { return ((p >> 28) & 7) | ((p >> 8) & 0x18); }
DWORD dst(DWORD type, DWORD num, DWORD mask, bool sat = false) {
    return 0x80000000 | ((type & 7) << 28) | ((type & 0x18) << 8) | (mask << 16) | (sat ? (1u << 20) : 0) | num;
}
DWORD src(DWORD type, DWORD num, DWORD swizzle, DWORD mod = 0) {
    return 0x80000000 | ((type & 7) << 28) | ((type & 0x18) << 8) | (swizzle << 16) | (mod << 24) | num;
}
constexpr DWORD kX = 0x00, kY = 0x55, kZ = 0xAA, kW = 0xFF, kXYZW = 0xE4;
constexpr DWORD kNeg = 1, kAbs = 11;

// The game's ps_3_0 with oC0 written to a spare temporary and the sharpened alpha added at the end. False when the
// shader is not one this can copy.
bool sharpen(const DWORD* code, size_t count, std::vector<DWORD>& out) {
    if (count < 2 || code[0] != 0xFFFF0300) return false;
    int max_temp = -1;
    bool writes = false;
    size_t end = 0;
    for (size_t i = 1; i < count;) {
        const DWORD tok = code[i];
        if (tok == kEnd) {
            end = i;
            break;
        }
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {  // comment
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if (op == 0x19 || op == 0x1A || op == 0x1C || op == 0x1E) return false;  // call, callnz, ret, label
        for (DWORD k = 1; k <= len && i + k < count; ++k) {
            const DWORD p = code[i + k];
            if ((op == 0x51 || op == 0x30 || op == 0x2F) && k > 1) continue;  // def/defi/defb literals
            if (op == 0x1F && k == 1) continue;                                  // dcl's usage token
            const DWORD type = reg_type(p);
            if (type == kRegTemp) max_temp = max_temp > static_cast<int>(p & 0x7FF) ? max_temp : static_cast<int>(p & 0x7FF);
            if (type == kRegColorOut && (p & 0x7FF) == 0) writes = true;
        }
        i += 1 + len;
    }
    if (!end || !writes || max_temp + 2 > 31) return false;
    const DWORD n = static_cast<DWORD>(max_temp + 1), u = static_cast<DWORD>(max_temp + 2);
    out.assign(code, code + end);
    for (size_t i = 1; i < out.size();) {
        const DWORD tok = out[i];
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        for (DWORD k = 1; k <= len && i + k < out.size(); ++k) {
            DWORD& p = out[i + k];
            if ((op == 0x51 || op == 0x30 || op == 0x2F) && k > 1) continue;
            if (op == 0x1F && k == 1) continue;
            if (reg_type(p) == kRegColorOut && (p & 0x7FF) == 0) {
                // oC0 -> rN, keeping the write mask and modifiers.
                p = (p & ~(0x70000000u | 0x00001800u | 0x7FFu)) | n;
            }
        }
        i += 1 + len;
    }
    const DWORD tail[] = {
        0x0200005B, dst(kRegTemp, u, 1), src(kRegTemp, n, kW),                                  // dsx u.x, n.w
        0x0200005C, dst(kRegTemp, u, 2), src(kRegTemp, n, kW),                                  // dsy u.y, n.w
        0x03000002, dst(kRegTemp, u, 1), src(kRegTemp, u, kX, kAbs), src(kRegTemp, u, kY, kAbs),  // u.x = fwidth
        0x0300000B, dst(kRegTemp, u, 1), src(kRegTemp, u, kX), src(kRegConst, kSharpenConst, kY), // max with floor
        0x02000006, dst(kRegTemp, u, 1), src(kRegTemp, u, kX),                                  // rcp
        0x03000002, dst(kRegTemp, u, 2), src(kRegTemp, n, kW), src(kRegConst, kSharpenConst, kX, kNeg),  // a - ref
        0x04000004, dst(kRegTemp, n, 8, true), src(kRegTemp, u, kY), src(kRegTemp, u, kX),
        src(kRegConst, kSharpenConst, kZ),                                                       // sat(.. + 0.5)
        0x02000001, dst(kRegColorOut, 0, 0xF), src(kRegTemp, n, kXYZW),                        // mov oC0, rN
    };
    out.insert(out.end(), std::begin(tail), std::end(tail));
    out.push_back(kEnd);
    return true;
}

DWORD float_bits(float f) {
    DWORD d = 0;
    std::memcpy(&d, &f, sizeof(d));
    return d;
}

// The game's ps_3_0 drawing the packed depth, writing it as one float instead (FixAntialiasingDepth): oC0 written to
// a spare temporary rN (the game's red, green and blue digits of the depth, and its alpha), then at the end
// red + green / 256 + blue / 65536 - (1/256 + 1/65536) to red, green and blue of oC0 and the alpha as it was. The
// game's readers decode dot(rgb, (255/256, 255/65536, 255/16777216)), and a float target reads back (x, 1, 1), so
// they get the depth the digits held, whole. False when the shader is not one this can copy.
// exact_alpha (kind 5): the alpha becomes the colour pass's alpha test already made, 1 (drawn) or 0, from c214 =
// (F, T, 0.5, 1): round(alpha * F) - T >= 0, T half a step off the reference (see pre_pass_kind).
bool depth_float(const DWORD* code, size_t count, std::vector<DWORD>& out, bool exact_alpha = false) {
    if (count < 2 || code[0] != 0xFFFF0300) return false;
    int max_temp = -1;
    bool writes = false;
    size_t end = 0;
    for (size_t i = 1; i < count;) {
        const DWORD tok = code[i];
        if (tok == kEnd) {
            end = i;
            break;
        }
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if (op == 0x19 || op == 0x1A || op == 0x1C || op == 0x1E) return false;  // call, callnz, ret, label
        for (DWORD k = 1; k <= len && i + k < count; ++k) {
            const DWORD p = code[i + k];
            if ((op == 0x51 || op == 0x30 || op == 0x2F) && k > 1) continue;
            if (op == 0x1F && k == 1) continue;
            const DWORD type = reg_type(p);
            if (type == kRegTemp) max_temp = max_temp > static_cast<int>(p & 0x7FF) ? max_temp : static_cast<int>(p & 0x7FF);
            if (type == kRegConst && ((p & 0x7FF) == kDepthConst || (p & 0x7FF) == kAlphaConst)) return false;
            if (type == kRegColorOut && (p & 0x7FF) == 0) writes = true;
        }
        i += 1 + len;
    }
    if (!end || !writes || max_temp + (exact_alpha ? 2 : 1) > 31) return false;
    const DWORD n = static_cast<DWORD>(max_temp + 1), u = static_cast<DWORD>(max_temp + 2);
    out.clear();
    out.push_back(code[0]);
    const DWORD def[] = {0x05000051, dst(kRegConst, kDepthConst, 0xF), float_bits(1.0f), float_bits(1.0f / 256.0f),
                         float_bits(1.0f / 65536.0f), float_bits(-(1.0f / 256.0f + 1.0f / 65536.0f))};
    out.insert(out.end(), std::begin(def), std::end(def));
    const size_t body = out.size();
    out.insert(out.end(), code + 1, code + end);
    for (size_t i = body; i < out.size();) {
        const DWORD tok = out[i];
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        for (DWORD k = 1; k <= len && i + k < out.size(); ++k) {
            DWORD& p = out[i + k];
            if ((op == 0x51 || op == 0x30 || op == 0x2F) && k > 1) continue;
            if (op == 0x1F && k == 1) continue;
            if (reg_type(p) == kRegColorOut && (p & 0x7FF) == 0) {
                // oC0 -> rN, keeping the write mask; full precision (a _pp here would round the digits to 16 bits).
                p = (p & ~(0x70000000u | 0x00001800u | 0x7FFu | 0x00200000u)) | n;
            }
        }
        i += 1 + len;
    }
    const DWORD tail[] = {
        0x03000008, dst(kRegTemp, n, 1), src(kRegTemp, n, kXYZW), src(kRegConst, kDepthConst, kXYZW),  // dp3 n.x
        0x03000002, dst(kRegColorOut, 0, 7), src(kRegTemp, n, kX), src(kRegConst, kDepthConst, kW),     // + offset
    };
    out.insert(out.end(), std::begin(tail), std::end(tail));
    if (exact_alpha) {
        // DXVK's own steps (round(alpha * F), compared with the reference in the same units), the multiply and the
        // rounding apart as there; round half up for its half to even (alpha * F lands on a half only by accident).
        const DWORD A = kAlphaConst;
        const DWORD test[] = {
            0x03000005, dst(kRegTemp, u, 1), src(kRegTemp, n, kW), src(kRegConst, A, kX),              // a * F
            0x03000002, dst(kRegTemp, u, 1), src(kRegTemp, u, kX), src(kRegConst, A, kZ),              // + 0.5
            0x02000013, dst(kRegTemp, u, 2), src(kRegTemp, u, kX),                                     // frc
            0x03000002, dst(kRegTemp, u, 1), src(kRegTemp, u, kX), src(kRegTemp, u, kY, kNeg),         // rounded
            0x03000002, dst(kRegTemp, u, 1), src(kRegTemp, u, kX), src(kRegConst, A, kY, kNeg),        // - T
            0x04000058, dst(kRegColorOut, 0, 8, true), src(kRegTemp, u, kX), src(kRegConst, A, kW),
            src(kRegConst, A, kW, kNeg),                                                               // 1 or 0
        };
        out.insert(out.end(), std::begin(test), std::end(test));
    } else {
        const DWORD alpha[] = {0x02000001, dst(kRegColorOut, 0, 8), src(kRegTemp, n, kW)};             // alpha
        out.insert(out.end(), std::begin(alpha), std::end(alpha));
    }
    out.push_back(kEnd);
    return true;
}

// The game's depth copy (FRPG_Fil_DepthCopy, ps_599aa31f: texld r0 ; dp3 oDepth, r0, (the decode) ; mov oC0, r0)
// copies the packed depth's texel into the 512x360 colour target that the low-resolution effects (their soft edges)
// and their upsample read as packed depth. From the float target that texel is (x, 1, 1): its copy, oC0 written to a
// spare rN and at the end the depth decoded from rN and encoded again in the game's three digits (the pre-pass
// shaders' own steps: R = floor(d * 255.999985), G = floor(rest * 256), B the rest, over 255 and 255/256), so the
// copy holds what it held from the 8-bit target. False for any other shader.
bool depth_copy_encode(const DWORD* code, size_t count, std::vector<DWORD>& out) {
    if (count < 2 || code[0] != 0xFFFF0300) return false;
    constexpr DWORD kRegDepthOut = 9;
    int max_temp = -1;
    int moved = -1, decoded = -1;
    size_t end = 0;
    for (size_t i = 1; i < count;) {
        const DWORD tok = code[i];
        if (tok == kEnd) {
            end = i;
            break;
        }
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if (op == 0x19 || op == 0x1A || op == 0x1C || op == 0x1E) return false;
        if (i + len >= count) return false;
        if (op == 0x01 && len == 2 && reg_type(code[i + 1]) == kRegColorOut && (code[i + 1] & 0x7FF) == 0 &&
            ((code[i + 1] >> 16) & 0xF) == 0xF && reg_type(code[i + 2]) == kRegTemp &&
            ((code[i + 2] >> 16) & 0xFF) == kXYZW && ((code[i + 2] >> 24) & 0xF) == 0) {
            moved = static_cast<int>(code[i + 2] & 0x7FF);  // mov oC0, rK
        }
        if (op == 0x08 && len == 3 && reg_type(code[i + 1]) == kRegDepthOut && reg_type(code[i + 2]) == kRegTemp) {
            decoded = static_cast<int>(code[i + 2] & 0x7FF);  // dp3 oDepth, rK, c
        }
        for (DWORD k = 1; k <= len; ++k) {
            const DWORD p = code[i + k];
            if ((op == 0x51 || op == 0x30 || op == 0x2F) && k > 1) continue;
            if (op == 0x1F && k == 1) continue;
            const DWORD type = reg_type(p);
            if (type == kRegTemp) max_temp = max_temp > static_cast<int>(p & 0x7FF) ? max_temp : static_cast<int>(p & 0x7FF);
            if (type == kRegConst && ((p & 0x7FF) == kEncodeConst || (p & 0x7FF) == kDecodeConst)) return false;
        }
        i += 1 + len;
    }
    if (!end || moved < 0 || moved != decoded || max_temp + 2 > 31) return false;
    const DWORD n = static_cast<DWORD>(max_temp + 1), u = static_cast<DWORD>(max_temp + 2);
    out.clear();
    out.push_back(code[0]);
    const DWORD defs[] = {
        0x05000051, dst(kRegConst, kEncodeConst, 0xF), float_bits(255.999985f), float_bits(256.0f),
        float_bits(1.0f / 255.0f), float_bits(256.0f / 255.0f),
        0x05000051, dst(kRegConst, kDecodeConst, 0xF), float_bits(0.99609375f), float_bits(0.00389099121f),
        float_bits(1.51991844e-005f), float_bits(0.0f),
    };
    out.insert(out.end(), std::begin(defs), std::end(defs));
    const size_t body = out.size();
    out.insert(out.end(), code + 1, code + end);
    for (size_t i = body; i < out.size();) {
        const DWORD tok = out[i];
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        for (DWORD k = 1; k <= len && i + k < out.size(); ++k) {
            DWORD& p = out[i + k];
            if ((op == 0x51 || op == 0x30 || op == 0x2F) && k > 1) continue;
            if (op == 0x1F && k == 1) continue;
            if (reg_type(p) == kRegColorOut && (p & 0x7FF) == 0) {
                p = (p & ~(0x70000000u | 0x00001800u | 0x7FFu | 0x00200000u)) | n;
            }
        }
        i += 1 + len;
    }
    const DWORD E = kEncodeConst;
    const DWORD tail[] = {
        0x03000008, dst(kRegTemp, u, 1), src(kRegTemp, n, kXYZW), src(kRegConst, kDecodeConst, kXYZW),  // d
        0x03000005, dst(kRegTemp, u, 2), src(kRegTemp, u, kX), src(kRegConst, E, kX),                   // d * 255.99
        0x02000013, dst(kRegTemp, u, 4), src(kRegTemp, u, kY),                                         // frc
        0x03000002, dst(kRegTemp, u, 2), src(kRegTemp, u, kY), src(kRegTemp, u, kZ, kNeg),            // R
        0x04000004, dst(kRegTemp, u, 1), src(kRegTemp, u, kX), src(kRegConst, E, kX),
        src(kRegTemp, u, kY, kNeg),                                                                    // rest
        0x03000005, dst(kRegTemp, u, 4), src(kRegTemp, u, kX), src(kRegConst, E, kY),                   // * 256
        0x02000013, dst(kRegTemp, u, 8), src(kRegTemp, u, kZ),                                         // frc
        0x03000002, dst(kRegTemp, u, 4), src(kRegTemp, u, kZ), src(kRegTemp, u, kW, kNeg),            // G
        0x04000004, dst(kRegTemp, u, 8), src(kRegTemp, u, kX), src(kRegConst, E, kY),
        src(kRegTemp, u, kZ, kNeg),                                                                    // B
        0x03000005, dst(kRegColorOut, 0, 1), src(kRegTemp, u, kY), src(kRegConst, E, kZ),              // R / 255
        0x03000005, dst(kRegColorOut, 0, 2), src(kRegTemp, u, kZ), src(kRegConst, E, kZ),              // G / 255
        0x03000005, dst(kRegColorOut, 0, 4), src(kRegTemp, u, kW), src(kRegConst, E, kW),              // B * 256/255
        0x02000001, dst(kRegColorOut, 0, 8), src(kRegTemp, n, kW),                                     // alpha
    };
    out.insert(out.end(), std::begin(tail), std::end(tail));
    out.push_back(kEnd);
    return true;
}

// kind: 1 the float depth, 2 the sharpened alpha, 3 both (the float depth's copy, sharpened), 4 the depth copy, 5 the
// float depth with the colour pass's alpha test.
IDirect3DPixelShader9* variant_slow(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, int kind);

IDirect3DPixelShader9* variant_of(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, int kind) {
    if (reinterpret_cast<uintptr_t>(ps) & 7u) return variant_slow(dev, ps, kind);  // the kind goes in those bits
    const uint32_t key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ps)) | static_cast<uint32_t>(kind - 1);
    uint64_t* slot = &g_variant_cache[(key * 2654435761u) >> 19];
    uint64_t e;  // one 8-byte read (movq)
    _mm_storel_epi64(reinterpret_cast<__m128i*>(&e), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(slot)));
    if (static_cast<uint32_t>(e) == key) return reinterpret_cast<IDirect3DPixelShader9*>(static_cast<uintptr_t>(e >> 32));
    IDirect3DPixelShader9* made = variant_slow(dev, ps, kind);
    const uint64_t n = key | (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(made)) << 32);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(slot), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&n)));
    return made;
}

IDirect3DPixelShader9* variant_slow(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, int kind) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_variants.find(ps);
    if (it == g_variants.end()) {
        ps->AddRef();
        it = g_variants.emplace(ps, Variants{}).first;
    }
    Variants& v = it->second;
    if (v.tried[kind]) return v.made[kind];
    v.tried[kind] = true;
    IDirect3DPixelShader9* made = nullptr;
    UINT size = 0;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size % 4 == 0) {
        std::vector<DWORD> code(size / 4), a, b;
        bool ok = SUCCEEDED(ps->GetFunction(code.data(), &size));
        const std::vector<DWORD>* final = &a;
        if (ok && kind == 2) ok = sharpen(code.data(), code.size(), a);
        if (ok && kind == 1) ok = depth_float(code.data(), code.size(), a);
        if (ok && kind == 3) {
            ok = depth_float(code.data(), code.size(), a) && sharpen(a.data(), a.size(), b);
            final = &b;
        }
        if (ok && kind == 4) ok = depth_copy_encode(code.data(), code.size(), a);
        if (ok && kind == 5) ok = depth_float(code.data(), code.size(), a, true);
        if (!ok || FAILED(dev->CreatePixelShader(final->data(), &made))) made = nullptr;
    }
    v.made[kind] = made;
    if (kind == 4) {
        if (made) LOG_INFO("FixAntialiasingDepth: the game's depth copy (into the effects' low-resolution depth) writes "
                           "the float depth in the game's three digits");
        return made;
    }
    if (kind == 2) {
        ++(made ? g_made : g_refused);
        if (g_made + g_refused <= 3 || (g_made + g_refused) % 50 == 0) {
            LOG_INFO("AlphaToCoverage: %ld alpha-tested pixel shaders sharpened, %ld given plain coverage", g_made,
                     g_refused);
        }
    } else {
        ++(made ? g_depth_made : g_depth_refused);
        if (!made || g_depth_made + g_depth_refused <= 2 || (g_depth_made + g_depth_refused) % 10 == 0) {
            LOG_INFO("FixAntialiasingDepth: %ld shaders drawing the depth write it as a float, %ld could not%s",
                     g_depth_made, g_depth_refused, made ? "" : " (that one's surfaces get a wrong depth)");
        }
    }
    return made;
}

// A render state as the device holds it: from the render-state filter when it knows (three of these were read back
// from Direct3D for every draw into the multisampled scene, some 7,000 calls a frame in a busy view), else asked.
DWORD render_state(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state) {
    unsigned long v = 0;
    if (d3dperf_render_state(static_cast<unsigned long>(state), &v)) return v;
    DWORD d = 0;
    dev->GetRenderState(state, &d);
    return d;
}

// DXVK's alpha test precision for a render target format (D3D9DeviceEx::GetAlphaTestPrecision, DXVK 2.x and 3.x): the
// bits above 8 the alpha and the reference are compared in, 0xF a float comparison.
int alpha_precision(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A2B10G10R10: case D3DFMT_A2R10G10B10: case D3DFMT_A2W10V10U10: return 2;
    case D3DFMT_R16F: case D3DFMT_G16R16F: case D3DFMT_A16B16G16R16F: return 7;
    case D3DFMT_G16R16: case D3DFMT_A16B16G16R16: case D3DFMT_V16U16: case D3DFMT_L16: case D3DFMT_Q16W16V16U16:
        return 8;
    case D3DFMT_R32F: case D3DFMT_G32R32F: case D3DFMT_A32B32G32R32F: return 0xF;
    default: return 0;
    }
}

// The shader kind for a draw into the depth (pre-pass): 5 when its alpha test has to be the colour pass's (constants
// set), else 1. Under DXVK the alpha test is made at the precision of the render target's format: round(alpha * 255)
// against the reference for the game's 8-bit targets, round(alpha * 1023) against it times about 4 for the 10-bit
// scene of HighPrecisionColor, but alpha against reference / 255 as floats for our R32F depth. So a pixel whose
// alpha was just above the reference passed in the pre-pass and failed in the colour pass: the pre-pass wrote its
// depth, the colour pass (depth test less-or-equal, no depth writes) then drew neither it nor the surface behind it,
// and the pixel kept the scene's clear colour: grey specks on the hollows' robes and the Asylum's floor that crawled
// while anything moved (2026-11-22: the Steam Deck, where DXVK offers neither coverage switch, and AlphaToCoverage
// off; the game's 8-bit pre-pass, stricter than the 10-bit colour pass, never left any). With coverage DXVK makes no
// alpha test; native Direct3D 9 compares the same way for every format (no specks there, checked).
int pre_pass_kind(IDirect3DDevice9* dev, bool coverage) {
    if (!g_alpha_exact || coverage || (g_depth_diag & 16)) return 1;
    const int p = g_colour_prec;
    if (p < 0 || p > 8) return 1;  // not seen yet, or a float colour target (the same comparison as ours)
    if (!render_state(dev, D3DRS_ALPHATESTENABLE)) return 1;
    const DWORD func = render_state(dev, D3DRS_ALPHAFUNC);
    if (func != D3DCMP_GREATER && func != D3DCMP_GREATEREQUAL) return 1;  // the game's: GREATER 128
    const uint32_t ref = render_state(dev, D3DRS_ALPHAREF) & 0xFF;
    const uint32_t ref_int = (ref << p) | (ref >> (8 - p));
    const float c[4] = {static_cast<float>((256u << p) - 1u),
                        static_cast<float>(ref_int) + (func == D3DCMP_GREATER ? 0.5f : -0.5f), 0.5f, 1.0f};
    dev->SetPixelShaderConstantF(kAlphaConst, c, 1);
    if (!g_logged_exact) {
        g_logged_exact = true;
        LOG_INFO("FixAntialiasingDepth: under DXVK the depth pre-pass's alpha test is made as the colour pass's (%d-bit "
                 "target, reference %u), so no pixel is drawn by one and not the other",
                 8 + p, static_cast<unsigned>(ref));
    }
    return 5;
}

// The colour pass's target: render target 0 of the first alpha-tested draw after the pre-pass the size of the depth.
void watch_colour(IDirect3DDevice9* dev) {
    if (g_depth_target) {
        g_watch_colour = true;
        return;
    }
    if (!g_watch_colour || g_rt_w != g_depth_w || g_rt_h != g_depth_h) return;
    if (!render_state(dev, D3DRS_ALPHATESTENABLE)) return;
    g_watch_colour = false;
    g_colour_prec = alpha_precision(g_rt_format);
}

// Before a draw: coverage on or off, and the sharpened shader in place (true: the game's goes back after the draw).
bool before_draw(IDirect3DDevice9* dev) {
    if (g_copy_swap && o_set_ps && !(g_depth_diag & 2)) {
        o_set_ps(dev, g_copy_swap);
        return true;
    }
    if (g_alpha_exact) watch_colour(dev);
    const LONG mode = g_mode;
    bool want = false;
    const bool depth = g_depth_float && g_depth_target && !(g_depth_diag & 1);
    if (g_method == Method::None) {
        if (!depth || !g_game_ps || !o_set_ps) return false;
        const int kind = pre_pass_kind(dev, false);
        IDirect3DPixelShader9* conv = variant_of(dev, g_game_ps, kind);
        if (!conv && kind != 1) conv = variant_of(dev, g_game_ps, 1);
        if (!conv) return false;
        o_set_ps(dev, conv);
        return true;
    }
    if (g_multisampled && mode) {
        want = render_state(dev, D3DRS_ALPHATESTENABLE) && !render_state(dev, D3DRS_ALPHABLENDENABLE);
    }
    if (g_method == Method::Nvidia) {
        // As the device holds it each time: a state block the game applies may have changed it.
        const DWORD now = render_state(dev, D3DRS_ADAPTIVETESS_Y);
        if ((now == kNvOn) != want) dev->SetRenderState(D3DRS_ADAPTIVETESS_Y, want ? kNvOn : D3DFMT_UNKNOWN);
    } else if (want != g_amd_on) {
        dev->SetRenderState(D3DRS_POINTSIZE, want ? kAmdOn : kAmdOff);
        g_amd_on = want;
    }
    if (want && !g_logged_on) {
        g_logged_on = true;
        LOG_INFO("AlphaToCoverage: alpha-tested surfaces now write coverage (first draw)");
    }
    if (!g_game_ps || !o_set_ps) return false;
    bool sharpen_it = want && mode == 2;
    if (sharpen_it) {
        const DWORD func = render_state(dev, D3DRS_ALPHAFUNC);
        sharpen_it = func == D3DCMP_GREATEREQUAL || func == D3DCMP_GREATER;
    }
    if (!sharpen_it && !depth) return false;
    const int depth_kind = depth && !sharpen_it ? pre_pass_kind(dev, want) : 1;
    IDirect3DPixelShader9* copy =
        variant_of(dev, g_game_ps, depth_kind == 5 ? 5 : (depth ? 1 : 0) | (sharpen_it ? 2 : 0));
    if (!copy && depth_kind == 5) copy = variant_of(dev, g_game_ps, 1);
    if (!copy && depth && sharpen_it) {
        copy = variant_of(dev, g_game_ps, 1);  // the depth right, the edge plain coverage
        sharpen_it = false;
    }
    if (!copy) return false;
    if (sharpen_it) {
        const DWORD ref = render_state(dev, D3DRS_ALPHAREF);
        const float c[4] = {static_cast<float>(ref & 0xFF) / 255.0f, 1.0f / 1024.0f, 0.5f, 0.0f};
        dev->SetPixelShaderConstantF(kSharpenConst, c, 1);
    }
    o_set_ps(dev, copy);
    return true;
}

void after_draw(IDirect3DDevice9* dev, bool swapped) {
    if (swapped) o_set_ps(dev, g_game_ps);
}

// aadepth 4 / 8 (diagnostics): the pre-pass's alpha reference one step stricter / looser around the draw.
DWORD g_diag_ref = 0;
bool g_diag_ref_set = false;
void diag_ref_begin(IDirect3DDevice9* dev) {
    g_diag_ref_set = false;
    const LONG d = g_depth_diag;
    if (!(d & 12) || !g_depth_target || !render_state(dev, D3DRS_ALPHATESTENABLE)) return;
    g_diag_ref = render_state(dev, D3DRS_ALPHAREF);
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG_INFO("aadepth: pre-pass alpha test ref %lu func %lu", static_cast<unsigned long>(g_diag_ref),
                 static_cast<unsigned long>(render_state(dev, D3DRS_ALPHAFUNC)));
    }
    const DWORD r = (d & 4) ? (g_diag_ref < 255 ? g_diag_ref + 1 : 255) : (g_diag_ref ? g_diag_ref - 1 : 0);
    dev->SetRenderState(D3DRS_ALPHAREF, r);
    g_diag_ref_set = true;
}
void diag_ref_end(IDirect3DDevice9* dev) {
    if (g_diag_ref_set) dev->SetRenderState(D3DRS_ALPHAREF, g_diag_ref);
    g_diag_ref_set = false;
}

HRESULT WINAPI hk_set_rt(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* rt) {
    if (rt && rt == g_aliased_surface) {  // the game draws into the texture whose depth was in ours
        g_aliased = nullptr;
        g_aliased_surface = nullptr;
    }
    if (index == 0) {
        D3DSURFACE_DESC d{};
        const bool got = rt && SUCCEEDED(rt->GetDesc(&d));
        g_multisampled = got && d.MultiSampleType != D3DMULTISAMPLE_NONE;
        // The game's own R32F targets are 64x64 and smaller (its exposure); the depth's are the scene's size.
        g_depth_target = got && d.Format == D3DFMT_R32F && d.Width >= 256;
        g_rt_format = got ? d.Format : D3DFMT_UNKNOWN;
        g_rt_w = got ? d.Width : 0;
        g_rt_h = got ? d.Height : 0;
        if (g_depth_target) {
            g_depth_w = d.Width;
            g_depth_h = d.Height;
        }
        if (g_multisampled && !g_logged_target) {
            g_logged_target = true;
            const int samples = d.MultiSampleType == D3DMULTISAMPLE_NONMASKABLE ? 0 : static_cast<int>(d.MultiSampleType);
            LOG_INFO("AlphaToCoverage: the game's anti-aliasing draws the scene %ux%u with %dx multisampling (quality "
                     "%lu); its alpha-tested surfaces get coverage", d.Width, d.Height, samples,
                     static_cast<unsigned long>(d.MultiSampleQuality));
        }
    }
    return o_set_rt(dev, index, rt);
}

HRESULT WINAPI hk_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    g_game_ps = ps;
    g_copy_swap = g_depth_float && ps ? variant_of(dev, ps, 4) : nullptr;
    return o_set_ps(dev, ps);
}

HRESULT WINAPI hk_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    const bool s = before_draw(dev);
    diag_ref_begin(dev);
    const HRESULT hr = o_draw(dev, type, start, count);
    diag_ref_end(dev);
    after_draw(dev, s);
    return hr;
}

HRESULT WINAPI hk_draw_indexed(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT vertices,
                               UINT start, UINT count) {
    const bool s = before_draw(dev);
    diag_ref_begin(dev);
    const HRESULT hr = o_draw_indexed(dev, type, base, min_index, vertices, start, count);
    diag_ref_end(dev);
    after_draw(dev, s);
    return hr;
}

HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    const bool s = before_draw(dev);
    const HRESULT hr = o_draw_up(dev, type, count, data, stride);
    after_draw(dev, s);
    return hr;
}

HRESULT WINAPI hk_draw_indexed_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT min_index, UINT vertices,
                                  UINT count, const void* indices, D3DFORMAT format, const void* data, UINT stride) {
    const bool s = before_draw(dev);
    const HRESULT hr = o_draw_indexed_up(dev, type, min_index, vertices, count, indices, format, data, stride);
    after_draw(dev, s);
    return hr;
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

template <typename Fn>
void hook(IDirect3DDevice9* dev, int slot, void* ours, Fn& original) {
    void* p = patch_slot(dev, slot, ours);
    if (p && !original) original = reinterpret_cast<Fn>(p);
}

// FixAntialiasingDepth: the depth resolved into an 8-bit texture. The game resolves the depth (the float target)
// into T12 for its readers, but where T12 is already taken in that frame it takes another texture of the screen's
// size from its pool, an 8-bit one: the depth of field's copy did so wherever a water surface in view had read the
// depth first (2026-11-18, Undead Burg, the invader's view onto a roof with water in it). From the float target the
// copy into 8 bits kept the depth only to 1/256 (z/w steps 6.4 m, 12.8 m and then infinity at the near plane of
// 0.05 m), so the depth of field blurred everything past about 13 m behind a hard line. The game's own 8-bit digits
// survive such a copy, so the game as shipped is right. The copy now also goes into a float texture of ours of that
// size, which the game's readers get in that texture's place until the game draws into it or copies something else
// into it: the depth they decode is the one T12 would have held.

void alias_release() {
    if (g_alias_surface) g_alias_surface->Release();
    if (g_alias) g_alias->Release();
    g_alias_surface = nullptr;
    g_alias = nullptr;
    g_alias_w = g_alias_h = 0;
    g_aliased = nullptr;
    g_aliased_surface = nullptr;
}

HRESULT WINAPI hk_stretch_depth(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
                                const RECT* dr, D3DTEXTUREFILTERTYPE filter) {
    if (g_depth_float && src && dst) {
        if (dst == g_aliased_surface) {  // something else goes into it now
            g_aliased = nullptr;
            g_aliased_surface = nullptr;
        }
        D3DSURFACE_DESC s{}, d{};
        if (SUCCEEDED(src->GetDesc(&s)) && s.Format == D3DFMT_R32F && s.Width >= 256 && SUCCEEDED(dst->GetDesc(&d)) &&
            d.Format != D3DFMT_R32F && d.Width == s.Width && d.Height == s.Height) {
            IDirect3DTexture9* tex = nullptr;
            if (SUCCEEDED(dst->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) && tex) {
                if (!g_alias || g_alias_w != d.Width || g_alias_h != d.Height) {
                    alias_release();
                    if (SUCCEEDED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                                     D3DPOOL_DEFAULT, &g_alias, nullptr)) &&
                        g_alias && SUCCEEDED(g_alias->GetSurfaceLevel(0, &g_alias_surface))) {
                        g_alias_w = d.Width;
                        g_alias_h = d.Height;
                    } else {
                        alias_release();
                    }
                }
                if (g_alias_surface && SUCCEEDED(o_stretch(dev, src, sr, g_alias_surface, dr, filter))) {
                    g_aliased = tex;
                    g_aliased_surface = dst;
                    if (InterlockedIncrement(&g_alias_copies) == 1) {
                        LOG_INFO("FixAntialiasingDepth: the depth was resolved into an 8-bit %ux%u texture (the usual "
                                 "one was taken in that frame); a float copy is read in its place, so the depth of "
                                 "field and the effects get the whole depth there too", d.Width, d.Height);
                    }
                }
                tex->Release();
            }
        }
    }
    return o_stretch(dev, src, sr, dst, dr, filter);
}

HRESULT WINAPI hk_set_texture_depth(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
    if (tex && tex == g_aliased && g_alias) return o_set_texture(dev, stage, g_alias);
    return o_set_texture(dev, stage, tex);
}

// The game's multisample types by its setting (0x10D38CC: none, 2x, 4x, ...); its Antialiasing option uses entry 1.
constexpr uintptr_t kMsaaTable = 0x010D38CC;

void install_hooks(IDirect3DDevice9* dev) {
    hook(dev, 37, reinterpret_cast<void*>(&hk_set_rt), o_set_rt);
    hook(dev, 107, reinterpret_cast<void*>(&hk_set_ps), o_set_ps);
    hook(dev, 81, reinterpret_cast<void*>(&hk_draw), o_draw);
    hook(dev, 82, reinterpret_cast<void*>(&hk_draw_indexed), o_draw_indexed);
    hook(dev, 83, reinterpret_cast<void*>(&hk_draw_up), o_draw_up);
    hook(dev, 84, reinterpret_cast<void*>(&hk_draw_indexed_up), o_draw_indexed_up);
}

}  // namespace

void atoc_hook_device(IDirect3DDevice9* dev) {
    if (!dev) return;
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return;
    dev->GetCreationParameters(&cp);
    const D3DFORMAT adapter = SUCCEEDED(dev->GetDisplayMode(0, &mode)) && mode.Format != D3DFMT_UNKNOWN
                                  ? mode.Format
                                  : D3DFMT_X8R8G8B8;
    const auto supported = [&](DWORD fourcc) {
        return d3d->CheckDeviceFormat(cp.AdapterOrdinal, D3DDEVTYPE_HAL, adapter, 0, D3DRTYPE_SURFACE,
                                      static_cast<D3DFORMAT>(fourcc)) == D3D_OK;
    };
    g_method = supported(kNvOn) ? Method::Nvidia : (supported(kAmdOn) ? Method::Amd : Method::None);
    // DXVK on AMD graphics (the Steam Deck, or DXVK = true on an AMD card) lists neither switch (it answers NVIDIA's
    // only on other vendors and never AMD's) but takes AMD's: POINTSIZE = 'A2M1' turns coverage on when its device's
    // vendor is AMD (D3D9DeviceEx::SetRenderState, DXVK 1.x to 3.x). Its device answers for ID3D9VkInteropDevice
    // (DXVK 2.1 and later).
    bool dxvk_amd = false;
    if (g_method == Method::None) {
        D3DADAPTER_IDENTIFIER9 id{};
        static const GUID kVkInteropDevice = {0x2eaa4b89, 0x0107, 0x4bdb, {0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0}};
        IUnknown* vk = nullptr;
        const bool dxvk = SUCCEEDED(dev->QueryInterface(kVkInteropDevice, reinterpret_cast<void**>(&vk))) && vk;
        if (vk) vk->Release();
        if (dxvk && SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id)) && id.VendorId == 0x1002) {
            g_method = Method::Amd;
            dxvk_amd = true;
        }
    }
    d3d->Release();
    if (g_method == Method::None) {
        LOG_INFO("AlphaToCoverage: the graphics driver offers neither NVIDIA's nor AMD's switch for it; not used");
        return;
    }
    install_hooks(dev);
    LOG_INFO("AlphaToCoverage: ready (%s, edges sharpened around the alpha test); takes effect while the "
             "anti-aliasing is on (AntialiasingSamples)",
             g_method == Method::Nvidia ? "NVIDIA's switch"
             : dxvk_amd                 ? "AMD's switch, which DXVK takes on AMD graphics without listing it"
                                        : "AMD's switch");
}

void atoc_set_mode(int mode) { g_mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode); }
void atoc_set_depth_diag(int bits) { InterlockedExchange(&g_depth_diag, bits); }

bool atoc_set_msaa_samples(int samples) {
    if (samples != 2 && samples != 4 && samples != 8) return false;
    auto* entry = reinterpret_cast<uint32_t*>(kMsaaTable + 4);
    if (*entry != 2) {
        LOG_ERROR("AntialiasingSamples: the game's multisample table at %08X holds %u, not 2 in this build. Not "
                  "changing it.", static_cast<unsigned>(kMsaaTable + 4), *entry);
        return false;
    }
    if (samples == 2) return true;
    DWORD old = 0;
    if (!VirtualProtect(entry, 4, PAGE_READWRITE, &old)) return false;
    *entry = static_cast<uint32_t>(samples);
    DWORD ignored = 0;
    VirtualProtect(entry, 4, old, &ignored);
    LOG_INFO("AntialiasingSamples: the game's anti-aliasing (when on) multisamples the scene %dx (the game's own: 2x)",
             samples);
    return true;
}

// The anti-aliasing's on/off switch. The game decides every frame whether the scene is multisampled from its
// Antialiasing option, read live from its settings ([[0x1378700]+0x34]+0xE5, setting 11 of the PC settings screen;
// DarkSouls.ini [DisplaySettingFilter] Antialiasing): 0xEAE440 ("is the scene's anti-aliasing on": the frame's own
// flag at +0x22C and the option) and the frame's scene setup at 0xEB0312 each test it with
// `cmp byte ptr [ecx+0xE5],0` / `setne al` (a third read, 0xEB5166, only picks a debug menu label). The multisampled
// targets are made whether or not the option is on (only the game's ForceDisableAA leaves them out: 0x100A1F1, and its
// frame flag at 0xEB0302), so the option takes effect at once. Both tests read this mod's switch instead
// (`cmp byte ptr [g_aa_on],0`, the same 7 bytes): on with AntialiasingSamples 2/4/8, off with "off", and with "game"
// a copy of the game's option made every frame (atoc_aa_tick), so the game behaves as without the switch and FPS Unlock
// Settings can still turn it on or off at once. The game's option, its row on the PC settings screen and DarkSouls.ini
// stay as the player left them. DSfix's resolution needs the game's anti-aliasing off (dllmain.cpp), so the switch
// stays off beside DSfix when DSfix makes the resolution.
namespace {
constexpr uintptr_t kAaTests[] = {0x00EAE457, 0x00EB032D};
constexpr uint8_t kAaTestBytes[7] = {0x80, 0xB9, 0xE5, 0x00, 0x00, 0x00, 0x00};  // cmp byte ptr [ecx+0xE5],0
constexpr uintptr_t kGameSettingsHolder = 0x01378700;                            // [+0x34] = the game's settings
volatile uint8_t g_aa_on = 0;
bool g_aa_patched = false;
volatile LONG g_aa_follow_game = 0;  // AntialiasingSamples = game: g_aa_on copies the game's option every frame

// The game's Antialiasing option as it stands (-1 while its settings are not there yet).
int game_aa_option() {
    __try {
        const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(kGameSettingsHolder);
        if (!holder) return -1;
        const uintptr_t s = *reinterpret_cast<const uintptr_t*>(holder + 0x34);
        if (!s) return -1;
        return *reinterpret_cast<const uint8_t*>(s + 0xE5) != 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// DarkSouls.ini's [DisplaySettingFilter] KEY (the game reads it at start), or -1 without the file.
int game_ini_filter(const wchar_t* key) {
    wchar_t local[MAX_PATH] = {}, ini[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return -1;
    _snwprintf_s(ini, _TRUNCATE, L"%s\\NBGI\\DarkSouls\\DarkSouls.ini", local);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) return -1;
    return static_cast<int>(GetPrivateProfileIntW(L"DisplaySettingFilter", key, 0, ini));
}
}  // namespace

bool atoc_set_aa_mode(int mode, int samples, bool dsfix_resolution) {
    if (dsfix_resolution) mode = 2;
    const int option = game_ini_filter(L"Antialiasing");
    const char* option_text = option < 0 ? "no DarkSouls.ini yet" : option ? "on in DarkSouls.ini" : "off in DarkSouls.ini";
    for (uintptr_t site : kAaTests) {
        if (std::memcmp(reinterpret_cast<const void*>(site), kAaTestBytes, sizeof(kAaTestBytes)) != 0) {
            LOG_ERROR("Anti-aliasing: the game's test of its Antialiasing option at %08X is not the expected "
                      "instruction in this build; the game's option decides (AntialiasingSamples has no say).",
                      static_cast<unsigned>(site));
            return false;
        }
    }
    g_aa_on = mode == 1 ? 1 : mode == 0 && option > 0 ? 1 : 0;
    InterlockedExchange(&g_aa_follow_game, mode == 0 ? 1 : 0);
    const uint32_t flag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_aa_on));
    for (uintptr_t site : kAaTests) {
        uint8_t code[7] = {0x80, 0x3D, 0, 0, 0, 0, 0x00};  // cmp byte ptr [g_aa_on],0
        std::memcpy(code + 2, &flag, 4);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof(code), PAGE_EXECUTE_READWRITE, &old)) {
            LOG_ERROR("Anti-aliasing: could not change the game's code at %08X (%lu)", static_cast<unsigned>(site),
                      GetLastError());
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(site), code, sizeof(code));
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(site), sizeof(code), old, &ignored);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_aa_patched = true;
    if (dsfix_resolution) {
        LOG_INFO("Anti-aliasing: the game's own kept off while DSfix makes the resolution (it needs it off; DSfix's "
                 "aaType smooths the picture). The game's option: %s", option_text);
    } else if (mode == 0) {
        LOG_INFO("Anti-aliasing: the game's own Antialiasing option decides (AntialiasingSamples = game; %s)%s",
                 option_text, option > 0 ? "" : ": none while it is off");
    } else if (mode == 1) {
        LOG_INFO("Anti-aliasing: on, the scene multisampled %dx (AntialiasingSamples), whatever the game's own "
                 "Antialiasing option says (%s)", samples, option_text);
    } else {
        LOG_INFO("Anti-aliasing: off (AntialiasingSamples = off; the game's own option, %s, is not used)", option_text);
    }
    if (mode == 1 && game_ini_filter(L"ForceDisableAA") > 0) {
        LOG_INFO("Anti-aliasing: ForceDisableAA = 1 in DarkSouls.ini, so the game makes no multisampled targets and "
                 "the picture has none; set it to 0 for AntialiasingSamples to work");
    }
    return true;
}

bool atoc_aa_switch(int mode) {
    if (!g_aa_patched) return false;
    InterlockedExchange(&g_aa_follow_game, mode == 0 ? 1 : 0);
    g_aa_on = mode == 0 ? (game_aa_option() > 0 ? 1 : 0) : mode == 1 ? 1 : 0;
    return true;
}

void atoc_aa_tick() {
    if (!g_aa_follow_game) return;
    const int option = game_aa_option();
    if (option >= 0) g_aa_on = static_cast<uint8_t>(option);
}

bool atoc_aa_on() {
    if (g_aa_patched) return g_aa_on != 0;
    return game_aa_option() > 0;
}

// FixAntialiasingDepth. With the game's own anti-aliasing on, the depth that the post-processing reads is drawn in a
// pre-pass into a multisampled 8-bit target (created at 0x100A740: the frame log's R3, with the multisampled depth
// buffer D4 that the scene's colour pass then reuses), as z/w in three base-256 digits (ps_c5b09a80 and its kind: R
// and G whole numbers over 255, B the rest). StretchRect resolves it into T12 (created from the descriptor written at
// 0x100AAFC), averaging each pixel's samples channel by channel into 8 bits, and every reader decodes it as
// dot(rgb, (255/256, 255/65536, 255/16777216)) (FRPG_Fil_DepthCopy_MSAA into the depth buffer, the low-resolution
// effects' depth copy and their upsample, the depth of field). Where a pixel's samples lie on both sides of a step of
// the top digit, the averaged R and G are rounded to 8 bits and decode up to 1/512 of z/w past both samples. With
// the near plane at 0.05 m, z/w crosses those steps 12.8, 6.4, 4.3, 3.2 ... m from the camera, so at the edges of
// grass blades in a ring around the player the depth was off by up to several metres (6 m at the 12.8 m ring), and
// the low-resolution effects (Firelink's mist) laid back over the scene by that depth left black specks there
// (2026-11-16, Firelink at 4K: a grid of them with the game's 512x360 effects, finer ones with ours). The game as
// shipped does it too.
//
// Both targets become one 32-bit float a pixel (R32F, format index 13 of the game's table at 0x10D3670: the same size
// as the game's 8-bit target), and the shaders that draw into them get copies that write the depth their digits hold
// as that float (depth_float above), so the resolve averages the samples' depths exactly and the game's readers,
// which see (x, 1, 1) from a float target, decode it unchanged. The first version (2.4.2 test 13) kept the digits in
// 16 bits a channel (A16B16G16R16, index 14), exact too but twice the memory at four samples a pixel: 4K with 4x
// went from about 203 to 179 FPS on an RTX 3090. That stays as the fallback for a driver that cannot multisample or
// filter a float target. Without the anti-aliasing the pre-pass draws into T12 directly, the same way.
namespace {
constexpr uintptr_t kFormatTable = 0x010D3670;
constexpr uint32_t kIndexA8R8G8B8 = 6, kIndexR32F = 13, kIndexA16B16G16R16 = 14;
struct DepthFormatSite {
    uintptr_t at;    // the format index: imm8 of a push or imm32 of a mov
    uint32_t bytes;  // 1 or 4
    const char* what;
};
constexpr DepthFormatSite kDepthFormats[] = {
    {0x0100A741, 1, "the multisampled depth target"},
    {0x0100AAFF, 4, "the depth texture it is resolved into"},
};
bool g_depth_fix = false;
bool g_depth_force16 = false;  // FixAntialiasingDepth = 16bit (tests)
int g_depth_samples = 2;

bool write_format_index(const DepthFormatSite& f, uint32_t index) {
    auto* p = reinterpret_cast<uint8_t*>(f.at);
    DWORD old = 0;
    if (!VirtualProtect(p, f.bytes, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, &index, f.bytes);
    DWORD ignored = 0;
    VirtualProtect(p, f.bytes, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), p, f.bytes);
    return true;
}

void write_depth_format(uint32_t index) {
    for (const DepthFormatSite& f : kDepthFormats) write_format_index(f, index);
}
}  // namespace

bool atoc_fix_depth_install(int samples, bool force16) {
    g_depth_force16 = force16;
    const auto* table = reinterpret_cast<const uint32_t*>(kFormatTable);
    if (table[kIndexA8R8G8B8] != D3DFMT_A8R8G8B8 || table[kIndexR32F] != D3DFMT_R32F ||
        table[kIndexA16B16G16R16] != D3DFMT_A16B16G16R16) {
        LOG_ERROR("FixAntialiasingDepth: the game's format table at %08X holds %u, %u and %u, not %u, %u and %u in this "
                  "build. Not changing it.", static_cast<unsigned>(kFormatTable), table[kIndexA8R8G8B8],
                  table[kIndexR32F], table[kIndexA16B16G16R16], static_cast<unsigned>(D3DFMT_A8R8G8B8),
                  static_cast<unsigned>(D3DFMT_R32F), static_cast<unsigned>(D3DFMT_A16B16G16R16));
        return false;
    }
    for (const DepthFormatSite& f : kDepthFormats) {
        uint32_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(f.at), f.bytes);
        if (v != kIndexA8R8G8B8) {
            LOG_ERROR("FixAntialiasingDepth: the format of %s at %08X is %u, not %u in this build. Not changing it.",
                      f.what, static_cast<unsigned>(f.at), v, kIndexA8R8G8B8);
            return false;
        }
    }
    // The float targets until the device says otherwise (atoc_fix_depth_check, before the game makes them).
    write_depth_format(kIndexR32F);
    g_depth_fix = true;
    g_depth_samples = samples == 4 || samples == 8 ? samples : 2;
    return true;
}

void atoc_fix_depth_check(IDirect3DDevice9* dev) {
    if (!g_depth_fix || !dev) return;
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return;
    dev->GetCreationParameters(&cp);
    const D3DFORMAT adapter = SUCCEEDED(dev->GetDisplayMode(0, &mode)) && mode.Format != D3DFMT_UNKNOWN
                                  ? mode.Format
                                  : D3DFMT_X8R8G8B8;
    const auto msaa = static_cast<D3DMULTISAMPLE_TYPE>(g_depth_samples);
    const auto can = [&](D3DFORMAT f, bool filter) {
        return d3d->CheckDeviceFormat(cp.AdapterOrdinal, D3DDEVTYPE_HAL, adapter, D3DUSAGE_RENDERTARGET,
                                      D3DRTYPE_TEXTURE, f) == D3D_OK &&
               (!filter || d3d->CheckDeviceFormat(cp.AdapterOrdinal, D3DDEVTYPE_HAL, adapter, D3DUSAGE_QUERY_FILTER,
                                                  D3DRTYPE_TEXTURE, f) == D3D_OK) &&
               d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, D3DDEVTYPE_HAL, f, TRUE, msaa, nullptr) == D3D_OK &&
               d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, D3DDEVTYPE_HAL, f, FALSE, msaa, nullptr) == D3D_OK;
    };
    const bool fl = !g_depth_force16 && can(D3DFMT_R32F, true);
    const bool u16 = !fl && can(D3DFMT_A16B16G16R16, false);
    d3d->Release();
    if (fl) {
        g_depth_float = true;
        // DXVK (its device answers for ID3D9VkInteropDevice, DXVK 2.1 and later; under Proton Direct3D 9 is DXVK's
        // unless WineD3D was asked for): its alpha test depends on the target's format (pre_pass_kind).
        static const GUID kVkInteropDevice = {0x2eaa4b89, 0x0107, 0x4bdb, {0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0}};
        IUnknown* vk = nullptr;
        const bool interop = SUCCEEDED(dev->QueryInterface(kVkInteropDevice, reinterpret_cast<void**>(&vk))) && vk;
        if (vk) vk->Release();
        g_alpha_exact = interop || d3d_under_proton();
        install_hooks(dev);
        hook(dev, 34, reinterpret_cast<void*>(&hk_stretch_depth), o_stretch);
        hook(dev, 65, reinterpret_cast<void*>(&hk_set_texture_depth), o_set_texture);
        LOG_INFO("FixAntialiasingDepth: with the game's anti-aliasing on, the depth the effects and the depth of field "
                 "read is one float per sample (was three 8-bit digits, the same size), so it stays right at the edges "
                 "of grass and leaves (no black specks in a ring around the player)");
        return;
    }
    if (u16) {
        write_depth_format(kIndexA16B16G16R16);
        LOG_INFO("FixAntialiasingDepth: the graphics driver cannot multisample or filter a float target; the depth's "
                 "three digits are kept in 16 bits a channel instead (right at the edges of grass too, twice the "
                 "memory)");
        return;
    }
    // Before the game makes its render targets (it does after the device exists): back to the game's 8 bits.
    write_depth_format(kIndexA8R8G8B8);
    g_depth_fix = false;
    LOG_ERROR("FixAntialiasingDepth: the graphics driver can multisample neither a float nor a 16-bit target here; "
              "the depth stays 8-bit as in the game");
}

void atoc_reset() {
    alias_release();
}
