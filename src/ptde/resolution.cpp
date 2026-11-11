#include "resolution.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

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

bool is_copy_shader(const DWORD* code) {
    if (!code || code[0] != kCopyShader[0]) return false;
    size_t k = 1;
    for (size_t i = 1; i < 4096; ++i) {
        const DWORD op = code[i];
        if ((op & 0xFFFF) == 0xFFFE) {  // comment block
            i += (op >> 16) & 0x7FFF;
            continue;
        }
        if (k >= sizeof(kCopyShader) / sizeof(kCopyShader[0]) || op != kCopyShader[k]) return false;
        if (op == 0x0000FFFF) return k == sizeof(kCopyShader) / sizeof(kCopyShader[0]) - 1;
        ++k;
    }
    return false;
}

HRESULT WINAPI hk_create_ps(IDirect3DDevice9* dev, const DWORD* code, IDirect3DPixelShader9** out) {
    const HRESULT hr = o_create_ps(dev, code, out);
    if (SUCCEEDED(hr) && out && *out && is_copy_shader(code)) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_copy_shaders.insert(*out);
    }
    return hr;
}

HRESULT WINAPI hk_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    bool copy = false;
    if (ps) {
        std::lock_guard<std::mutex> lock(g_lock);
        copy = g_copy_shaders.count(ps) != 0;
    }
    g_copy_bound = copy;
    return o_set_ps(dev, ps);
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
    void* p = patch_slot(dev, 106, reinterpret_cast<void*>(&hk_create_ps));
    if (p && !o_create_ps) o_create_ps = reinterpret_cast<CreatePSFn>(p);
    p = patch_slot(dev, 107, reinterpret_cast<void*>(&hk_set_ps));
    if (p && !o_set_ps) o_set_ps = reinterpret_cast<SetPSFn>(p);
    p = patch_slot(dev, 65, reinterpret_cast<void*>(&hk_set_texture));
    if (p && !o_set_texture) o_set_texture = reinterpret_cast<SetTextureFn>(p);
    p = patch_slot(dev, 83, reinterpret_cast<void*>(&hk_draw_up));
    if (p && !o_draw_up) o_draw_up = reinterpret_cast<DrawUPFn>(p);
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
