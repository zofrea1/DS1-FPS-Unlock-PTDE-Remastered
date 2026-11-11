// A game-like fullscreen Direct3D 9 device for checking variable refresh: vrrprobe.exe [d3d9|DXVK_DLL_PATH]
//   [full|window] [seconds]
// Covers the primary monitor (exclusive fullscreen or a borderless window), clears to alternating greys and presents
// immediately (no vsync) at an uneven ~100 frames a second, the case where G-SYNC / VRR should follow the frame rate.
// Run tools-side NvAPI polling (or watch the monitor's refresh counter) meanwhile.
#include <windows.h>

#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

static double g_fps = 0;

LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(w, m, wp, lp); }

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "d3d9";
    const bool full = argc > 2 ? std::strcmp(argv[2], "full") == 0 : true;
    const double seconds = argc > 3 ? std::atof(argv[3]) : 5.0;
    if (const char* e = std::getenv("VRRPROBE_FPS")) g_fps = std::atof(e);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
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
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "vrrprobe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, "vrrprobe", "vrrprobe", WS_POPUP | WS_VISIBLE, mi.rcMonitor.left,
                                mi.rcMonitor.top, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { std::printf("CreateWindow failed %lu\n", GetLastError()); return 2; }
    SetForegroundWindow(hwnd);
    // As the game does: a windowed device first, then Reset to fullscreen.
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        std::printf("CreateDevice %08lX with immediate presents, trying the default interval\n", hr);
        pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    if (FAILED(hr)) { std::printf("CreateDevice %08lX\n", hr); return 2; }
    if (full) {
        D3DDISPLAYMODE dm{};
        d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm);
        pp.Windowed = FALSE;
        pp.FullScreen_RefreshRateInHz = argc > 4 ? static_cast<UINT>(std::atoi(argv[4])) : dm.RefreshRate;
        std::printf("desktop mode %ux%u @ %u Hz, format %d\n", dm.Width, dm.Height, dm.RefreshRate, static_cast<int>(dm.Format));
        hr = dev->Reset(&pp);
        if (FAILED(hr)) { std::printf("Reset to fullscreen %08lX\n", hr); return 2; }
    }
    std::printf("%s %s %dx%d @ %u Hz\n", dxvk ? "DXVK" : "system Direct3D 9", full ? "exclusive fullscreen" : "borderless window",
                w, h, pp.FullScreen_RefreshRateInHz);
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int frame = 0;
    for (;;) {
        QueryPerformanceCounter(&t);
        const double now = static_cast<double>(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (now > seconds) break;
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        const int g = (frame & 1) ? 90 : 110;
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(g, g, g), 1.0f, 0);
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        ++frame;
        if (g_fps > 0) {  // VRRPROBE_FPS: paced to that rate (busy wait), even frames
            const double due = static_cast<double>(frame) / g_fps;
            for (;;) {
                QueryPerformanceCounter(&t);
                if (static_cast<double>(t.QuadPart - t0.QuadPart) / f.QuadPart >= due) break;
                if (due - static_cast<double>(t.QuadPart - t0.QuadPart) / f.QuadPart > 0.002) Sleep(1);
            }
        } else {
            Sleep(8 + (frame % 5));  // about 100 fps, uneven
        }
    }
    std::printf("%d frames in %.1f s\n", frame, seconds);
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
