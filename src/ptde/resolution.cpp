#include "resolution.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "shaders/dof_composite_ps.h"

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
// 2x2 spot out of every 7.5x7.5 at 4K and let highlights flicker in the blur and glare. Such a copy reads a 1024x720
// version of the scene instead, made just before it by halving the scene with filtered blits.
namespace {

// The copy shader's instructions (comments left out): ps_3_0; dcl_texcoord1 v0.xy; dcl_2d s0;
// texld_pp oC0, v0, s0; end.
constexpr DWORD kCopyShader[] = {0xFFFF0300, 0x0200001F, 0x80010005, 0x90030000, 0x0200001F, 0x90000000,
                                 0xA00F0800, 0x03000042, 0x802F0800, 0x90E40000, 0xA0E40800, 0x0000FFFF};
constexpr UINT kOriginalWidth = 1024, kOriginalHeight = 720;

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
// The halving chain, made on first use and released before a device reset: [0] is the final 1024x720.
constexpr int kChain = 5;
IDirect3DTexture9* g_chain[kChain] = {};
UINT g_chain_w[kChain] = {}, g_chain_h[kChain] = {};
volatile LONG g_proxied = 0;
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

bool is_dof_composite(const DWORD* code) {
    return same_instructions(code, kDofComposite, sizeof(kDofComposite) / sizeof(kDofComposite[0]));
}

// ---------------------------------------------------------------------------------------------------------------
// The depth of field's blur at the render resolution. Its blur levels keep their original sizes (512x360, 256x180),
// so the amount of blur is the game's at any resolution; the composite stretches them over the scene, 2x at the
// original 1024x720 but 7.5x at 4K, where its single bilinear read shows the 512x360 grid (blocks and diamonds in
// the blurred distance and around the near blur). The composite is swapped for ours (shaders/dof_composite.hlsl):
// the same math, the low-resolution textures read with a cubic B-spline. The texture sizes reach it as c200/c201
// (no game pixel shader uses a register above c183); its samplers are set to bilinear and clamped for the draw.
std::unordered_set<void*> g_dof_shaders;
IDirect3DPixelShader9* g_dof_ours = nullptr;
IDirect3DDevice9* g_dof_device = nullptr;
volatile bool g_dof_bound = false;
volatile LONG g_dof_draws = 0;

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
    } else if (SUCCEEDED(hr) && out && *out && !g_scale_post && is_dof_composite(code)) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_dof_shaders.insert(*out);
        if (g_dof_device != dev) {  // ours, once per device (a pixel shader outlives a reset)
            if (g_dof_ours) g_dof_ours->Release();
            g_dof_ours = nullptr;
            g_dof_device = dev;
            if (FAILED(o_create_ps(dev, reinterpret_cast<const DWORD*>(kDofCompositePs), &g_dof_ours))) {
                g_dof_ours = nullptr;
                LOG_ERROR("Resolution: the smooth depth of field shader could not be made; the game's is used");
            } else {
                LOG_INFO("Resolution: the depth of field's blur is read with a cubic filter (the game's blur levels, "
                         "smooth at %ux%u)", static_cast<unsigned>(g_frame_w), static_cast<unsigned>(g_frame_h));
            }
        }
    }
    return hr;
}

HRESULT WINAPI hk_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    bool copy = false;
    bool dof = false;
    if (ps) {
        std::lock_guard<std::mutex> lock(g_lock);
        copy = g_copy_shaders.count(ps) != 0;
        dof = g_dof_ours && g_dof_device == dev && g_dof_shaders.count(ps) != 0;
    }
    g_copy_bound = copy;
    g_dof_bound = dof;
    if (dof) {
        // The game's sizes; the draw sets them again from the textures it binds.
        const float sizes[8] = {512.0f, 360.0f, 1.0f / 512, 1.0f / 360, 256.0f, 180.0f, 1.0f / 256, 1.0f / 180};
        dev->SetPixelShaderConstantF(200, sizes, 2);
        return o_set_ps(dev, g_dof_ours);
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

// Our composite's draw: the low-resolution textures' sizes and bilinear, clamped reads on stages 1-4, then the
// game's states back.
HRESULT dof_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    float sizes[8] = {512.0f, 360.0f, 1.0f / 512, 1.0f / 360, 256.0f, 180.0f, 1.0f / 256, 1.0f / 180};
    stage_size(dev, 1, sizes);
    stage_size(dev, 4, sizes + 4);
    dev->SetPixelShaderConstantF(200, sizes, 2);
    static const D3DSAMPLERSTATETYPE kStates[] = {D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
                                                  D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
    static const DWORD kValues[] = {D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP};
    DWORD saved[4][5] = {};
    for (DWORD s = 1; s <= 4; ++s) {
        for (int i = 0; i < 5; ++i) {
            dev->GetSamplerState(s, kStates[i], &saved[s - 1][i]);
            if (saved[s - 1][i] != kValues[i]) dev->SetSamplerState(s, kStates[i], kValues[i]);
        }
    }
    const HRESULT hr = o_draw_up(dev, type, count, data, stride);
    for (DWORD s = 1; s <= 4; ++s) {
        for (int i = 0; i < 5; ++i) {
            if (saved[s - 1][i] != kValues[i]) dev->SetSamplerState(s, kStates[i], saved[s - 1][i]);
        }
    }
    InterlockedIncrement(&g_dof_draws);
    return hr;
}

HRESULT WINAPI hk_set_texture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
    if (stage == 0) g_tex0 = tex;
    return o_set_texture(dev, stage, tex);
}

// A render target texture of this size, kept in the chain slot (remade when the size changes).
IDirect3DTexture9* chain_target(IDirect3DDevice9* dev, int slot, UINT w, UINT h) {
    if (g_chain[slot] && (g_chain_w[slot] != w || g_chain_h[slot] != h)) {
        g_chain[slot]->Release();
        g_chain[slot] = nullptr;
    }
    if (!g_chain[slot]) {
        if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_chain[slot],
                                      nullptr))) {
            g_chain[slot] = nullptr;
            return nullptr;
        }
        g_chain_w[slot] = w;
        g_chain_h[slot] = h;
    }
    return g_chain[slot];
}

bool blit(IDirect3DDevice9* dev, IDirect3DTexture9* from, IDirect3DTexture9* to) {
    IDirect3DSurface9* a = nullptr;
    IDirect3DSurface9* b = nullptr;
    bool ok = SUCCEEDED(from->GetSurfaceLevel(0, &a)) && SUCCEEDED(to->GetSurfaceLevel(0, &b)) &&
              SUCCEEDED(dev->StretchRect(a, nullptr, b, nullptr, D3DTEXF_LINEAR));
    if (a) a->Release();
    if (b) b->Release();
    return ok;
}

// The scene halved (filtered) down to about twice 1024x720, then to exactly 1024x720.
IDirect3DTexture9* proxy_of(IDirect3DDevice9* dev, IDirect3DTexture9* src, UINT w, UINT h) {
    IDirect3DTexture9* cur = src;
    int slot = kChain - 1;
    while (w >= kOriginalWidth * 4 && h >= kOriginalHeight * 3 && slot > 0) {
        w /= 2;
        h /= 2;
        IDirect3DTexture9* next = chain_target(dev, slot, w, h);
        if (!next || !blit(dev, cur, next)) return nullptr;
        cur = next;
        --slot;
    }
    IDirect3DTexture9* last = chain_target(dev, 0, kOriginalWidth, kOriginalHeight);
    if (!last || !blit(dev, cur, last)) return nullptr;
    return last;
}

HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    if (g_dof_bound) return dof_draw(dev, type, count, data, stride);
    IDirect3DBaseTexture9* tex = g_tex0;
    if (g_copy_bound && tex && tex->GetType() == D3DRTYPE_TEXTURE) {
        auto* src = static_cast<IDirect3DTexture9*>(tex);
        D3DSURFACE_DESC sd{};
        if (SUCCEEDED(src->GetLevelDesc(0, &sd)) && (sd.Usage & D3DUSAGE_RENDERTARGET) && sd.Width > kOriginalWidth &&
            sd.Format == D3DFMT_A8R8G8B8) {
            IDirect3DSurface9* rt = nullptr;
            D3DSURFACE_DESC rd{};
            const bool into_small = SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt && SUCCEEDED(rt->GetDesc(&rd)) &&
                               rd.Width * 2 <= kOriginalWidth;
            if (rt) rt->Release();
            if (into_small) {
                IDirect3DTexture9* proxy = proxy_of(dev, src, sd.Width, sd.Height);
                if (proxy) {
                    o_set_texture(dev, 0, proxy);
                    const HRESULT hr = o_draw_up(dev, type, count, data, stride);
                    o_set_texture(dev, 0, src);
                    InterlockedIncrement(&g_proxied);
                    if (!g_logged) {
                        g_logged = true;
                        LOG_INFO("Resolution: the scene (%ux%u) is reduced to 1024x720 before the game copies it into "
                                 "its %ux%u blur and glare targets", sd.Width, sd.Height, rd.Width, rd.Height);
                    }
                    return hr;
                }
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
}

void resolution_reset() {
    for (auto& t : g_chain) {
        if (t) t->Release();
        t = nullptr;
    }
    g_tex0 = nullptr;
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
