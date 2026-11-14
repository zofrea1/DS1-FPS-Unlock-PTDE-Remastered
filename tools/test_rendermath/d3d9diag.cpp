// d3d9diag.exe: why a windowed Direct3D 9 device cannot be created: the adapter's display mode, the format checks
// and CreateDevice with a few different parameters. Prints one line per step.
#include <windows.h>

#include <d3d9.h>

#include <cstdio>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")

LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(w, m, wp, lp); }

int main() {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { std::printf("Direct3DCreate9 failed\n"); return 2; }
    std::printf("adapters %u\n", d3d->GetAdapterCount());
    D3DADAPTER_IDENTIFIER9 id{};
    HRESULT hr = d3d->GetAdapterIdentifier(0, 0, &id);
    std::printf("identifier %08lX %s on %s\n", hr, id.Description, id.DeviceName);
    D3DDISPLAYMODE dm{};
    hr = d3d->GetAdapterDisplayMode(0, &dm);
    std::printf("display mode %08lX %ux%u @ %u format %d\n", hr, dm.Width, dm.Height, dm.RefreshRate, static_cast<int>(dm.Format));
    std::printf("monitor %p\n", static_cast<void*>(d3d->GetAdapterMonitor(0)));
    D3DCAPS9 caps{};
    hr = d3d->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps);
    std::printf("caps %08lX devcaps %08lX\n", hr, caps.DevCaps);
    const D3DFORMAT backs[] = {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8};
    for (D3DFORMAT bb : backs) {
        hr = d3d->CheckDeviceType(0, D3DDEVTYPE_HAL, dm.Format, bb, TRUE);
        std::printf("CheckDeviceType display %d back %d windowed: %08lX\n", static_cast<int>(dm.Format), static_cast<int>(bb), hr);
    }
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "d3d9diag";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "d3d9diag", "d3d9diag", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 360, nullptr,
                                nullptr, wc.hInstance, nullptr);
    std::printf("window %p\n", static_cast<void*>(hwnd));
    struct Try { const char* name; D3DDEVTYPE type; DWORD flags; D3DFORMAT fmt; D3DSWAPEFFECT fx; UINT interval; };
    const Try tries[] = {
        {"HAL hardware-vp X8R8G8B8 discard default", D3DDEVTYPE_HAL, D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DFMT_X8R8G8B8, D3DSWAPEFFECT_DISCARD, D3DPRESENT_INTERVAL_DEFAULT},
        {"HAL software-vp UNKNOWN discard default", D3DDEVTYPE_HAL, D3DCREATE_SOFTWARE_VERTEXPROCESSING, D3DFMT_UNKNOWN, D3DSWAPEFFECT_DISCARD, D3DPRESENT_INTERVAL_DEFAULT},
        {"HAL hardware-vp UNKNOWN copy default", D3DDEVTYPE_HAL, D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DFMT_UNKNOWN, D3DSWAPEFFECT_COPY, D3DPRESENT_INTERVAL_DEFAULT},
        {"NULLREF software-vp", D3DDEVTYPE_NULLREF, D3DCREATE_SOFTWARE_VERTEXPROCESSING, D3DFMT_UNKNOWN, D3DSWAPEFFECT_DISCARD, D3DPRESENT_INTERVAL_DEFAULT},
    };
    for (const Try& t : tries) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.hDeviceWindow = hwnd;
        pp.BackBufferFormat = t.fmt;
        pp.SwapEffect = t.fx;
        pp.PresentationInterval = t.interval;
        IDirect3DDevice9* dev = nullptr;
        hr = d3d->CreateDevice(0, t.type, hwnd, t.flags, &pp, &dev);
        std::printf("CreateDevice %s: %08lX (last error %lu)\n", t.name, hr, GetLastError());
        if (dev) dev->Release();
    }
    IDirect3D9Ex* ex = nullptr;
    hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &ex);
    std::printf("Direct3DCreate9Ex %08lX\n", hr);
    if (ex) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.hDeviceWindow = hwnd;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        IDirect3DDevice9Ex* dev = nullptr;
        hr = ex->CreateDeviceEx(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, nullptr, &dev);
        std::printf("CreateDeviceEx: %08lX\n", hr);
        if (dev) dev->Release();
        ex->Release();
    }
    DestroyWindow(hwnd);
    d3d->Release();
    return 0;
}
