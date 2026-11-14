#include "shadowbias.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>
#include <unordered_map>

// Characters and objects take their shadow from a 2048x2048 shadow map: the vertex shader hands down the light's
// clip position of the vertex (vertex constants c137..c140, a matrix from the model's space: c139 the depth row, c140
// the w row), and the pixel shader compares depth / w with sixteen unfiltered samples of the map (packed depth), each
// a hard lit-or-not, and averages them. The game adds no depth bias, so a lit surface that curves away from the light
// is at the map's own depth to within a texel's slope: which of the sixteen comparisons pass then follows the map's
// texel grid, and the surface shows that grid as a fine weave, as rings on round shapes (the egg sacs of the
// egg-burdened in Quelaag's Domain, the player's back). At 1024x720 the weave is finer than the pixels; at higher
// render resolutions it is plain to see.
//
// ShadowBias takes a constant off the clip depth the vertex shader hands down (c139's fourth component: the input's w
// is 1), so the compared depth / w is nearer by bias / w: by more where the light is far and a shadow texel covers
// more of the surface. The shadow map itself and the lights are as the game made them.
//
// The constants are the game's per draw, and some vertex shaders use c139 for something else (a bone palette at
// c128..c241), so the bias goes only where the bound vertex shader reads c139 as that matrix row (a dp4 with c139).
// Each vertex shader is classed when the game makes it: the row (wants the biased c139), another use of c139 (wants
// the game's), or none. The device holds the biased row while a shader of the first kind is bound and the game's
// value when one of the second kind is; writes of c139 by the game are followed by the biased row while one of the
// first kind is bound.
namespace {

using CreateVsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVsConstFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
CreateVsFn o_create_vs = nullptr;
SetVsFn o_set_vs = nullptr;
SetVsConstFn o_set_vs_const = nullptr;

constexpr UINT kDepthRow = 139;
enum Kind : uint8_t { kNone = 0, kShadowRow = 1, kOtherUse = 2 };

volatile float g_bias = 0.0f;
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<IDirect3DVertexShader9*, uint8_t> g_kinds;  // shaders that use c139 (the others are not kept)

// The render thread's: the bound shader's kind, the game's c139 as last set, and whether the device holds the biased
// row.
uint8_t g_bound = kNone;
float g_row[4] = {};
bool g_row_known = false;
bool g_biased = false;
bool g_logged = false;

// How a vertex shader uses c139: the instruction stream walked token by token.
uint8_t classify(const DWORD* code) {
    if (!code || (code[0] & 0xFFFF0000u) != 0xFFFE0000u) return kNone;
    const bool extra_rel = ((code[0] >> 8) & 0xFF) >= 2;  // a relative address has its own token from 2.0 on
    uint8_t kind = kNone;
    const DWORD* p = code + 1;
    for (int guard = 0; guard < 20000; ++guard) {
        const DWORD t = *p;
        const DWORD op = t & 0xFFFF;
        if (op == 0xFFFF) break;
        if (op == 0xFFFE) {  // comment
            p += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (t >> 24) & 0xF;
        if (op != 0x1F && op != 0x51 && op != 0x30 && op != 0x2F) {  // not dcl, def, defi, defb
            for (DWORD i = 1; i <= len; ++i) {
                const DWORD a = p[i];
                if (!(a & 0x80000000u)) continue;
                const DWORD type = ((a >> 28) & 7) | ((a >> 8) & 0x18);
                const DWORD num = a & 0x7FF;
                const bool rel = (a & 0x2000) != 0;
                if (rel && extra_rel) ++i;  // its address token
                if (i == 1 || type != 2) continue;  // the destination, or not a float constant
                if (rel) {
                    if (num <= kDepthRow && num > 24) kind = kOtherUse;  // an array that can reach c139
                } else if (num == kDepthRow) {
                    if (op == 9 && kind != kOtherUse) {
                        kind = kShadowRow;  // dp4
                    } else {
                        kind = kOtherUse;
                    }
                }
            }
        }
        p += 1 + len;
    }
    return kind;
}

void send_row(IDirect3DDevice9* dev, bool biased) {
    if (!g_row_known) return;
    float row[4] = {g_row[0], g_row[1], g_row[2], g_row[3]};
    if (biased) row[3] -= g_bias;
    o_set_vs_const(dev, kDepthRow, row, 1);
    g_biased = biased;
    if (biased && !g_logged) {
        g_logged = true;
        LOG_INFO("ShadowBias: the shadows on characters and objects are compared %.4f nearer in the light's clip depth "
                 "(no weave of shadow map texels on lit, curved surfaces)",
                 static_cast<double>(g_bias));
    }
}

HRESULT WINAPI hk_create_vs(IDirect3DDevice9* dev, const DWORD* code, IDirect3DVertexShader9** out) {
    const HRESULT hr = o_create_vs(dev, code, out);
    if (SUCCEEDED(hr) && out && *out) {
        const uint8_t kind = classify(code);
        AcquireSRWLockExclusive(&g_lock);
        if (kind != kNone) {
            g_kinds[*out] = kind;
        } else {
            g_kinds.erase(*out);  // an address used again
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    return hr;
}

HRESULT WINAPI hk_set_vs(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    const HRESULT hr = o_set_vs(dev, vs);
    uint8_t kind = kNone;
    if (vs) {
        AcquireSRWLockShared(&g_lock);
        const auto it = g_kinds.find(vs);
        if (it != g_kinds.end()) kind = it->second;
        ReleaseSRWLockShared(&g_lock);
    }
    g_bound = kind;
    const bool on = g_bias != 0.0f;
    if (kind == kShadowRow && on && !g_biased) {
        send_row(dev, true);
    } else if (g_biased && (kind == kOtherUse || (kind == kShadowRow && !on))) {
        send_row(dev, false);
    }
    return hr;
}

HRESULT WINAPI hk_set_vs_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    const HRESULT hr = o_set_vs_const(dev, start, v, count);
    if (v && start <= kDepthRow && start + count > kDepthRow) {
        std::memcpy(g_row, v + (kDepthRow - start) * 4, sizeof(g_row));
        g_row_known = true;
        g_biased = false;
        if (g_bound == kShadowRow && g_bias != 0.0f) send_row(dev, true);
    }
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

}  // namespace

// The side of the shadow map is one number in the game's data (0x12DFF34, 2048). The game reads it when it makes the
// shadow map (0x100A32B: a packed-depth colour target and its depth), for the viewport of each slice (0xEB4856: with
// four slices each has a quarter, half the side) and for the half-texel step of the matrix that looks the shadow up
// (0xF477DB). Everything else works in the map's own 0..1 coordinates, so a larger number gives the same shadows
// from more texels.
constexpr uintptr_t kShadowMapSize = 0x012DFF34;

bool shadowmap_set_resolution(int size) {
    if (size != 4096 && size != 8192) return size == 2048;
    auto* p = reinterpret_cast<int32_t*>(kShadowMapSize);
    if (*p != 2048) {
        LOG_ERROR("ShadowMapResolution: the shadow map size at %08X is %d, not 2048 (another build or mod?). Not "
                  "changing it.", static_cast<unsigned>(kShadowMapSize), *p);
        return false;
    }
    *p = size;
    LOG_INFO("ShadowMapResolution: the shadow map is %dx%d (the game's own: 2048x2048)", size, size);
    return true;
}

void shadowbias_set(float bias) {
    g_bias = bias < 0.0f ? 0.0f : (bias > 1.0f ? 1.0f : bias);
}

void shadowbias_hook_device(IDirect3DDevice9* dev) {
    if (!dev) return;
    void* p = patch_slot(dev, 91, reinterpret_cast<void*>(&hk_create_vs));
    if (p && !o_create_vs) o_create_vs = reinterpret_cast<CreateVsFn>(p);
    p = patch_slot(dev, 92, reinterpret_cast<void*>(&hk_set_vs));
    if (p && !o_set_vs) o_set_vs = reinterpret_cast<SetVsFn>(p);
    p = patch_slot(dev, 94, reinterpret_cast<void*>(&hk_set_vs_const));
    if (p && !o_set_vs_const) o_set_vs_const = reinterpret_cast<SetVsConstFn>(p);
    g_row_known = false;
    g_biased = false;
    g_bound = kNone;
}
