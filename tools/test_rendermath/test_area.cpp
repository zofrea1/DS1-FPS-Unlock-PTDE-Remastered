// test_area: the area-average copy shader (src/ptde/shaders/area_reduce.hlsl, bilinear pairs) against the previous
// point-sampled version (area_reduce_old_ps.h, from git) on a random 3840x2160 image into 512x360 and 256x180:
// largest and mean difference per channel, and GPU time of each (timestamp queries).
//   test_area.exe [d3d9|DXVK_DLL_PATH]
#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../src/ptde/shaders/area_reduce_ps.h"
#include "area_reduce_old_ps.h"
#include "test_area_vs.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr UINT kSrcW = 3840, kSrcH = 2160;

struct Vertex {
    float x, y, z, w;
    float u, v;
};

double run(IDirect3DDevice9* dev, IDirect3DTexture9* src, const BYTE* ps_code, UINT dw, UINT dh, bool linear,
           std::vector<uint32_t>& out) {
    IDirect3DPixelShader9* ps = nullptr;
    dev->CreatePixelShader(reinterpret_cast<const DWORD*>(ps_code), &ps);
    IDirect3DSurface9* rt = nullptr;
    dev->CreateRenderTarget(dw, dh, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
    IDirect3DSurface9* sys = nullptr;
    dev->CreateOffscreenPlainSurface(dw, dh, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    dev->SetRenderTarget(0, rt);
    D3DVIEWPORT9 vp{0, 0, dw, dh, 0, 1};
    dev->SetViewport(&vp);
    dev->SetPixelShader(ps);
    dev->SetTexture(0, src);
    const DWORD f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, f);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, f);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    const float sizes[8] = {float(kSrcW), float(kSrcH), 1.0f / kSrcW, 1.0f / kSrcH, float(dw), float(dh), 1.0f / dw,
                            1.0f / dh};
    dev->SetPixelShaderConstantF(200, sizes, 2);
    // uv = the target pixel's centre at each pixel centre (Direct3D 9: pixel centres at integer positions).
    const float u0 = 0.5f / dw, v0 = 0.5f / dh;
    const Vertex quad[4] = {{-1, 1, 0, 1, u0, v0}, {1, 1, 0, 1, 1 + u0, v0}, {-1, -1, 0, 1, u0, 1 + v0},
                            {1, -1, 0, 1, 1 + u0, 1 + v0}};
    IDirect3DQuery9 *q0 = nullptr, *q1 = nullptr, *qf = nullptr, *qd = nullptr;
    dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q0);
    dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q1);
    dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &qf);
    dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &qd);
    double ms = -1;
    for (int pass = 0; pass < 6; ++pass) {  // the last pass is timed
        if (qd) qd->Issue(D3DISSUE_BEGIN);
        if (q0) q0->Issue(D3DISSUE_END);
        dev->BeginScene();
        for (int k = 0; k < 10; ++k) dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
        dev->EndScene();
        if (q1) q1->Issue(D3DISSUE_END);
        if (qd) qd->Issue(D3DISSUE_END);
        if (qf) qf->Issue(D3DISSUE_END);
        UINT64 t0 = 0, t1 = 0, freq = 0;
        while (q1 && q1->GetData(&t1, sizeof(t1), D3DGETDATA_FLUSH) == S_FALSE) {
        }
        if (q0) q0->GetData(&t0, sizeof(t0), D3DGETDATA_FLUSH);
        while (qf && qf->GetData(&freq, sizeof(freq), D3DGETDATA_FLUSH) == S_FALSE) {
        }
        if (freq) ms = (t1 - t0) * 1000.0 / freq / 10.0;
    }
    dev->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr{};
    sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    out.resize(dw * dh);
    for (UINT y = 0; y < dh; ++y) std::memcpy(&out[y * dw], static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, dw * 4);
    sys->UnlockRect();
    for (IUnknown* p : {static_cast<IUnknown*>(q0), static_cast<IUnknown*>(q1), static_cast<IUnknown*>(qf),
                        static_cast<IUnknown*>(qd), static_cast<IUnknown*>(sys), static_cast<IUnknown*>(rt),
                        static_cast<IUnknown*>(ps)}) {
        if (p) p->Release();
    }
    return ms;
}

}  // namespace

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "d3d9";
    IDirect3D9* d3d = nullptr;
    if (std::strcmp(which, "d3d9") != 0) {
        HMODULE m = LoadLibraryA(which);
        auto create = m ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(m, "Direct3DCreate9")) : nullptr;
        d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    } else {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
    }
    if (!d3d) {
        std::printf("no Direct3D 9\n");
        return 2;
    }
    HWND w = CreateWindowExA(0, "STATIC", "test_area", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.hDeviceWindow = w;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        std::printf("CreateDevice %08lX\n", hr);
        return 2;
    }
    // A random image with structure: noise plus thin bright lines (the case the average is for).
    IDirect3DTexture9* src = nullptr;
    dev->CreateTexture(kSrcW, kSrcH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &src, nullptr);
    D3DLOCKED_RECT lr{};
    src->LockRect(0, &lr, nullptr, 0);
    uint32_t seed = 12345;
    for (UINT y = 0; y < kSrcH; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
        for (UINT x = 0; x < kSrcW; ++x) {
            seed = seed * 1664525u + 1013904223u;
            uint32_t c = seed >> 8;
            if (x % 37 == 0 || y % 23 == 0) c |= 0xFFFFFF;
            row[x] = 0xFF000000u | (c & 0xFFFFFF);
        }
    }
    src->UnlockRect(0);
    IDirect3DVertexShader9* vs = nullptr;
    dev->CreateVertexShader(reinterpret_cast<const DWORD*>(kTestAreaVs), &vs);
    dev->SetVertexShader(vs);
    const D3DVERTEXELEMENT9 decl[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
                                      D3DDECL_END()};
    IDirect3DVertexDeclaration9* vd = nullptr;
    dev->CreateVertexDeclaration(decl, &vd);
    dev->SetVertexDeclaration(vd);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    int fails = 0;
    const UINT sizes[][2] = {{512, 360}, {256, 180}};
    for (const auto& s : sizes) {
        std::vector<uint32_t> a, b;
        const double ms_old = run(dev, src, kAreaReduceOldPs, s[0], s[1], false, a);
        const double ms_new = run(dev, src, kAreaReducePs, s[0], s[1], true, b);
        int worst = 0;
        double sum = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(int((a[i] >> (8 * c)) & 0xFF) - int((b[i] >> (8 * c)) & 0xFF));
                worst = d > worst ? d : worst;
                sum += d;
            }
        }
        std::printf("%ux%u: point loop %.3f ms, bilinear pairs %.3f ms; difference max %d, mean %.4f (of 255)\n", s[0],
                    s[1], ms_old, ms_new, worst, sum / (a.size() * 3.0));
        if (worst > 1) ++fails;
    }
    std::printf(fails ? "FAIL\n" : "OK\n");
    return fails ? 1 : 0;
}
