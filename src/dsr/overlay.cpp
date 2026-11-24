#include "overlay.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "shaders/overlay_ps.h"
#include "shaders/overlay_vs.h"

// The game creates its device with D3D11CreateDevice and its swap chain through DXGI (CreateDXGIFactory): the swap
// chain it makes shows where Present (slot 8) and Present1 (slot 22 of IDXGISwapChain1) are in the method table every
// DXGI swap chain of the process shares, and those two entries are replaced. Nothing is drawn and nothing is held
// between frames unless a picture is queued (the back buffer is let go before Present, so the game's ResizeBuffers
// is never blocked by us).
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
void (*g_frame)(int, int) = nullptr;
volatile LONG g_busy = 0;  // inside our Present (a wrapper's Present1 calling Present is drawn once)

struct Item {
    const SettingsPageImage* img = nullptr;
    int slot = 0, x = 0, y = 0;
};
Item g_items[4];
int g_count = 0;

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    bool srgb = false;
    unsigned version = 0;
};

struct Res {
    ID3D11Device* dev = nullptr;  // not held: the resources below hold it
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* smp = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    Tex tex[4];
    bool failed = false;
};
Res g_res;

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void drop_resources() {
    release(g_res.vs);
    release(g_res.ps);
    release(g_res.cb);
    release(g_res.smp);
    release(g_res.blend);
    release(g_res.rs);
    release(g_res.dss);
    for (auto& t : g_res.tex) {
        release(t.srv);
        release(t.tex);
        t = Tex{};
    }
    g_res.dev = nullptr;
    g_res.failed = false;
}

bool make_resources(ID3D11Device* dev) {
    if (g_res.dev == dev && g_res.vs) return true;
    if (g_res.dev == dev && g_res.failed) return false;
    drop_resources();
    g_res.dev = dev;
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(dev->CreateVertexShader(kOverlayVs, sizeof(kOverlayVs), nullptr, &g_res.vs)) ||
        FAILED(dev->CreatePixelShader(kOverlayPs, sizeof(kOverlayPs), nullptr, &g_res.ps)) ||
        FAILED(dev->CreateBuffer(&cbd, nullptr, &g_res.cb)) || FAILED(dev->CreateSamplerState(&sd, &g_res.smp)) ||
        FAILED(dev->CreateBlendState(&bd, &g_res.blend)) || FAILED(dev->CreateRasterizerState(&rd, &g_res.rs)) ||
        FAILED(dev->CreateDepthStencilState(&dd, &g_res.dss))) {
        LOG_ERROR("FPS Unlock Settings: could not make the Direct3D 11 objects to draw the page");
        drop_resources();
        g_res.dev = dev;
        g_res.failed = true;
        return false;
    }
    return true;
}

bool upload(ID3D11Device* dev, ID3D11DeviceContext* ctx, Tex& t, const SettingsPageImage* img, bool srgb) {
    if (!img || !img->bits || img->w <= 0 || img->h <= 0) return false;
    if (t.tex && (t.w != img->w || t.h != img->h || t.srgb != srgb)) {
        release(t.srv);
        release(t.tex);
    }
    if (!t.tex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(img->w);
        td.Height = static_cast<UINT>(img->h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &t.tex)) ||
            FAILED(dev->CreateShaderResourceView(t.tex, nullptr, &t.srv))) {
            release(t.srv);
            release(t.tex);
            return false;
        }
        t.w = img->w;
        t.h = img->h;
        t.srgb = srgb;
        t.version = img->version - 1;
    }
    if (t.version != img->version) {
        ctx->UpdateSubresource(t.tex, 0, nullptr, img->bits, static_cast<UINT>(img->w) * 4, 0);
        t.version = img->version;
    }
    return true;
}

// The back buffer's format as a render target view (typeless ones as their plain format), and whether it is sRGB.
DXGI_FORMAT view_format(DXGI_FORMAT f, bool* srgb) {
    *srgb = false;
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: *srgb = true; return f;
        default: return f;
    }
}

// The state this draw changes, as the game left it.
struct Saved {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT mask = 0;
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ID3D11RasterizerState* rs = nullptr;
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11InputLayout* il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11ClassInstance* vs_ci[256] = {};
    UINT vs_nci = 256;
    ID3D11Buffer* vs_cb = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11ClassInstance* hs_ci[256] = {};
    UINT hs_nci = 256;
    ID3D11DomainShader* ds = nullptr;
    ID3D11ClassInstance* ds_ci[256] = {};
    UINT ds_nci = 256;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11ClassInstance* gs_ci[256] = {};
    UINT gs_nci = 256;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ClassInstance* ps_ci[256] = {};
    UINT ps_nci = 256;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* smp = nullptr;
};

void save(ID3D11DeviceContext* c, Saved& s) {
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
    c->OMGetBlendState(&s.blend, s.factor, &s.mask);
    c->OMGetDepthStencilState(&s.dss, &s.ref);
    c->RSGetState(&s.rs);
    c->RSGetViewports(&s.nvp, s.vp);
    c->IAGetInputLayout(&s.il);
    c->IAGetPrimitiveTopology(&s.topo);
    c->VSGetShader(&s.vs, s.vs_ci, &s.vs_nci);
    c->VSGetConstantBuffers(0, 1, &s.vs_cb);
    c->HSGetShader(&s.hs, s.hs_ci, &s.hs_nci);
    c->DSGetShader(&s.ds, s.ds_ci, &s.ds_nci);
    c->GSGetShader(&s.gs, s.gs_ci, &s.gs_nci);
    c->PSGetShader(&s.ps, s.ps_ci, &s.ps_nci);
    c->PSGetShaderResources(0, 1, &s.srv);
    c->PSGetSamplers(0, 1, &s.smp);
}

void restore(ID3D11DeviceContext* c, Saved& s) {
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    c->OMSetBlendState(s.blend, s.factor, s.mask);
    c->OMSetDepthStencilState(s.dss, s.ref);
    c->RSSetState(s.rs);
    c->RSSetViewports(s.nvp, s.vp);
    c->IASetInputLayout(s.il);
    c->IASetPrimitiveTopology(s.topo);
    c->VSSetShader(s.vs, s.vs_ci, s.vs_nci);
    c->VSSetConstantBuffers(0, 1, &s.vs_cb);
    c->HSSetShader(s.hs, s.hs_ci, s.hs_nci);
    c->DSSetShader(s.ds, s.ds_ci, s.ds_nci);
    c->GSSetShader(s.gs, s.gs_ci, s.gs_nci);
    c->PSSetShader(s.ps, s.ps_ci, s.ps_nci);
    c->PSSetShaderResources(0, 1, &s.srv);
    c->PSSetSamplers(0, 1, &s.smp);
    for (auto*& p : s.rtv) release(p);
    release(s.dsv);
    release(s.blend);
    release(s.dss);
    release(s.rs);
    release(s.il);
    release(s.vs);
    for (UINT i = 0; i < s.vs_nci && i < 256; ++i) release(s.vs_ci[i]);
    release(s.vs_cb);
    release(s.hs);
    for (UINT i = 0; i < s.hs_nci && i < 256; ++i) release(s.hs_ci[i]);
    release(s.ds);
    for (UINT i = 0; i < s.ds_nci && i < 256; ++i) release(s.ds_ci[i]);
    release(s.gs);
    for (UINT i = 0; i < s.gs_nci && i < 256; ++i) release(s.gs_ci[i]);
    release(s.ps);
    for (UINT i = 0; i < s.ps_nci && i < 256; ++i) release(s.ps_ci[i]);
    release(s.srv);
    release(s.smp);
}

void draw_items(ID3D11Device* dev, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bd) {
    if (!make_resources(dev)) return;
    bool srgb = false;
    D3D11_RENDER_TARGET_VIEW_DESC rvd{};
    rvd.Format = view_format(bd.Format, &srgb);
    rvd.ViewDimension = bd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(dev->CreateRenderTargetView(bb, &rvd, &rtv)) || !rtv) {
        static bool logged = false;
        if (!logged) {
            LOG_ERROR("FPS Unlock Settings: could not draw into the back buffer (format %d)", static_cast<int>(bd.Format));
            logged = true;
        }
        return;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        rtv->Release();
        return;
    }
    Saved* s = new Saved;  // ~10 KB: not on the game's render thread stack
    save(ctx, *s);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(g_res.blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_res.dss, 0);
    ctx->RSSetState(g_res.rs);
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(bd.Width), static_cast<float>(bd.Height), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(g_res.vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_res.cb);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_res.ps, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &g_res.smp);
    const float W = static_cast<float>(bd.Width), H = static_cast<float>(bd.Height);
    for (int i = 0; i < g_count; ++i) {
        const Item& it = g_items[i];
        Tex& t = g_res.tex[it.slot & 3];
        if (!upload(dev, ctx, t, it.img, srgb)) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_res.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
        const float r[4] = {it.x / W * 2.0f - 1.0f, 1.0f - it.y / H * 2.0f, (it.x + t.w) / W * 2.0f - 1.0f,
                            1.0f - (it.y + t.h) / H * 2.0f};
        std::memcpy(m.pData, r, sizeof(r));
        ctx->Unmap(g_res.cb, 0);
        ctx->PSSetShaderResources(0, 1, &t.srv);
        ctx->Draw(4, 0);
    }
    restore(ctx, *s);
    delete s;
    ctx->Release();
    rtv->Release();
}

// ---------------------------------------------------------------- diagnostics: a frame into a BMP, presents a second
std::mutex g_shot_lock;
std::wstring g_shot_file;
volatile LONG g_shot_pending = 0;
LARGE_INTEGER g_rate_t0{};
LONG g_rate_n = 0;
volatile LONG g_rate_x100 = 0;

void dump(ID3D11Device* dev, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bd) {
    std::wstring file;
    {
        std::lock_guard<std::mutex> lock(g_shot_lock);
        file = g_shot_file;
    }
    InterlockedExchange(&g_shot_pending, 0);
    bool srgb = false;
    const DXGI_FORMAT f = view_format(bd.Format, &srgb);
    const bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool ten = f == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!bgra && !rgba && !ten) {
        LOG_INFO("[overlay] shot: back buffer format %d not written", static_cast<int>(bd.Format));
        return;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC sd = bd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Texture2D* resolved = nullptr;
    if (bd.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = sd;
        rd.Usage = D3D11_USAGE_DEFAULT;
        rd.CPUAccessFlags = 0;
        if (SUCCEEDED(dev->CreateTexture2D(&rd, nullptr, &resolved))) ctx->ResolveSubresource(resolved, 0, bb, 0, f);
    }
    if (ctx && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
        ctx->CopyResource(staging, resolved ? resolved : bb);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            const int w = static_cast<int>(bd.Width), h = static_cast<int>(bd.Height);
            FILE* out = _wfopen(file.c_str(), L"wb");
            if (out) {
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                ih.biSize = sizeof(ih);
                ih.biWidth = w;
                ih.biHeight = -h;
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                ih.biCompression = BI_RGB;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w) * h * 4;
                std::fwrite(&fh, sizeof(fh), 1, out);
                std::fwrite(&ih, sizeof(ih), 1, out);
                uint32_t* line = new uint32_t[w];
                for (int y = 0; y < h; ++y) {
                    const auto* src = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) +
                                                                        static_cast<size_t>(y) * m.RowPitch);
                    for (int x = 0; x < w; ++x) {
                        const uint32_t p = src[x];
                        if (bgra) {
                            line[x] = p;
                        } else if (rgba) {
                            line[x] = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
                        } else {
                            const uint32_t r = (p & 0x3FF) >> 2, g = ((p >> 10) & 0x3FF) >> 2, b = ((p >> 20) & 0x3FF) >> 2;
                            line[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                        }
                    }
                    std::fwrite(line, 4, static_cast<size_t>(w), out);
                }
                delete[] line;
                std::fclose(out);
                LOG_INFO("[overlay] shot: %dx%d (format %d) written", w, h, static_cast<int>(bd.Format));
            }
            ctx->Unmap(staging, 0);
        }
    }
    release(staging);
    release(resolved);
    release(ctx);
}

void count_present() {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_rate_t0.QuadPart == 0) {
        g_rate_t0 = now;
        g_rate_n = 0;
        return;
    }
    ++g_rate_n;
    const double s = static_cast<double>(now.QuadPart - g_rate_t0.QuadPart) / static_cast<double>(freq.QuadPart);
    if (s >= 2.0) {
        InterlockedExchange(&g_rate_x100, static_cast<LONG>(g_rate_n / s * 100.0));
        g_rate_t0 = now;
        g_rate_n = 0;
    }
}

void on_present(IDXGISwapChain* sc) {
    if (FPSU_DIAGNOSTICS) count_present();
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) {
        dev->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    g_count = 0;
    if (g_frame) g_frame(static_cast<int>(bd.Width), static_cast<int>(bd.Height));
    if (g_count > 0) draw_items(dev, bb, bd);
    if (FPSU_DIAGNOSTICS && g_shot_pending) dump(dev, bb, bd);
    bb->Release();
    dev->Release();
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    const bool outer = InterlockedCompareExchange(&g_busy, 1, 0) == 0;
    if (outer && !(flags & DXGI_PRESENT_TEST)) on_present(sc);
    const HRESULT r = g_present(sc, sync, flags);
    if (outer) InterlockedExchange(&g_busy, 0);
    return r;
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    const bool outer = InterlockedCompareExchange(&g_busy, 1, 0) == 0;
    if (outer && !(flags & DXGI_PRESENT_TEST)) on_present(sc);
    const HRESULT r = g_present1(sc, sync, flags, p);
    if (outer) InterlockedExchange(&g_busy, 0);
    return r;
}

bool patch_slot(void** slot, void* value, void** original) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (original) *original = *slot;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), value);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

// The game's swap chain: its method table once the game has made it (nullptr before), and whether Present is ours.
std::mutex g_hook_lock;
void** volatile g_sc_vtable = nullptr;
bool g_present_hooked = false;

void hook_present(void** vtable, IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(g_hook_lock);
    if (g_present_hooked || !vtable || !g_frame) return;
    void* original = nullptr;
    if (!patch_slot(vtable + 8, reinterpret_cast<void*>(&hk_present), &original)) {
        LOG_ERROR("FPS Unlock Settings: could not hook the swap chain's Present; the page is not drawn");
        return;
    }
    g_present = reinterpret_cast<PresentFn>(original);
    bool present1 = false;
    IDXGISwapChain1* sc1 = nullptr;
    if (sc && SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
        if (*reinterpret_cast<void***>(sc1) == vtable && patch_slot(vtable + 22, reinterpret_cast<void*>(&hk_present1),
                                                                    &original)) {
            g_present1 = reinterpret_cast<Present1Fn>(original);
            present1 = true;
        }
        sc1->Release();
    }
    g_present_hooked = true;
    LOG_INFO("FPS Unlock Settings: drawn in the swap chain's Present%s", present1 ? " (and Present1)" : "");
}

// The game makes its swap chain through a DXGI factory (CreateDXGIFactory, imported): the factory's CreateSwapChain
// (slot 10) and IDXGIFactory2's CreateSwapChainForHwnd (slot 15) are watched, and the swap chain they make shows
// where its Present is. Nothing of our own is created on the device (a device made early from our thread crashed
// RivaTuner's hooks, RTSSHooks64.dll, 2026-11-23).
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
CreateFactoryFn g_create_factory = nullptr;
CreateSwapChainFn g_create_swap_chain = nullptr;
CreateSwapChainForHwndFn g_create_swap_chain_hwnd = nullptr;
void** g_factory_vtable = nullptr;

void note_swap_chain(IDXGISwapChain* sc) {
    if (!sc) return;
    void** vt = *reinterpret_cast<void***>(sc);
    g_sc_vtable = vt;
    hook_present(vt, sc);
}

HRESULT STDMETHODCALLTYPE hk_create_swap_chain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                               IDXGISwapChain** out) {
    const HRESULT r = g_create_swap_chain(f, dev, d, out);
    if (SUCCEEDED(r) && out) note_swap_chain(*out);
    return r;
}

HRESULT STDMETHODCALLTYPE hk_create_swap_chain_hwnd(IDXGIFactory2* f, IUnknown* dev, HWND wnd,
                                                    const DXGI_SWAP_CHAIN_DESC1* d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o,
                                                    IDXGISwapChain1** out) {
    const HRESULT r = g_create_swap_chain_hwnd(f, dev, wnd, d, fs, o, out);
    if (SUCCEEDED(r) && out) note_swap_chain(*out);
    return r;
}

HRESULT WINAPI hk_create_factory(REFIID riid, void** out) {
    const HRESULT r = g_create_factory(riid, out);
    if (FAILED(r) || !out || !*out) return r;
    std::lock_guard<std::mutex> lock(g_hook_lock);
    void** vt = *reinterpret_cast<void***>(*out);
    if (g_factory_vtable == vt) return r;
    void* original = nullptr;
    if (!g_create_swap_chain && patch_slot(vt + 10, reinterpret_cast<void*>(&hk_create_swap_chain), &original)) {
        g_create_swap_chain = reinterpret_cast<CreateSwapChainFn>(original);
    }
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2))) &&
        f2) {
        if (*reinterpret_cast<void***>(f2) == vt && !g_create_swap_chain_hwnd &&
            patch_slot(vt + 15, reinterpret_cast<void*>(&hk_create_swap_chain_hwnd), &original)) {
            g_create_swap_chain_hwnd = reinterpret_cast<CreateSwapChainForHwndFn>(original);
        }
        f2->Release();
    }
    g_factory_vtable = vt;
    return r;
}

}  // namespace

bool overlay_early() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "dxgi.dll") != 0 || !d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "CreateDXGIFactory") != 0) continue;
            void* original = nullptr;
            if (!patch_slot(reinterpret_cast<void**>(&iat->u1.Function), reinterpret_cast<void*>(&hk_create_factory),
                            &original)) {
                return false;
            }
            g_create_factory = reinterpret_cast<CreateFactoryFn>(original);
            return true;
        }
    }
    return false;
}

bool overlay_install(void (*frame)(int width, int height)) {
    if (!g_create_factory) {
        LOG_ERROR("FPS Unlock Settings: the game's CreateDXGIFactory import was not found; the page is not drawn");
        return false;
    }
    g_frame = frame;
    void** vt = g_sc_vtable;
    if (vt) {
        hook_present(vt, nullptr);
    } else {
        LOG_INFO("FPS Unlock Settings: waiting for the game's swap chain");
    }
    return true;
}

void overlay_draw(int slot, const SettingsPageImage* img, int x, int y) {
    if (!img || g_count >= 4) return;
    g_items[g_count++] = Item{img, slot & 3, x, y};
}

void overlay_request_shot(const wchar_t* file) {
    if (!FPSU_DIAGNOSTICS) return;
    {
        std::lock_guard<std::mutex> lock(g_shot_lock);
        g_shot_file = file;
    }
    InterlockedExchange(&g_shot_pending, 1);
}

double overlay_present_rate() { return static_cast<double>(g_rate_x100) / 100.0; }
