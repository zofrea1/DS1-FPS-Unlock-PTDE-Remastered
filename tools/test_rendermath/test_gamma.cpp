// GammaRamp against a real device: test_gamma.exe [d3d9|DXVK_DLL_PATH] [auto|true|false]
// Creates a windowed device on the system Direct3D 9 or on the DXVK DLL given, hooks it, sends a darkening ramp as
// the game would and reads back what reached the device. DXVK keeps the ramp it was given (GetGammaRamp).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <d3d9.h>

#include "gamma.cpp"

#pragma comment(lib, "d3d9.lib")

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "d3d9";
    const char* mode_name = argc > 2 ? argv[2] : "auto";
    const int mode = std::strcmp(mode_name, "true") == 0 ? kGammaOn : std::strcmp(mode_name, "false") == 0 ? kGammaOff : kGammaAuto;
    const bool dxvk = std::strcmp(which, "d3d9") != 0;
    IDirect3D9* d3d = nullptr;
    if (dxvk) {
        HMODULE m = LoadLibraryA(which);
        if (!m) { std::printf("cannot load %s\n", which); return 2; }
        auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(m, "Direct3DCreate9"));
        d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    } else {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
    }
    if (!d3d) { std::printf("no Direct3D 9\n"); return 2; }
    HWND w = CreateWindowExA(0, "STATIC", "gamma", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = w;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { std::printf("CreateDevice %08lX\n", hr); return 2; }
    gamma_hook_device(dev, mode, dxvk);
    D3DGAMMARAMP ramp{}, back{}, flat{};
    for (int i = 0; i < 256; ++i) {
        const WORD v = static_cast<WORD>(static_cast<int>(std::pow(i / 255.0, 1.2) * 65535.0 + 0.5) & 0xFF00);
        ramp.red[i] = ramp.green[i] = ramp.blue[i] = v;
    }
    identity(&flat);
    for (int k = 0; k < 3; ++k) dev->SetGammaRamp(0, 0, &ramp);  // the repeats log nothing
    dev->GetGammaRamp(0, &back);
    const bool got_game = std::memcmp(&back, &ramp, sizeof(ramp)) == 0;
    const bool got_flat = std::memcmp(&back, &flat, sizeof(flat)) == 0;
    std::printf("device holds %s\n", got_game ? "the game's ramp" : got_flat ? "identity" : "something else");
    dev->Release();
    d3d->Release();
    return 0;
}
