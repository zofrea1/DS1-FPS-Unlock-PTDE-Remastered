// What Windows composes for a Direct3D 9 window in HDR: hdrprobe.exe [d3d9|DXVK_DLL_PATH] [small|big|full]
// Draws 256 grey levels and the red, green and blue ramps into a small window's backbuffer (ColorFill, no shaders),
// presents for a second, then reads the composed desktop through DXGI Desktop Duplication in FP16 (scRGB, 1.0 = 80
// nits) and prints, per level, what the screen shows. Run once on the system Direct3D 9 and once on DXVK to compare.
#include <windows.h>
#include <psapi.h>

#include <d3d11.h>
#include <d3d9.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

namespace {

int kStripe = 3;              // pixels per level
int kRow = 40;                // pixels per ramp row
int kWidth = 256 * 3;         // 768
int kHeight = 4 * 40;         // grey, red, green, blue

float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}

D3DCOLOR level_color(int row, int i) {
    switch (row) {
    case 0: return D3DCOLOR_XRGB(i, i, i);
    case 1: return D3DCOLOR_XRGB(i, 0, 0);
    case 2: return D3DCOLOR_XRGB(0, i, 0);
    default: return D3DCOLOR_XRGB(0, 0, i);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "d3d9";
    const char* mode = argc > 2 ? argv[2] : "small";
    const bool big = std::strcmp(mode, "small") != 0, full = std::strcmp(mode, "full") == 0;
    int px = 40, py = 40;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (big) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        px = mi.rcMonitor.left;
        py = mi.rcMonitor.top;
        kWidth = mi.rcMonitor.right - mi.rcMonitor.left;
        kHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
        kStripe = kWidth / 256;
        kRow = kHeight / 4;
    }
    const bool dxvk = std::strcmp(which, "d3d9") != 0;
    IDirect3D9* d3d = nullptr;
    if (dxvk) {
        HMODULE m = LoadLibraryA(which);
        auto create = m ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(m, "Direct3DCreate9")) : nullptr;
        d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    } else {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
    }
    if (!d3d) { std::printf("no Direct3D 9\n"); return 2; }
    HWND w = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "STATIC", "hdrprobe", WS_POPUP | WS_VISIBLE, px, py,
                             kWidth, kHeight, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = full ? FALSE : TRUE;
    if (full) pp.FullScreen_RefreshRateInHz = 0;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.hDeviceWindow = w;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { std::printf("CreateDevice %08lX\n", hr); return 2; }

    // Desktop Duplication on the output showing the window.
    HMONITOR monitor = MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY);
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    DXGI_OUTPUT_DESC od{};
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
        IDXGIOutput* o = nullptr;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) == S_OK; ++i) {
            o->GetDesc(&od);
            if (od.Monitor == monitor) { output = o; break; }
            o->Release();
        }
        if (!output) { adapter->Release(); adapter = nullptr; }
    }
    if (!output) { std::printf("output not found\n"); return 2; }
    IDXGIOutput6* o6 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&o6));
    DXGI_OUTPUT_DESC1 od1{};
    if (o6) o6->GetDesc1(&od1);
    ID3D11Device* d11 = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &ctx);
    if (FAILED(hr)) { std::printf("D3D11CreateDevice %08lX\n", hr); return 2; }
    IDXGIOutput5* o5 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(&o5));
    DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    IDXGIOutputDuplication* dup = nullptr;
    hr = o5 ? o5->DuplicateOutput1(d11, 0, 1, formats, &dup) : E_NOINTERFACE;
    if (FAILED(hr)) { std::printf("DuplicateOutput1 %08lX\n", hr); return 2; }

    IDirect3DSurface9* bb = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    for (int frame = 0; frame < 90; ++frame) {
        for (int row = 0; row < 4; ++row) {
            for (int i = 0; i < 256; ++i) {
                RECT r{i * kStripe, row * kRow, (i + 1) * kStripe, (row + 1) * kRow};
                dev->ColorFill(bb, &r, level_color(row, i));
            }
        }
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    }
    // A few frames of the desktop; the last one is used.
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    for (int k = 0; k < 10; ++k) {
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* res = nullptr;
        if (dup->AcquireNextFrame(200, &fi, &res) != S_OK) continue;
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        tex->GetDesc(&td);
        if (!staging) {
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            d11->CreateTexture2D(&sd, nullptr, &staging);
        }
        ctx->CopyResource(staging, tex);
        tex->Release();
        res->Release();
        dup->ReleaseFrame();
    }
    if (!staging) { std::printf("no desktop frame\n"); return 2; }
    D3D11_MAPPED_SUBRESOURCE map{};
    ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    POINT origin{0, 0};
    ClientToScreen(w, &origin);
    const int ox = origin.x - od.DesktopCoordinates.left, oy = origin.y - od.DesktopCoordinates.top;
    std::printf("%s %s: output %ls, colour space %d, max %.0f nits, desktop format %d; window at %d,%d\n",
                dxvk ? "DXVK" : "system Direct3D 9", mode, od.DeviceName, static_cast<int>(od1.ColorSpace), od1.MaxLuminance,
                static_cast<int>(td.Format), ox, oy);
    auto sample = [&](int row, int i, float* rgb) {
        const int x = ox + i * kStripe + kStripe / 2, y = oy + row * kRow + kRow / 2;
        if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            const auto* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(map.pData) + y * map.RowPitch) + x * 4;
            for (int c = 0; c < 3; ++c) rgb[c] = half_to_float(p[c]);
        } else {  // B8G8R8A8 (SDR desktop)
            const auto* p = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch + x * 4;
            rgb[0] = p[2] / 255.0f; rgb[1] = p[1] / 255.0f; rgb[2] = p[0] / 255.0f;
        }
    };
    std::printf("level   grey R G B (scRGB)          | red row R | green row G | blue row B\n");
    for (int i = 0; i < 256; ++i) {
        if (!(i < 16 || i % 8 == 0 || i == 255)) continue;
        float g[3], r[3], gr[3], b[3];
        sample(0, i, g); sample(1, i, r); sample(2, i, gr); sample(3, i, b);
        std::printf("%3d  %8.5f %8.5f %8.5f | %8.5f | %8.5f | %8.5f\n", i, g[0], g[1], g[2], r[0], gr[1], b[2]);
    }
    ctx->Unmap(staging, 0);
    // Modules other software put into this process (overlays, filters).
    HMODULE mods[1024];
    DWORD need = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        std::printf("modules:");
        for (DWORD i = 0; i < need / sizeof(HMODULE); ++i) {
            char name[MAX_PATH];
            if (K32GetModuleFileNameExA(GetCurrentProcess(), mods[i], name, MAX_PATH)) {
                const char* base = std::strrchr(name, '\\');
                std::printf(" %s", base ? base + 1 : name);
            }
        }
        std::printf("\n");
    }
    return 0;
}
