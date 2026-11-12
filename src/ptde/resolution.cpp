#include "resolution.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "shaders/area_reduce_ps.h"
#include "shaders/dof_composite_ps.h"
#include "shaders/dof_near_rate_ps.h"
#include "shaders/hdr_coladj_ps.h"
#include "shaders/present_lut_ps.h"
#include "shaders/present_reduce_ps.h"
#include "shaders/present_reduce_vs.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace {

// Where RenderTargetManImp's constructor (0x100A1E0..0x100BB81) writes its target sizes: the address of each 32-bit
// immediate (a push or a mov into a seed) and the value it holds in this build.
struct Imm {
    uint32_t at;
    uint32_t value;
};
constexpr Imm kSizes[] = {
    {0x0100A5AC, 720}, {0x0100A5B1, 1024}, {0x0100A69D, 1024}, {0x0100A6A4, 720},  {0x0100A745, 720},
    {0x0100A74A, 1024}, {0x0100A7D3, 720}, {0x0100A7D8, 1024}, {0x0100A8C7, 1024}, {0x0100A8CE, 720},
    {0x0100A999, 1024}, {0x0100A9A0, 720}, {0x0100AAF1, 1024}, {0x0100AAF8, 720},  {0x0100ABC3, 1024},
    {0x0100ABCA, 720},  {0x0100AC59, 1024}, {0x0100AC60, 720}, {0x0100AD38, 1024}, {0x0100AD3F, 720},
    {0x0100AE0A, 1024}, {0x0100AE11, 720}, {0x0100AF66, 1024}, {0x0100AF6D, 720},  {0x0100B010, 1024},
    {0x0100B017, 720},  {0x0100B0DD, 720}, {0x0100B0E2, 1024}, {0x0100B17B, 1024}, {0x0100B182, 720},
    {0x0100B1ED, 720},  {0x0100B1F2, 1024}, {0x0100B28B, 1024}, {0x0100B292, 720}, {0x0100B39E, 512},
    {0x0100B3A5, 360},  {0x0100B508, 512}, {0x0100B50F, 360},  {0x0100B5DA, 512},  {0x0100B5E1, 360},
    {0x0100B703, 256},  {0x0100B70A, 180}, {0x0100B83E, 256},  {0x0100B845, 180},  {0x0100B98E, 128},
    {0x0100B995, 90},
    // The frame buffer (CoreGraphics init, 0xF90C30): the composite and HUD target, 1280x720.
    {0x00F90D6C, 1280}, {0x00F90D77, 720},
};

bool g_scale_post = false;
int g_frame_w = 0, g_frame_h = 0;  // the frame buffer's size (its original 1280x720)

uint32_t scaled(uint32_t original, int width, int height) {
    switch (original) {
        case 1024: return static_cast<uint32_t>(width);
        case 720: return static_cast<uint32_t>(height);
        // The post-processing targets keep their original sizes (g_scale_post: the experiment with them scaled).
        case 512: return g_scale_post ? static_cast<uint32_t>(width / 2) : original;
        case 360: return g_scale_post ? static_cast<uint32_t>(height / 2) : original;
        case 256: return g_scale_post ? static_cast<uint32_t>(width / 4) : original;
        case 180: return g_scale_post ? static_cast<uint32_t>(height / 4) : original;
        case 128: return g_scale_post ? static_cast<uint32_t>(width / 8) : original;
        case 90: return g_scale_post ? static_cast<uint32_t>(height / 8) : original;
        case 1280: return static_cast<uint32_t>(width);
        default: return original;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// The post-processing targets keep their original sizes, so the passes that copy the full scene into one of them
// (FRPG_Fil_Dof_DownSample: a single bilinear read, an exact 2x2 average from 1024x720 into 512x360) would read one
// 2x2 spot out of every 7.5x6 at 4K and let detail and highlights shimmer and flicker in the blur and the glare. Such
// a copy is drawn with our shader instead (shaders/area_reduce.hlsl): each target pixel the exact average of all the
// scene pixels it covers. (The driver's filtered StretchRect is no help here: it is bilinear too. Shrinking 3840 to
// 1024 lines one pixel wide every fourth column, a flat 25% grey, it gave 0-87% across a row; halved to 1920 first,
// 3-47%.)
namespace {

// The copy shader's instructions (comments left out): ps_3_0; dcl_texcoord1 v0.xy; dcl_2d s0;
// texld_pp oC0, v0, s0; end.
constexpr DWORD kCopyShader[] = {0xFFFF0300, 0x0200001F, 0x80010005, 0x90030000, 0x0200001F, 0x90000000,
                                 0xA00F0800, 0x03000042, 0x802F0800, 0x90E40000, 0xA0E40800, 0x0000FFFF};
constexpr UINT kOriginalWidth = 1024;

using CreateTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DTexture9**, HANDLE*);
CreateTextureFn o_create_texture = nullptr;
// DSfix in between (the game holds DSfix's device wrapper): its dofOverrideResolution enlarges the 512x360 and
// 256x180 targets that keep their original sizes here. Those are made on the device under the wrapper instead, so
// DSfix never sees them (it only wraps the device; textures pass through it as they are).
bool g_bypass_dsfix = false;
volatile LONG g_bypassed = 0;
using CreatePSFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPSFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawUPFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
CreatePSFn o_create_ps = nullptr;
SetPSFn o_set_ps = nullptr;
SetTextureFn o_set_texture = nullptr;
DrawUPFn o_draw_up = nullptr;

std::mutex g_lock;
std::unordered_set<void*> g_copy_shaders;
volatile bool g_copy_bound = false;
IDirect3DBaseTexture9* volatile g_tex0 = nullptr;
// Our averaging shader, once per device (a pixel shader outlives a reset).
IDirect3DPixelShader9* g_area_ps = nullptr;
IDirect3DDevice9* g_area_device = nullptr;
bool g_logged = false;

// The depth of field's composite (FRPG_Fil_Dof.fx, ps_7a60a0da): the sharp scene (s0) mixed with three 512x360 blur
// levels (s1-s3) by a weight from the scene's alpha and the 256x180 near blur mask (s4), c66/c67. Its instructions,
// comments left out.
constexpr DWORD kDofComposite[] = {
    0xFFFF0300, 0x0200001F, 0x80010005, 0x90030000, 0x0200001F, 0x90000000, 0xA00F0800, 0x0200001F, 0x90000000,
    0xA00F0801, 0x0200001F, 0x90000000, 0xA00F0802, 0x0200001F, 0x90000000, 0xA00F0803, 0x0200001F, 0x90000000,
    0xA00F0804, 0x03000042, 0x802F0000, 0x90E40000, 0xA0E40804, 0x03000042, 0x802F0001, 0x90E40000, 0xA0E40800,
    0x0300000B, 0x80210002, 0x80FF0001, 0x80FF0000, 0x02000001, 0x800F0000, 0xA0E40042, 0x04000004, 0x803F0000,
    0x80000002, 0x80E40000, 0xA0E40043, 0x03000042, 0x802F0002, 0x90E40000, 0xA0E40801, 0x03000002, 0x802E0000,
    0x81900000, 0x80E40000, 0x03000005, 0x802F0002, 0x80550000, 0x80E40002, 0x04000004, 0x802F0001, 0x80E40001,
    0x80000000, 0x80E40002, 0x03000042, 0x802F0002, 0x90E40000, 0xA0E40802, 0x04000004, 0x802F0001, 0x80E40002,
    0x80AA0000, 0x80E40001, 0x03000042, 0x802F0002, 0x90E40000, 0xA0E40803, 0x04000004, 0x802F0800, 0x80E40002,
    0x80FF0000, 0x80E40001, 0x0000FFFF};

// The depth of field's near blur mask (FRPG_Fil_Dof_NearRate.fx, ps_d7491115): one read of the packed depth (s1) at
// the centre of each 256x180 mask pixel, turned into a near blur rate by c8 and c10, with the colour of s0. Its
// instructions, comments left out.
constexpr DWORD kDofNearRate[] = {
    0xFFFF0300, 0x05000051, 0xA00F0000, 0x3F7F0000, 0x3B7F0000, 0x377F0000, 0x00000000, 0x0200001F, 0x80000005,
    0x90030000, 0x0200001F, 0x90000000, 0xA00F0800, 0x0200001F, 0x90000000, 0xA00F0801, 0x03000042, 0x800F0000,
    0x90E40000, 0xA0E40801, 0x03000008, 0x80010000, 0x80E40000, 0xA0E40000, 0x04000012, 0x80010001, 0x80000000,
    0xA0000008, 0xA0550008, 0x02000006, 0x80010000, 0x80000001, 0x02000006, 0x80020000, 0xA0550008, 0x03000005,
    0x80040000, 0x80550000, 0xA000000A, 0x04000004, 0x80020000, 0xA055000A, 0x80550000, 0x81AA0000, 0x04000004,
    0x80010000, 0xA0000008, 0x80000000, 0x81AA0000, 0x02000006, 0x80020000, 0x80550000, 0x03000005, 0x80110000,
    0x80550000, 0x80000000, 0x03000005, 0x80280800, 0x80000000, 0xA0AA000A, 0x03000042, 0x802F0000, 0x90E40000,
    0xA0E40800, 0x02000001, 0x80270800, 0x80E40000, 0x0000FFFF};

// True when the shader's instructions (its comment blocks left out) are exactly `want`.
bool same_instructions(const DWORD* code, const DWORD* want, size_t n) {
    if (!code || code[0] != want[0]) return false;
    size_t k = 1;
    for (size_t i = 1; i < 4096; ++i) {
        const DWORD op = code[i];
        if ((op & 0xFFFF) == 0xFFFE) {  // comment block
            i += (op >> 16) & 0x7FFF;
            continue;
        }
        if (k >= n || op != want[k]) return false;
        if (op == 0x0000FFFF) return k == n - 1;
        ++k;
    }
    return false;
}

bool is_copy_shader(const DWORD* code) {
    return same_instructions(code, kCopyShader, sizeof(kCopyShader) / sizeof(kCopyShader[0]));
}

// The final composite (FRPG_Fil_HDR_ColAdj.fx, ps_fe20dbc9): the scene (s0) with the 128x90 bloom (s1) and the
// 256x180 light shafts (s3) added, the colour matrix and the film grain (the 256x256 noise tile, s2). Its instructions,
// comments left out.
constexpr DWORD kHdrColAdj[] = {
    0xFFFF0300, 0x05000051, 0xA00F0000, 0x3F800000, 0x3F000000, 0x00000000, 0x00000000, 0x0200001F, 0x80010005,
    0x900F0000, 0x0200001F, 0x90000000, 0xA00F0800, 0x0200001F, 0x90000000, 0xA00F0801, 0x0200001F, 0x90000000,
    0xA00F0802, 0x0200001F, 0x90000000, 0xA00F0803, 0x03000042, 0x800F0000, 0x90EE0000, 0xA0E40802, 0x03000002,
    0x800F0001, 0x81E40000, 0xA0000000, 0x03000002, 0x800F0001, 0x80E40001, 0x80E40001, 0x03000042, 0x800F0002,
    0x90E40000, 0xA0E40800, 0x03000005, 0x80170002, 0x80E40002, 0xA0AA0038, 0x02000001, 0x80010003, 0xA0000038,
    0x03000002, 0x80080002, 0x81000003, 0xA0550047, 0x03000042, 0x800F0003, 0x90E40000, 0xA0E40801, 0x04000004,
    0x80080002, 0x80FF0003, 0x80FF0002, 0xA0000038, 0x04000004, 0x80070002, 0x80FF0002, 0x80E40003, 0x80E40002,
    0x03000042, 0x800F0003, 0x90E40000, 0xA0E40803, 0x04000004, 0x80070002, 0xA0550038, 0x80E40003, 0x80E40002,
    0x02000001, 0x80080002, 0xA0000000, 0x03000009, 0x80010003, 0x80E40002, 0xA0E4003E, 0x03000009, 0x80020003,
    0x80E40002, 0xA0E4003F, 0x03000009, 0x80040003, 0x80E40002, 0xA0E40040, 0x03000009, 0x80080003, 0x80E40002,
    0xA0E40041, 0x03000002, 0x800F0002, 0x81E40003, 0xA0000000, 0x04000004, 0x800F0001, 0x80E40001, 0x81E40002,
    0xA0000000, 0x03000002, 0x800F0002, 0x81E40000, 0xA0550000, 0x03000005, 0x800F0000, 0x80E40000, 0x80E40003,
    0x03000002, 0x800F0000, 0x80E40000, 0x80E40000, 0x04000058, 0x800F0000, 0x80E40002, 0xA0AA0000, 0x80E40000,
    0x04000058, 0x800F0001, 0x80E40002, 0x80E40001, 0xA0AA0000, 0x03000002, 0x800F0000, 0x80E40000, 0x80E40001,
    0x03000002, 0x800F0000, 0x81E40003, 0x80E40000, 0x04000004, 0x802F0800, 0xA0000044, 0x80E40000, 0x80E40003,
    0x0000FFFF};

// ---------------------------------------------------------------------------------------------------------------
// The low-resolution post-processing at the render resolution. The depth of field's blur levels (512x360, 256x180),
// the bloom (128x90) and the light shafts (256x180) keep their original sizes, so their amount is the game's at any
// resolution; two shaders stretch them over the scene: the depth of field's composite 2x at the original 1024x720 but
// 7.5x at 4K, and the final composite 10x and 5x at the original 1280x720 (then once more to the screen) but 30x and
// 15x at 4K. A single bilinear read that far shows the texture's grid: blocks and diamonds in the blurred distance,
// around the near blur and in every glow. Those two shaders are swapped for ours (shaders/*.hlsl, compiled by
// tools/build_shaders.bat): the same math, the low-resolution textures read with a cubic B-spline (four bilinear
// reads). Their sizes reach ours as c200/c201 (no game pixel shader uses a register above c183); those stages are set
// to bilinear and clamped for the draw. (Measured for the depth of field against the original at Firelink: detail and
// kinks of a half-blurred area 0.28/0.43 with ours, 0.29/0.42 in the original, 0.38/0.54 with one bilinear read.)
//
// The depth of field's near blur mask is swapped too (shaders/dof_near_rate.hlsl): the game's one depth read per
// 256x180 mask pixel became the average of the near rate over every depth pixel the mask pixel covers, so the near
// blur no longer flickers in blocks along near edges.
constexpr DWORD kTarget = 0xFF;  // size_stage: the render target's viewport instead of a stage
// The last copy of the frame buffer onto the back buffer in windowed and borderless mode (ps_067116db): one read of
// the frame buffer (s0), then the in-game Brightness through the 256x1 g_GammaTexture (s1). Its instructions.
constexpr DWORD kGammaCopy[] = {
    0xFFFF0300, 0x05000051, 0xA00F0000, 0x3F7F0000, 0x3B000000, 0x3F800000, 0x00000000, 0x0200001F, 0x80000005,
    0x900F0000, 0x0200001F, 0x90000000, 0xA00F0800, 0x0200001F, 0x90000000, 0xA00F0801, 0x0300005F, 0x800F0000,
    0x90E40000, 0xA0E40800, 0x04000004, 0x80070000, 0x80E40000, 0xA0000000, 0xA0550000, 0x0300005F, 0x800F0001,
    0x80000000, 0xA0E40801, 0x04000004, 0x80090800, 0x80000001, 0xA0E60000, 0xA0A70000, 0x0300005F, 0x800F0001,
    0x80550000, 0xA0E40801, 0x0300005F, 0x800F0000, 0x80AA0000, 0xA0E40801, 0x02000001, 0x80040800, 0x80AA0000,
    0x02000001, 0x80020800, 0x80550001, 0x0000FFFF};

struct SwapDef {
    const char* what;
    const DWORD* game;
    size_t tokens;
    const BYTE* ours;
    DWORD size_stage[2];  // their sizes go to c200 and c201
    float sizes[8];       // their original sizes, until the draw reads them from the textures
    DWORD smooth_mask;    // the stages read with the B-spline: bilinear, clamped for the draw
    DWORD point_mask;     // the stages read pixel by pixel: point sampled, clamped for the draw
};
constexpr SwapDef kSwaps[] = {
    {"the depth of field: its blur", kDofComposite, sizeof(kDofComposite) / sizeof(kDofComposite[0]), kDofCompositePs,
     {1, 4}, {512.0f, 360.0f, 1.0f / 512, 1.0f / 360, 256.0f, 180.0f, 1.0f / 256, 1.0f / 180}, 0x1E, 0},
    {"the final composite: the bloom and the light shafts", kHdrColAdj, sizeof(kHdrColAdj) / sizeof(kHdrColAdj[0]), kHdrColAdjPs, {1, 3},
     {128.0f, 90.0f, 1.0f / 128, 1.0f / 90, 256.0f, 180.0f, 1.0f / 256, 1.0f / 180}, 0x0A, 0},
    {"the depth of field: its near blur mask", kDofNearRate, sizeof(kDofNearRate) / sizeof(kDofNearRate[0]),
     kDofNearRatePs, {1, kTarget}, {1024.0f, 720.0f, 1.0f / 1024, 1.0f / 720, 256.0f, 180.0f, 1.0f / 256, 1.0f / 180},
     0, 0x02},
    {"the frame's scale-down to the window (windowed and borderless)", kGammaCopy,
     sizeof(kGammaCopy) / sizeof(kGammaCopy[0]), kPresentLutPs, {0, kTarget},
     {1280.0f, 720.0f, 1.0f / 1280, 1.0f / 720, 1280.0f, 720.0f, 1.0f / 1280, 1.0f / 720}, 0, 0x01},
};
constexpr int kSwapCount = sizeof(kSwaps) / sizeof(kSwaps[0]);
std::unordered_set<void*> g_swap_game[kSwapCount];       // the game's shaders, as it made them
IDirect3DPixelShader9* g_swap_ours[kSwapCount] = {};     // ours, once per device (a pixel shader outlives a reset)
IDirect3DDevice9* g_swap_device[kSwapCount] = {};
volatile LONG g_swap_bound = -1;     // the swap whose shader (ours) is bound
volatile LONG g_game_bound = -1;     // the swap whose game shader the game bound (ours in its place or not)
volatile bool g_swap_enabled = true;  // RemoteControl `dof 0/1` (all our post-processing shaders)
volatile LONG g_dof_force = 0;        // diagnostics: 1-3 shows only that blur level
constexpr int kDofSwap = 0;
constexpr int kPresentSwap = 3;               // the windowed scale-down (kGammaCopy)
volatile bool g_present_swap_enabled = true;  // RemoteControl `reduce 0/1`

int swap_of(const DWORD* code) {
    for (int i = 0; i < kSwapCount; ++i) {
        if (same_instructions(code, kSwaps[i].game, kSwaps[i].tokens)) return i;
    }
    return -1;
}

HRESULT WINAPI hk_create_texture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f,
                                 D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    const bool post = (w == 512 && h == 360) || (w == 256 && h == 180);
    if (g_bypass_dsfix && post && (usage & D3DUSAGE_RENDERTARGET) && out && !shared) {
        // The device under the wrapper: any texture the wrapper hands out knows it.
        IDirect3DTexture9* probe = nullptr;
        if (SUCCEEDED(o_create_texture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr)) && probe) {
            IDirect3DDevice9* real = nullptr;
            probe->GetDevice(&real);
            probe->Release();
            if (real && real != dev) {
                const HRESULT hr = real->CreateTexture(w, h, levels, usage, f, pool, out, nullptr);
                real->Release();
                if (SUCCEEDED(hr)) {
                    if (InterlockedIncrement(&g_bypassed) == 1) {
                        LOG_INFO("Resolution: the %ux%u depth of field and glare targets are made beside DSfix, so its "
                                 "dofOverrideResolution leaves them at their original sizes", w, h);
                    }
                    return hr;
                }
            } else if (real) {
                real->Release();
            }
        }
    }
    return o_create_texture(dev, w, h, levels, usage, f, pool, out, shared);
}

HRESULT WINAPI hk_create_ps(IDirect3DDevice9* dev, const DWORD* code, IDirect3DPixelShader9** out) {
    const HRESULT hr = o_create_ps(dev, code, out);
    if (SUCCEEDED(hr) && out && *out && is_copy_shader(code)) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_copy_shaders.insert(*out);
        if (g_area_device != dev) {
            if (g_area_ps) g_area_ps->Release();
            g_area_ps = nullptr;
            g_area_device = dev;
            if (FAILED(o_create_ps(dev, reinterpret_cast<const DWORD*>(kAreaReducePs), &g_area_ps))) {
                g_area_ps = nullptr;
                LOG_ERROR("Resolution: the averaging shader for the blur and glare copies could not be made; the "
                          "game's copy is used");
            }
        }
    } else if (SUCCEEDED(hr) && out && *out && !g_scale_post) {
        const int i = swap_of(code);
        if (i >= 0) {
            std::lock_guard<std::mutex> lock(g_lock);
            g_swap_game[i].insert(*out);
            if (g_swap_device[i] != dev) {
                if (g_swap_ours[i]) g_swap_ours[i]->Release();
                g_swap_ours[i] = nullptr;
                g_swap_device[i] = dev;
                if (FAILED(o_create_ps(dev, reinterpret_cast<const DWORD*>(kSwaps[i].ours), &g_swap_ours[i]))) {
                    g_swap_ours[i] = nullptr;
                    LOG_ERROR("Resolution: the smooth shader for %s could not be made; the game's is used",
                              kSwaps[i].what);
                } else {
                    if (kSwaps[i].point_mask) {
                        LOG_INFO("Resolution: %s averaged over every pixel it covers (steady at %dx%d)",
                                 kSwaps[i].what, g_frame_w, g_frame_h);
                    } else {
                        LOG_INFO("Resolution: %s read with a cubic filter (the game's own, smooth at %dx%d)",
                                 kSwaps[i].what, g_frame_w, g_frame_h);
                    }
                }
            }
        }
    }
    return hr;
}

HRESULT WINAPI hk_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    bool copy = false;
    int game = -1;
    int swap = -1;
    if (ps) {
        std::lock_guard<std::mutex> lock(g_lock);
        copy = g_copy_shaders.count(ps) != 0;
        for (int i = 0; i < kSwapCount && game < 0; ++i) {
            if (g_swap_game[i].count(ps)) game = i;
        }
        if (game >= 0 && g_swap_enabled && g_swap_ours[game] && g_swap_device[game] == dev &&
            (game != kPresentSwap || g_present_swap_enabled)) {
            swap = game;
        }
    }
    g_copy_bound = copy;
    g_game_bound = game;
    g_swap_bound = swap;
    if (swap >= 0) {
        // The original sizes; the draw sets them again from the textures it binds.
        dev->SetPixelShaderConstantF(200, kSwaps[swap].sizes, 2);
        return o_set_ps(dev, g_swap_ours[swap]);
    }
    return o_set_ps(dev, ps);
}

// The texture bound to a stage: its width and height into out[0..3] (w, h, 1/w, 1/h). False for none.
bool stage_size(IDirect3DDevice9* dev, DWORD stage, float* out) {
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(dev->GetTexture(stage, &t)) || !t) return false;
    D3DSURFACE_DESC d{};
    const bool ok = t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d)) &&
                    d.Width && d.Height;
    t->Release();
    if (!ok) return false;
    out[0] = static_cast<float>(d.Width);
    out[1] = static_cast<float>(d.Height);
    out[2] = 1.0f / d.Width;
    out[3] = 1.0f / d.Height;
    return true;
}

// The viewport's width and height into out[0..3] (w, h, 1/w, 1/h). False for none.
bool viewport_size(IDirect3DDevice9* dev, float* out) {
    D3DVIEWPORT9 vp{};
    if (FAILED(dev->GetViewport(&vp)) || !vp.Width || !vp.Height) return false;
    out[0] = static_cast<float>(vp.Width);
    out[1] = static_cast<float>(vp.Height);
    out[2] = 1.0f / vp.Width;
    out[3] = 1.0f / vp.Height;
    return true;
}

// Our shader's draw: the sizes, bilinear (or point) and clamped reads on its stages, then the game's states back.
HRESULT swap_draw(IDirect3DDevice9* dev, int swap, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    const SwapDef& d = kSwaps[swap];
    float sizes[8];
    std::memcpy(sizes, d.sizes, sizeof(sizes));
    for (int k = 0; k < 2; ++k) {
        if (d.size_stage[k] == kTarget) {
            viewport_size(dev, sizes + 4 * k);
        } else {
            stage_size(dev, d.size_stage[k], sizes + 4 * k);
        }
    }
    dev->SetPixelShaderConstantF(200, sizes, 2);
    static const D3DSAMPLERSTATETYPE kStates[] = {D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
                                                  D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
    static const DWORD kSmooth[] = {D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP};
    static const DWORD kPoint[] = {D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP};
    const DWORD mask = d.smooth_mask | d.point_mask;
    DWORD saved[8][5] = {};
    for (DWORD s = 0; s < 8; ++s) {
        if (!(mask & (1u << s))) continue;
        const DWORD* want = (d.point_mask & (1u << s)) ? kPoint : kSmooth;
        for (int i = 0; i < 5; ++i) {
            dev->GetSamplerState(s, kStates[i], &saved[s][i]);
            if (saved[s][i] != want[i]) dev->SetSamplerState(s, kStates[i], want[i]);
        }
    }
    const HRESULT hr = o_draw_up(dev, type, count, data, stride);
    for (DWORD s = 0; s < 8; ++s) {
        if (!(mask & (1u << s))) continue;
        const DWORD* want = (d.point_mask & (1u << s)) ? kPoint : kSmooth;
        for (int i = 0; i < 5; ++i) {
            if (saved[s][i] != want[i]) dev->SetSamplerState(s, kStates[i], saved[s][i]);
        }
    }
    return hr;
}

HRESULT WINAPI hk_set_texture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
    if (stage == 0) g_tex0 = tex;
    return o_set_texture(dev, stage, tex);
}

// The game's copy of a large render target into a small one, drawn with our averaging shader: the sizes to c200/c201,
// point sampling and clamping on stage 0, then the game's shader and states back.
HRESULT area_draw(IDirect3DDevice9* dev, const D3DSURFACE_DESC& src, const D3DVIEWPORT9& vp, D3DPRIMITIVETYPE type,
                  UINT count, const void* data, UINT stride) {
    const float sizes[8] = {static_cast<float>(src.Width), static_cast<float>(src.Height), 1.0f / src.Width,
                            1.0f / src.Height, static_cast<float>(vp.Width), static_cast<float>(vp.Height),
                            1.0f / vp.Width, 1.0f / vp.Height};
    dev->SetPixelShaderConstantF(200, sizes, 2);
    static const D3DSAMPLERSTATETYPE kStates[] = {D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
                                                  D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
    static const DWORD kValues[] = {D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP};
    DWORD saved[5] = {};
    for (int i = 0; i < 5; ++i) {
        dev->GetSamplerState(0, kStates[i], &saved[i]);
        if (saved[i] != kValues[i]) dev->SetSamplerState(0, kStates[i], kValues[i]);
    }
    IDirect3DPixelShader9* game = nullptr;
    dev->GetPixelShader(&game);
    o_set_ps(dev, g_area_ps);
    const HRESULT hr = o_draw_up(dev, type, count, data, stride);
    o_set_ps(dev, game);
    if (game) game->Release();
    for (int i = 0; i < 5; ++i) {
        if (saved[i] != kValues[i]) dev->SetSamplerState(0, kStates[i], saved[i]);
    }
    return hr;
}

HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    const LONG force = g_dof_force;
    const LONG swap = g_swap_bound;
    if (force >= 1 && force <= 3 && g_game_bound == kDofSwap) {
        // blend = (0, level 1, level 2, level 3) from saturate(blurW * c66 + c67): c66 = 0, c67 = the cumulative mask.
        float saved[8] = {};
        dev->GetPixelShaderConstantF(66, saved, 2);
        float forced[8] = {0, 0, 0, 0, 0, force <= 1 ? 1.0f : 0.0f, force <= 2 ? 1.0f : 0.0f, 1.0f};
        dev->SetPixelShaderConstantF(66, forced, 2);
        const HRESULT hr = swap == kDofSwap ? swap_draw(dev, swap, type, count, data, stride)
                                            : o_draw_up(dev, type, count, data, stride);
        dev->SetPixelShaderConstantF(66, saved, 2);
        return hr;
    }
    if (swap >= 0) return swap_draw(dev, swap, type, count, data, stride);
    IDirect3DBaseTexture9* tex = g_tex0;
    if (g_copy_bound && tex && tex->GetType() == D3DRTYPE_TEXTURE) {
        auto* src = static_cast<IDirect3DTexture9*>(tex);
        D3DSURFACE_DESC sd{};
        if (SUCCEEDED(src->GetLevelDesc(0, &sd)) && (sd.Usage & D3DUSAGE_RENDERTARGET) && sd.Width > kOriginalWidth &&
            sd.Format == D3DFMT_A8R8G8B8) {
            IDirect3DSurface9* rt = nullptr;
            D3DSURFACE_DESC rd{};
            D3DVIEWPORT9 vp{};
            const bool into_small = SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt && SUCCEEDED(rt->GetDesc(&rd)) &&
                                    rd.Width * 2 <= kOriginalWidth && SUCCEEDED(dev->GetViewport(&vp)) && vp.Width &&
                                    vp.Height;
            if (rt) rt->Release();
            if (into_small && g_swap_enabled && g_area_ps && g_area_device == dev) {
                const HRESULT hr = area_draw(dev, sd, vp, type, count, data, stride);
                if (!g_logged) {
                    g_logged = true;
                    LOG_INFO("Resolution: the scene (%ux%u) is averaged into the game's %ux%u blur and glare targets "
                             "(each pixel from all the scene pixels it covers)", sd.Width, sd.Height, vp.Width,
                             vp.Height);
                }
                return hr;
            }
        }
    }
    return o_draw_up(dev, type, count, data, stride);
}

// The menus lay out in the frame buffer's original 1280x720: their vertices are in those units and the menu shader
// maps them onto the viewport, so they scale with the frame buffer. A clipping rectangle the menu sets (SetScissorRect)
// is in pixels of the target, though, and the game passes it in the same 1280x720 units: at 4K the pause menu's
// heading box (848,109-1144,134, the selected tab's name, and 848,137-1144,162) clipped a third of the screen away
// from its text, which was never drawn. A rectangle set while the frame buffer (the scaled 1280x720 target) is the
// render target and that lies within 1280x720 is mapped onto the viewport as the menu's vertices are. The game's own
// full-target rectangles (0,0 to the target size) are larger and stay as they are.
using SetScissorFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*);
SetScissorFn o_set_scissor = nullptr;

HRESULT WINAPI hk_set_scissor(IDirect3DDevice9* dev, const RECT* r) {
    if (!r || r->left < 0 || r->top < 0 || r->right > 1280 || r->bottom > 720 ||
        (g_frame_w == 1280 && g_frame_h == 720)) {
        return o_set_scissor(dev, r);
    }
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC d{};
    const bool frame = SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt && SUCCEEDED(rt->GetDesc(&d)) &&
                       static_cast<int>(d.Width) == g_frame_w && static_cast<int>(d.Height) == g_frame_h;
    if (rt) rt->Release();
    // The menu's viewport spans the frame buffer (its full width or height). A small pass's own viewport (the eye
    // adaptation's 64x64 and 16x16, which can be set while the frame buffer is still bound) and a rectangle that is
    // just that viewport are left alone.
    D3DVIEWPORT9 vp{};
    if (!frame || FAILED(dev->GetViewport(&vp)) || vp.Width == 0 || vp.Height == 0 ||
        (static_cast<int>(vp.Width) != g_frame_w && static_cast<int>(vp.Height) != g_frame_h) ||
        (r->left == static_cast<LONG>(vp.X) && r->top == static_cast<LONG>(vp.Y) &&
         r->right == static_cast<LONG>(vp.X + vp.Width) && r->bottom == static_cast<LONG>(vp.Y + vp.Height))) {
        return o_set_scissor(dev, r);
    }
    const double sx = vp.Width / 1280.0, sy = vp.Height / 720.0;
    RECT m;
    m.left = vp.X + static_cast<LONG>(r->left * sx);
    m.top = vp.Y + static_cast<LONG>(r->top * sy);
    m.right = vp.X + static_cast<LONG>(r->right * sx + 0.999);
    m.bottom = vp.Y + static_cast<LONG>(r->bottom * sy + 0.999);
    static int logged = 0;  // the first few, for the log
    if (logged < 6) {
        ++logged;
        LOG_INFO("Resolution: a menu clipping rectangle (%ld,%ld-%ld,%ld in 1280x720) is mapped to %ld,%ld-%ld,%ld",
                 r->left, r->top, r->right, r->bottom, m.left, m.top, m.right, m.bottom);
    }
    return o_set_scissor(dev, &m);
}

// ---------------------------------------------------------------------------------------------------------------
// The frame's last scale-down (shaders/present_reduce.hlsl): when the frame buffer is larger than the screen
// (Supersampling, or RenderResolution above the display), the game's StretchRect of it onto the back buffer is drawn
// as an exact area average in linear light instead of the driver's bilinear filter.
using StretchRectFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
                                       const RECT*, D3DTEXTUREFILTERTYPE);
StretchRectFn o_stretch = nullptr;
volatile bool g_reduce_enabled = true;

struct Reduce {
    IDirect3DDevice9* dev = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DStateBlock9* state = nullptr;
    bool srgb = false;
    bool failed = false;
    bool logged = false;
};
Reduce g_reduce;

template <typename T>
void release_ptr(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool reduce_ready(IDirect3DDevice9* dev, D3DFORMAT src_format, D3DFORMAT dst_format) {
    if (g_reduce.dev != dev) {
        release_ptr(g_reduce.vs);
        release_ptr(g_reduce.ps);
        release_ptr(g_reduce.decl);
        release_ptr(g_reduce.state);
        g_reduce.dev = dev;
        g_reduce.failed = false;
        static const D3DVERTEXELEMENT9 kDecl[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
        if (FAILED(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(kPresentReduceVs), &g_reduce.vs)) ||
            FAILED(o_create_ps(dev, reinterpret_cast<const DWORD*>(kPresentReducePs), &g_reduce.ps)) ||
            FAILED(dev->CreateVertexDeclaration(kDecl, &g_reduce.decl))) {
            g_reduce.failed = true;
            LOG_ERROR("Resolution: the shaders for the frame's scale-down could not be made; the driver's filter is "
                      "used");
            return false;
        }
        // Linear-light averaging needs sRGB reads of the frame buffer and sRGB writes to the back buffer.
        IDirect3D9* d3d = nullptr;
        D3DDISPLAYMODE mode{};
        g_reduce.srgb = false;
        if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d && SUCCEEDED(dev->GetDisplayMode(0, &mode))) {
            const D3DFORMAT adapter = mode.Format == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : mode.Format;
            g_reduce.srgb =
                SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, adapter, D3DUSAGE_QUERY_SRGBREAD,
                                                 D3DRTYPE_TEXTURE, src_format)) &&
                SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, adapter,
                                                 D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_SURFACE, dst_format));
        }
        if (d3d) d3d->Release();
    }
    if (g_reduce.failed) return false;
    if (!g_reduce.state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_reduce.state))) return false;
    return true;
}

HRESULT reduce_draw(IDirect3DDevice9* dev, IDirect3DTexture9* tex, const D3DSURFACE_DESC& sd, const RECT& s,
                    IDirect3DSurface9* dst, const RECT& d) {
    if (FAILED(g_reduce.state->Capture())) return E_FAIL;
    IDirect3DSurface9* rts[4] = {};
    for (DWORD i = 0; i < 4; ++i) dev->GetRenderTarget(i, &rts[i]);
    IDirect3DSurface9* depth = nullptr;
    dev->GetDepthStencilSurface(&depth);

    dev->SetRenderTarget(0, dst);
    for (DWORD i = 1; i < 4; ++i) {
        if (rts[i]) dev->SetRenderTarget(i, nullptr);
    }
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp{static_cast<DWORD>(d.left), static_cast<DWORD>(d.top), static_cast<DWORD>(d.right - d.left),
                    static_cast<DWORD>(d.bottom - d.top), 0.0f, 1.0f};
    dev->SetViewport(&vp);
    static const D3DRENDERSTATETYPE kRs[] = {D3DRS_ZENABLE,          D3DRS_ZWRITEENABLE,     D3DRS_ALPHABLENDENABLE,
                                            D3DRS_ALPHATESTENABLE,  D3DRS_CULLMODE,         D3DRS_STENCILENABLE,
                                            D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FILLMODE,
                                            D3DRS_CLIPPLANEENABLE,  D3DRS_SRGBWRITEENABLE};
    const DWORD rs_values[] = {FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, 0xF, FALSE, D3DFILL_SOLID, 0,
                               g_reduce.srgb ? TRUE : FALSE};
    for (size_t i = 0; i < sizeof(kRs) / sizeof(kRs[0]); ++i) dev->SetRenderState(kRs[i], rs_values[i]);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, g_reduce.srgb ? TRUE : FALSE);
    o_set_texture(dev, 0, tex);
    dev->SetVertexShader(g_reduce.vs);
    o_set_ps(dev, g_reduce.ps);
    dev->SetVertexDeclaration(g_reduce.decl);
    const float sx = static_cast<float>(s.right - s.left) / static_cast<float>(d.right - d.left);
    const float sy = static_cast<float>(s.bottom - s.top) / static_cast<float>(d.bottom - d.top);
    const float consts[8] = {sx, sy, s.left - d.left * sx, s.top - d.top * sy, 1.0f / sd.Width, 1.0f / sd.Height,
                             static_cast<float>(sd.Width), static_cast<float>(sd.Height)};
    dev->SetPixelShaderConstantF(200, consts, 2);
    static const float kQuad[16] = {-1, 1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, -1, 0, 1};
    const HRESULT hr = o_draw_up(dev, D3DPT_TRIANGLESTRIP, 2, kQuad, 16);

    for (DWORD i = 0; i < 4; ++i) {
        if (i == 0 || rts[i]) dev->SetRenderTarget(i, rts[i]);
        if (rts[i]) rts[i]->Release();
    }
    dev->SetDepthStencilSurface(depth);
    if (depth) depth->Release();
    g_reduce.state->Apply();  // after the targets: setting a target resets the viewport
    return hr;
}

HRESULT WINAPI hk_stretch(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
                          const RECT* dr, D3DTEXTUREFILTERTYPE filter) {
    D3DSURFACE_DESC sd{}, dd{};
    if (g_reduce_enabled && src && dst && g_frame_w > 0 && SUCCEEDED(src->GetDesc(&sd)) &&
        static_cast<int>(sd.Width) == g_frame_w && static_cast<int>(sd.Height) == g_frame_h &&
        SUCCEEDED(dst->GetDesc(&dd)) && (dd.Usage & D3DUSAGE_RENDERTARGET)) {
        const RECT s = sr ? *sr : RECT{0, 0, static_cast<LONG>(sd.Width), static_cast<LONG>(sd.Height)};
        const RECT d = dr ? *dr : RECT{0, 0, static_cast<LONG>(dd.Width), static_cast<LONG>(dd.Height)};
        const LONG sw = s.right - s.left, sh = s.bottom - s.top, dw = d.right - d.left, dh = d.bottom - d.top;
        // Only a scale-down (in one direction at least, in none up): a same-size copy stays the game's.
        if (dw > 0 && dh > 0 && sw >= dw && sh >= dh && (sw > dw || sh > dh)) {
            IDirect3DTexture9* tex = nullptr;
            if (SUCCEEDED(src->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) && tex &&
                reduce_ready(dev, sd.Format, dd.Format)) {
                const HRESULT hr = reduce_draw(dev, tex, sd, s, dst, d);
                tex->Release();
                if (SUCCEEDED(hr)) {
                    if (!g_reduce.logged) {
                        g_reduce.logged = true;
                        LOG_INFO("Resolution: the frame (%ldx%ld) is scaled down to the screen (%ldx%ld) as an exact "
                                 "average of the pixels each screen pixel covers%s", sw, sh, dw, dh,
                                 g_reduce.srgb ? ", in linear light" : "");
                    }
                    return hr;
                }
            } else if (tex) {
                tex->Release();
            }
        }
    }
    return o_stretch(dev, src, sr, dst, dr, filter);
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

bool g_active = false;

}  // namespace

void resolution_hook_device(IDirect3DDevice9* dev) {
    if (!g_active || !dev) return;
    if (g_bypass_dsfix) {
        void* t = patch_slot(dev, 23, reinterpret_cast<void*>(&hk_create_texture));
        if (t && !o_create_texture) o_create_texture = reinterpret_cast<CreateTextureFn>(t);
    }
    void* p = patch_slot(dev, 106, reinterpret_cast<void*>(&hk_create_ps));
    if (p && !o_create_ps) o_create_ps = reinterpret_cast<CreatePSFn>(p);
    p = patch_slot(dev, 107, reinterpret_cast<void*>(&hk_set_ps));
    if (p && !o_set_ps) o_set_ps = reinterpret_cast<SetPSFn>(p);
    p = patch_slot(dev, 65, reinterpret_cast<void*>(&hk_set_texture));
    if (p && !o_set_texture) o_set_texture = reinterpret_cast<SetTextureFn>(p);
    p = patch_slot(dev, 83, reinterpret_cast<void*>(&hk_draw_up));
    if (p && !o_draw_up) o_draw_up = reinterpret_cast<DrawUPFn>(p);
    p = patch_slot(dev, 75, reinterpret_cast<void*>(&hk_set_scissor));
    if (p && !o_set_scissor) o_set_scissor = reinterpret_cast<SetScissorFn>(p);
    p = patch_slot(dev, 34, reinterpret_cast<void*>(&hk_stretch));
    if (p && !o_stretch) o_stretch = reinterpret_cast<StretchRectFn>(p);
}

void resolution_set_present_reduce(bool on) {
    g_reduce_enabled = on;
    g_present_swap_enabled = on;
}

void resolution_set_smooth_dof(bool on) { g_swap_enabled = on; }
void resolution_force_dof_level(int level) { g_dof_force = level; }

void resolution_reset() {
    g_tex0 = nullptr;
    release_ptr(g_reduce.state);  // made again on the next use
}

namespace {
// fld qword ptr [0x011E7DE8] (0.8) in the lens flare's visibility function; the address operand is at +2.
constexpr uint32_t kSunScaleLoad = 0x00FD3A52;
constexpr uint8_t kSunScaleBytes[] = {0xDD, 0x05, 0xE8, 0x7D, 0x1E, 0x01};
double g_sun_scale = 0.8;
}  // namespace

bool resolution_dsfix_sun(int render_width, int render_height) {
    if (render_width <= 0 || render_height <= 0) return false;
    auto* p = reinterpret_cast<uint8_t*>(kSunScaleLoad);
    if (std::memcmp(p, kSunScaleBytes, sizeof(kSunScaleBytes)) != 0) {
        LOG_ERROR("Sun and moon: the lens flare code at %08X does not match this build. Not patching.", kSunScaleLoad);
        return false;
    }
    const double factor = (static_cast<double>(render_width) * render_height) / (1024.0 * 720.0);
    if (factor <= 1.0) return false;
    g_sun_scale = 0.8 * factor;
    DWORD old = 0;
    if (!VirtualProtect(p + 2, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_sun_scale));
    std::memcpy(p + 2, &addr, 4);
    DWORD ignored = 0;
    VirtualProtect(p + 2, 4, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(kSunScaleBytes));
    LOG_INFO("Sun and moon: DSfix renders at %dx%d, so the lens flare's visibility probe is expected %.2f times larger "
             "(they no longer stay hidden after something passes in front)", render_width, render_height, factor);
    return true;
}

void resolution_bypass_dsfix(bool on) { g_bypass_dsfix = on; }

bool resolution_install(int width, int height, bool scale_post) {
    g_scale_post = scale_post;
    if (width < 64 || height < 64 || width > 8192 || height > 8192) return false;
    for (const Imm& s : kSizes) {
        uint32_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(s.at), 4);
        if (v != s.value) {
            LOG_ERROR("Resolution: the render target size at %08X is %u, not %u in this build. Not changing it.", s.at,
                      v, s.value);
            return false;
        }
    }
    for (const Imm& s : kSizes) {
        auto* p = reinterpret_cast<uint8_t*>(s.at);
        DWORD old = 0;
        if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        const uint32_t v = scaled(s.value, width, height);
        std::memcpy(p, &v, 4);
        DWORD ignored = 0;
        VirtualProtect(p, 4, old, &ignored);
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(0x0100A1E0), 0x19A1);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(0x00F90D60), 0x20);
    g_frame_w = static_cast<int>(scaled(1280, width, height));
    g_frame_h = static_cast<int>(scaled(720, width, height));
    g_active = true;
    if (scale_post) {
        LOG_INFO("Resolution: the 3D scene renders at %dx%d (was 1024x720); depth of field and glare at %dx%d and "
                 "%dx%d, bloom at %dx%d", width, height, width / 2, height / 2, width / 4, height / 4, width / 8,
                 height / 8);
    } else {
        LOG_INFO("Resolution: the 3D scene and the HUD render at %dx%d (were 1024x720 and 1280x720); depth of field, "
                 "glare and bloom keep their original sizes", width, height);
    }
    return true;
}
