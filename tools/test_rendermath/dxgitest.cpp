// DXVK presenting through DXGI (src/ptde/dxgipresent.cpp) outside the game:
//   dxgitest.exe MODE [window|big] [seconds] [fps]
//   MODE: d3d9 (system Direct3D 9), dxvk:PATH (DXVK's own Vulkan presentation), dxgi:PATH (DXVK through DXGI)
// Environment: DXGITEST_PACE 0|1|2 (dxgipresent_set_pacing; the frames are timed from their start, as the game's are
// from its frame boundaries), DXGITEST_HDRCHAIN 1 (scRGB swap chain), DXGITEST_JITTER MS (each frame waits a random
// 0..MS before its Present, like the game's draw thread), DXGITEST_POS X,Y (window place), DXGITEST_REFRESH HZ.
// A game-like borderless window (big = covering the primary monitor), a Direct3D 9 device presenting immediately.
// 1. Colours: 256 grey levels and the red, green and blue ramps (ColorFill), then the composed desktop read through
//    DXGI Desktop Duplication in FP16 (scRGB in HDR): what the screen shows per level, to compare the modes.
// 2. Motion: a moving bar for the given seconds at the given frame rate (0 = as fast as it goes); frames per second
//    reached, and under dxgi how Windows showed the frames (dxgipresent_composition).
// The module's log lines go to dxgitest.log next to the exe.
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

#include "../../src/common/log.h"
#include "../../src/ptde/dxgipresent.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

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

LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(w, m, wp, lp); }

void pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
}

// Control: a plain Direct3D 11 flip-model swap chain (flip discard, tearing allowed) in the same kind of window.
int run_d3d11(bool big, double seconds, double fps) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int px = 40, py = 40, width = 960, height = 540;
    if (big) {
        px = mi.rcMonitor.left;
        py = mi.rcMonitor.top;
        width = mi.rcMonitor.right - mi.rcMonitor.left;
        height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = "dxgitest11";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "dxgitest11", "dxgitest11", WS_POPUP | WS_VISIBLE, px, py, width, height, nullptr,
                             nullptr, nullptr, nullptr);
    SetForegroundWindow(w);
    pump();
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    IDXGIFactory2* f = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f));
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = width;
    d.Height = height;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 3;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    IDXGISwapChain1* sc = nullptr;
    HRESULT hr = f->CreateSwapChainForHwnd(dev, w, &d, nullptr, nullptr, &sc);
    if (FAILED(hr)) {
        std::printf("CreateSwapChainForHwnd %08lX\n", hr);
        return 2;
    }
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int frames = 0;
    double next = 0;
    int mode = -1;
    for (;;) {
        QueryPerformanceCounter(&now);
        const double t = static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (t >= seconds) break;
        if (fps > 0) {
            if (t < next) {
                if (next - t > 0.002) Sleep(static_cast<DWORD>((next - t - 0.0015) * 1000));
                continue;
            }
            next += 1.0 / fps;
            if (next < t) next = t;
        }
        ID3D11Texture2D* bb = nullptr;
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
        ID3D11RenderTargetView* rtv = nullptr;
        dev->CreateRenderTargetView(bb, nullptr, &rtv);
        const float c = (frames & 1) ? 0.2f : 0.1f;
        const float col[4] = {c, c, c, 1};
        ctx->ClearRenderTargetView(rtv, col);
        rtv->Release();
        bb->Release();
        sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        pump();
        if ((++frames & 63) == 0) {
            IDXGISwapChainMedia* m = nullptr;
            if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChainMedia), reinterpret_cast<void**>(&m)))) {
                DXGI_FRAME_STATISTICS_MEDIA st{};
                if (SUCCEEDED(m->GetFrameStatisticsMedia(&st))) mode = static_cast<int>(st.CompositionMode);
                m->Release();
            }
        }
    }
    static const char* const names[] = {"composed", "overlay", "none (flipped straight)", "composition failure"};
    std::printf("plain Direct3D 11 flip discard %dx%d: %d frames in %.1f s, composition mode %s\n", width, height,
                frames, seconds, mode >= 0 && mode <= 3 ? names[mode] : "not known");
    sc->Release();
    DestroyWindow(w);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "d3d9";
    const bool big = argc > 2 && std::strcmp(argv[2], "big") == 0;
    const double seconds = argc > 3 ? std::atof(argv[3]) : 3.0;
    const double fps = argc > 4 ? std::atof(argv[4]) : 0.0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    log_init(exe, L"dxgitest.log");

    const bool use_dxvk = std::strncmp(which, "dxvk:", 5) == 0, use_dxgi = std::strncmp(which, "dxgi:", 5) == 0;
    if (std::strcmp(which, "d3d11") == 0) return run_d3d11(big, seconds, fps);
    const char* pace_env = std::getenv("DXGITEST_PACE");
    const char* hdr_env = std::getenv("DXGITEST_HDRCHAIN");
    const char* jitter_env = std::getenv("DXGITEST_JITTER");
    const char* refresh_env = std::getenv("DXGITEST_REFRESH");
    const double jitter_ms = jitter_env ? std::atof(jitter_env) : 0.0;
    static double s_cap = 0;
    s_cap = fps;
    dxgipresent_set_pacing(pace_env ? std::atoi(pace_env) : 0, []() { return static_cast<int>(s_cap); });
    dxgipresent_set_hdr_chain(hdr_env ? std::atoi(hdr_env) : 0);
    if (refresh_env) dxgipresent_test_refresh(std::atoi(refresh_env));
    IDirect3D9* d3d = nullptr;
    if (use_dxvk || use_dxgi) {
        HMODULE m = LoadLibraryA(which + 5);
        if (!m) { std::printf("cannot load %s\n", which + 5); return 2; }
        if (use_dxgi && !dxgipresent_install(m)) { std::printf("install failed\n"); return 2; }
        auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(m, "Direct3DCreate9"));
        d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    } else {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
    }
    if (!d3d) { std::printf("no Direct3D 9\n"); return 2; }

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int px = 40, py = 40, width = 960, height = 540;
    if (big) {
        px = mi.rcMonitor.left;
        py = mi.rcMonitor.top;
        width = mi.rcMonitor.right - mi.rcMonitor.left;
        height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    if (const char* pos = std::getenv("DXGITEST_POS")) std::sscanf(pos, "%d,%d", &px, &py);
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = "dxgitest";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(std::getenv("DXGITEST_NOFOCUS") ? WS_EX_NOACTIVATE : 0, "dxgitest", "dxgitest", WS_POPUP | WS_VISIBLE, px, py, width, height, nullptr,
                             nullptr, nullptr, nullptr);
    if (!std::getenv("DXGITEST_NOFOCUS")) SetForegroundWindow(w);
    pump();
    RECT cr{};
    GetClientRect(w, &cr);
    width = cr.right;
    height = cr.bottom;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.hDeviceWindow = w;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &dev);
    if (FAILED(hr)) { std::printf("CreateDevice %08lX\n", hr); return 2; }
    std::printf("%s, %s window %dx%d\n", use_dxgi ? "DXVK through DXGI" : use_dxvk ? "DXVK (Vulkan presentation)"
                                                                               : "system Direct3D 9",
                big ? "monitor-sized" : "small", width, height);

    // 1. Colours.
    const int stripe = width / 256, row_h = height / 4;
    IDirect3DSurface9* bb = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    auto draw_levels = [&]() {
        for (int row = 0; row < 4; ++row) {
            for (int i = 0; i < 256; ++i) {
                RECT r{i * stripe, row * row_h, (i + 1) * stripe, (row + 1) * row_h};
                dev->ColorFill(bb, &r, level_color(row, i));
            }
        }
    };
    for (int frame = 0; frame < 60; ++frame) {
        draw_levels();
        hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
        pump();
        Sleep(5);
    }
    std::printf("present result %08lX\n", hr);

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
    if (output && !std::getenv("DXGITEST_NOCOLOR")) {
        IDXGIOutput6* o6 = nullptr;
        output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&o6));
        DXGI_OUTPUT_DESC1 od1{};
        if (o6) o6->GetDesc1(&od1);
        ID3D11Device* d11 = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr,
                          &ctx);
        IDXGIOutput5* o5 = nullptr;
        output->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(&o5));
        DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT};
        IDXGIOutputDuplication* dup = nullptr;
        hr = o5 && d11 ? o5->DuplicateOutput1(d11, 0, 1, formats, &dup) : E_NOINTERFACE;
        ID3D11Texture2D* staging = nullptr;
        D3D11_TEXTURE2D_DESC td{};
        for (int k = 0; dup && k < 10; ++k) {
            draw_levels();
            dev->Present(nullptr, nullptr, nullptr, nullptr);
            pump();
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
        if (staging) {
            D3D11_MAPPED_SUBRESOURCE map{};
            ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
            POINT origin{0, 0};
            ClientToScreen(w, &origin);
            const int ox = origin.x - od.DesktopCoordinates.left, oy = origin.y - od.DesktopCoordinates.top;
            std::printf("desktop: colour space %d, max %.0f nits, format %d (%dx%d), window at %d,%d\n",
                        static_cast<int>(od1.ColorSpace), od1.MaxLuminance, static_cast<int>(td.Format), td.Width,
                        td.Height, ox, oy);
            // Desktop Duplication hands the image in the panel's own orientation; a rotated (portrait) desktop needs
            // its coordinates turned. Both 90-degree directions are tried; the one showing the grey ramp is used.
            const int dw = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            const int dh = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            const bool rotated = static_cast<int>(td.Width) != dw;
            int turn = 0;
            auto sample = [&](int row, int i, float* rgb) {
                const int dx = ox + i * stripe + stripe / 2, dy = oy + row * row_h + row_h / 2;
                int x = dx, y = dy;
                if (rotated) {
                    x = turn == 0 ? dy : dh - 1 - dy;
                    y = turn == 0 ? dw - 1 - dx : dx;
                }
                if (x < 0 || y < 0 || x >= static_cast<int>(td.Width) || y >= static_cast<int>(td.Height)) {
                    rgb[0] = rgb[1] = rgb[2] = -1;
                    return;
                }
                if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    const auto* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(map.pData) +
                                                                      y * map.RowPitch) + x * 4;
                    for (int c = 0; c < 3; ++c) rgb[c] = half_to_float(p[c]);
                } else {
                    const auto* p = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch + x * 4;
                    rgb[0] = p[2] / 255.0f;
                    rgb[1] = p[1] / 255.0f;
                    rgb[2] = p[0] / 255.0f;
                }
            };
            if (rotated) {
                float lo[3], hi[3], lo2[3], hi2[3];
                sample(0, 0, lo);
                sample(0, 255, hi);
                turn = 1;
                sample(0, 0, lo2);
                sample(0, 255, hi2);
                turn = (hi2[0] - lo2[0]) > (hi[0] - lo[0]) ? 1 : 0;
                std::printf("portrait desktop: sampling turned %s\n", turn ? "one way" : "the other way");
            }
            std::printf("level   grey R G B (scRGB)          | red row R | green row G | blue row B\n");
            for (int i = 0; i < 256; ++i) {
                if (!(i < 8 || i % 16 == 0 || i == 255)) continue;
                float g[3], r[3], gr[3], b[3];
                sample(0, i, g);
                sample(1, i, r);
                sample(2, i, gr);
                sample(3, i, b);
                std::printf("%3d  %8.5f %8.5f %8.5f | %8.5f | %8.5f | %8.5f\n", i, g[0], g[1], g[2], r[0], gr[1], b[2]);
            }
            ctx->Unmap(staging, 0);
        } else {
            std::printf("no desktop frame (duplication %08lX)\n", hr);
        }
        if (dup) dup->Release();
    }

    // 2. Motion.
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int frames = 0;
    double next = 0;
    double worst = 0, last = 0;
    for (;;) {
        QueryPerformanceCounter(&now);
        const double t = static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (t >= seconds) break;
        if (fps > 0) {
            if (t < next) {
                const double wait = next - t;
                if (wait > 0.002) Sleep(static_cast<DWORD>((wait - 0.0015) * 1000));
                continue;
            }
            next += 1.0 / fps;
            if (next < t) next = t;
        }
        dev->ColorFill(bb, nullptr, D3DCOLOR_XRGB(24, 24, 24));
        const int x = static_cast<int>(std::fmod(t * 600.0, static_cast<double>(width)));
        RECT bar{x, 0, x + width / 24, height};
        dev->ColorFill(bb, &bar, D3DCOLOR_XRGB(230, 230, 230));
        if (jitter_ms > 0) {
            // the draw thread taking a different time each frame before its Present
            const double until = t + (std::rand() % 1000) / 1000.0 * jitter_ms / 1000.0;
            for (;;) {
                LARGE_INTEGER q;
                QueryPerformanceCounter(&q);
                if (static_cast<double>(q.QuadPart - start.QuadPart) / freq.QuadPart >= until) break;
                YieldProcessor();
            }
        }
        if (use_dxgi) dxgipresent_frame_presented(start.QuadPart + static_cast<LONGLONG>(t * freq.QuadPart));
        hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
        pump();
        if (frames > 0 && t - last > worst) worst = t - last;
        last = t;
        ++frames;
    }
    std::printf("motion: %d frames in %.1f s = %.1f fps (target %.0f), longest gap %.2f ms, last present %08lX\n",
                frames, seconds, frames / seconds, fps, worst * 1000.0, hr);
    if (use_dxgi) {
        std::printf("Windows showed the frames: %s\n", dxgipresent_composition());
        dxgipresent_report();
    }
    bb->Release();
    dev->Release();
    d3d->Release();
    DestroyWindow(w);
    log_close();
    return 0;
}
