// FixSun beside DSfix 3.0 (src/ptde/resolution.cpp): the GetData hook on a real Direct3D 9 device's query method
// table, and who asks for the count. sunquery_game\game.dll stands for the game (or DSfix 2.x, which passes queries
// through); sunquery_dll\dinput8.dll stands for DSfix 3.0's query wrapper, which calls the real GetData and divides
// the count. The exe itself stands for a diagnostics hook of the mod sitting in front (no decision, DSfix 2.x assumed).
// The game patch itself (0xFD3A52) is not exercised; g_sun_* are set as resolution_dsfix_sun would set them.
// Build: build_sunquery.bat. Exit code 0 = all cases pass.
#include "../../src/ptde/resolution.cpp"

#include <cstdio>

namespace {

using CallFn = HRESULT(__cdecl*)(IDirect3DQuery9*, void*, DWORD, DWORD);

__declspec(noinline) HRESULT __cdecl exe_call(IDirect3DQuery9* q, void* data, DWORD size, DWORD flags) {
    const HRESULT hr = q->GetData(data, size, flags);
    return hr;
}

IDirect3DDevice9* make_device() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"sunquery";
    RegisterClassW(&wc);
    HWND w = CreateWindowW(L"sunquery", L"sunquery", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                           wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return nullptr;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = w;
    IDirect3DDevice9* dev = nullptr;
    d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    return dev;
}

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_fail;
}

void* slot7(IDirect3DQuery9* q) { return (*reinterpret_cast<void***>(q))[7]; }

// One case: fresh state, hook, a timestamp query first (must not decide), then the occlusion count.
void run(IDirect3DDevice9* dev, const char* name, double probe, double dsfix, CallFn call, double want) {
    std::printf("-- %s\n", name);
    g_sun_patched = true;
    g_sun_probe = probe;
    g_sun_dsfix = dsfix;
    g_sun_scale = kSunScaleGame * probe;
    g_sun_checked = 0;
    o_query_get_data = nullptr;
    IDirect3DQuery9* occ = nullptr;
    IDirect3DQuery9* ts = nullptr;
    dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occ);
    dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &ts);
    void* original = slot7(occ);
    resolution_sun_hook_device(dev);
    check(slot7(occ) == reinterpret_cast<void*>(&hk_query_get_data), "GetData hooked on the shared method table");
    check(o_query_get_data != nullptr, "original kept");

    dev->BeginScene();
    occ->Issue(D3DISSUE_BEGIN);
    occ->Issue(D3DISSUE_END);
    if (ts) ts->Issue(D3DISSUE_END);
    dev->EndScene();

    UINT64 t = 0;
    if (ts) call(ts, &t, sizeof(t), D3DGETDATA_FLUSH);
    check(g_sun_checked == 0, "a timestamp query decides nothing");

    DWORD pixels = 0;
    HRESULT hr = S_FALSE;
    for (int i = 0; i < 1000 && hr == S_FALSE; ++i) {
        hr = call(occ, &pixels, sizeof(pixels), D3DGETDATA_FLUSH);
    }
    check(SUCCEEDED(hr), "occlusion count read through the hook");
    check(g_sun_checked == 1, "decided at the first occlusion count");
    char line[160];
    std::snprintf(line, sizeof(line), "expected-count scale %.6f (want %.6f)", g_sun_scale, want);
    check(std::fabs(g_sun_scale - want) < 1e-9, line);
    check(slot7(occ) == original, "original GetData back in the table");
    if (ts) ts->Release();
    occ->Release();
}

CallFn load_call(const wchar_t* dir, const wchar_t* rel) {
    wchar_t path[MAX_PATH];
    lstrcpyW(path, dir);
    lstrcatW(path, rel);
    HMODULE m = LoadLibraryW(path);
    return m ? reinterpret_cast<CallFn>(GetProcAddress(m, "call_get_data")) : nullptr;
}

}  // namespace

int main() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    log_init(exe, L"test_sunquery.log");
    wchar_t dir[MAX_PATH];
    lstrcpyW(dir, exe);
    *(wcsrchr(dir, L'\\') + 1) = 0;
    const CallFn dsfix30 = load_call(dir, L"sunquery_dll\\dinput8.dll");
    const CallFn game = load_call(dir, L"sunquery_game\\game.dll");
    if (!dsfix30 || !game) {
        std::printf("FAIL  could not load the stand-in DLLs beside the exe\n");
        return 2;
    }
    IDirect3DDevice9* dev = make_device();
    if (!dev) {
        std::printf("FAIL  no Direct3D 9 device\n");
        return 2;
    }
    const double k4 = 3840.0 * 2160.0 / (1024.0 * 720.0);  // 11.25
    run(dev, "DSfix 2.x resolution (count read by the game)", k4, k4, game, 0.8 * k4);
    run(dev, "DSfix 3.0 resolution (count read by DSfix's wrapper)", k4, k4, dsfix30, 0.8);
    run(dev, "our resolution beside DSfix 3.0 at 4K", 1.0, k4, dsfix30, 0.8 / k4);
    run(dev, "our resolution beside DSfix 2.x", 1.0, k4, game, 0.8);
    run(dev, "a diagnostics hook of the mod in front (no decision: DSfix 2.x assumed)", k4, k4, exe_call, 0.8 * k4);
    dev->Release();
    log_close();
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
