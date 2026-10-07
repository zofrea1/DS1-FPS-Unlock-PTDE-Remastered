#include "d3d.h"

#include "aspect.h"
#include "dxvk.h"
#include "log.h"
#include "settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

HMODULE g_self = nullptr;
Settings g_settings;
volatile LONG g_settings_state = 0;  // 0 = not loaded, 1 = loading, 2 = loaded

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                        IDirect3DDevice9**);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

CreateFn g_create = nullptr;
CreateDeviceFn g_create_device = nullptr;
ResetFn g_reset = nullptr;

constexpr int kCreateDeviceSlot = 16;  // IDirect3D9::CreateDevice
constexpr int kResetSlot = 16;         // IDirect3DDevice9::Reset
constexpr int kSetVsConstSlot = 94;    // IDirect3DDevice9::SetVertexShaderConstantF

using SetVsConstFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
SetVsConstFn g_set_vs_const = nullptr;
bool g_probe = false;
unsigned long long g_frame_hash = 1469598103934665603ull;
unsigned long long g_small_hash = 1469598103934665603ull;  // uploads of up to 8 registers (camera, world matrices)
unsigned long long g_big_hash = 1469598103934665603ull;    // larger uploads (skinning palettes)

HRESULT WINAPI hook_set_vs_const(IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    float corrected[24 * 4];
    const float* use = data;
    if (data && aspect_correct_constants(data, count, corrected)) {
        use = corrected;
    }
    if (g_probe && data && count >= 3 && count < 512) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        unsigned long long h = g_frame_hash ^ start;
        h *= 1099511628211ull;
        for (UINT i = 0; i < count * 16; ++i) {
            h = (h ^ bytes[i]) * 1099511628211ull;
        }
        g_frame_hash = h;
        unsigned long long& part = count <= 8 ? g_small_hash : g_big_hash;
        unsigned long long p = part ^ start;
        p *= 1099511628211ull;
        for (UINT i = 0; i < count * 16; ++i) {
            p = (p ^ bytes[i]) * 1099511628211ull;
        }
        part = p;
    }
    return g_set_vs_const(device, start, use, count);
}

const Settings& settings() {
    if (g_settings_state != 2) {
        if (InterlockedCompareExchange(&g_settings_state, 1, 0) == 0) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(g_self, path, MAX_PATH);
            g_settings = settings_load(path);
            g_settings_state = 2;
        } else {
            while (g_settings_state != 2) {
                Sleep(1);
            }
        }
    }
    return g_settings;
}

// Fullscreen modes the display offers at the requested size, and the one to use.
void adjust_present_parameters(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp, const char* where) {
    const Settings& cfg = settings();
    if (!pp || pp->Windowed || cfg.fullscreen_refresh < 0) {
        return;
    }
    const D3DFORMAT format = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
    const UINT count = d3d->GetAdapterModeCount(adapter, format);
    UINT rates[64] = {};
    int rate_count = 0;
    for (UINT i = 0; i < count; ++i) {
        D3DDISPLAYMODE mode{};
        if (FAILED(d3d->EnumAdapterModes(adapter, format, i, &mode))) {
            continue;
        }
        if (mode.Width != pp->BackBufferWidth || mode.Height != pp->BackBufferHeight) {
            continue;
        }
        bool seen = false;
        for (int r = 0; r < rate_count; ++r) {
            seen |= rates[r] == mode.RefreshRate;
        }
        if (!seen && rate_count < 64) {
            rates[rate_count++] = mode.RefreshRate;
        }
    }
    char list[512] = {};
    int n = 0;
    for (int r = 0; r < rate_count; ++r) {
        n += std::snprintf(list + n, sizeof(list) - n, "%s%u", r ? ", " : "", rates[r]);
    }
    const UINT requested = pp->FullScreen_RefreshRateInHz;
    if (rate_count == 0) {
        LOG_INFO("%s: fullscreen %ux%u, no display modes listed at that size; leaving the refresh rate at %u Hz",
                 where, pp->BackBufferWidth, pp->BackBufferHeight, requested);
        return;
    }
    UINT chosen = 0;
    if (cfg.fullscreen_refresh > 0) {
        // Exact match if offered, otherwise the highest rate not above the request.
        for (int r = 0; r < rate_count; ++r) {
            if (rates[r] == static_cast<UINT>(cfg.fullscreen_refresh)) {
                chosen = rates[r];
            }
        }
        if (chosen == 0) {
            for (int r = 0; r < rate_count; ++r) {
                if (rates[r] <= static_cast<UINT>(cfg.fullscreen_refresh) && rates[r] > chosen) {
                    chosen = rates[r];
                }
            }
        }
    }
    if (chosen == 0) {
        for (int r = 0; r < rate_count; ++r) {
            if (rates[r] > chosen) {
                chosen = rates[r];
            }
        }
    }
    LOG_INFO("%s: fullscreen %ux%u offers [%s] Hz; game asked for %u Hz, using %u Hz", where, pp->BackBufferWidth,
             pp->BackBufferHeight, list, requested, chosen);
    pp->FullScreen_RefreshRateInHz = chosen;
}

HRESULT WINAPI hook_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    IDirect3D9* d3d = nullptr;
    if (pp && SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
        aspect_set_enabled(settings().fps_unlock && settings().fix_aspect);
        if (aspect_is_enabled()) {
            aspect_adjust_present(d3d, D3DADAPTER_DEFAULT, pp, "Reset");
        }
        if (!pp->Windowed) {
            adjust_present_parameters(d3d, D3DADAPTER_DEFAULT, pp, "Reset");
        }
        d3d->Release();
    }
    const HRESULT hr = g_reset(device, pp);
    if (SUCCEEDED(hr) && aspect_is_active()) {
        aspect_hook_device(device);
    }
    return hr;
}

// Patch one slot of a COM object's vtable, returning the original pointer.
void* patch_slot(void* object, int slot, void* replacement) {
    void** vtable = *reinterpret_cast<void***>(object);
    void* original = vtable[slot];
    if (original == replacement) {
        return nullptr;
    }
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) {
        return nullptr;
    }
    vtable[slot] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &ignored);
    return original;
}

HRESULT WINAPI hook_create_device(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                  D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    aspect_set_enabled(settings().fps_unlock && settings().fix_aspect);
    if (aspect_is_enabled()) {
        aspect_adjust_present(d3d, adapter, pp, "CreateDevice");
    }
    adjust_present_parameters(d3d, adapter, pp, "CreateDevice");
    const HRESULT hr = g_create_device(d3d, adapter, type, window, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        void* original = patch_slot(*out, kResetSlot, reinterpret_cast<void*>(&hook_reset));
        if (original && !g_reset) {
            g_reset = reinterpret_cast<ResetFn>(original);
            LOG_INFO("Device Reset hooked");
        }
        // Aspect rewrites the projection on the way through this hook. The content probe only hashes.
        g_probe = settings().content_probe;
        void* vs = (g_probe || aspect_is_active())
                       ? patch_slot(*out, kSetVsConstSlot, reinterpret_cast<void*>(&hook_set_vs_const))
                       : nullptr;
        if (vs && !g_set_vs_const) {
            g_set_vs_const = reinterpret_cast<SetVsConstFn>(vs);
            LOG_INFO("Device SetVertexShaderConstantF hooked%s", g_probe ? " (content-cadence probe)" : "");
        }
        if (aspect_is_active()) {
            aspect_hook_device(*out);
        }
    }
    return hr;
}

// DXVK goes in before the game's first call reaches the system Direct3DCreate9, so whatever
// wraps that function (DSfix) wraps DXVK.
void route_to_dxvk() {
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) {
        slash[1] = 0;
    } else {
        dir[0] = 0;
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s%s", dir, kDxvkFileName);
    dxvk_route(path);
}

IDirect3D9* WINAPI hook_direct3d_create9(UINT sdk_version) {
    // The import slot is patched from DllMain, before the INI can be read; with FPSUnlock false
    // Direct3D and the device are left alone.
    if (settings().fps_unlock && settings().dxvk) {
        route_to_dxvk();
    }
    IDirect3D9* d3d = g_create(sdk_version);
    if (d3d && settings().fps_unlock) {
        void* original = patch_slot(d3d, kCreateDeviceSlot, reinterpret_cast<void*>(&hook_create_device));
        if (original && !g_create_device) {
            g_create_device = reinterpret_cast<CreateDeviceFn>(original);
            LOG_INFO("IDirect3D9::CreateDevice hooked");
        }
    }
    return d3d;
}

// Finds the game's import-table slot for d3d9.dll!Direct3DCreate9.
void** find_create_slot() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return nullptr;
    }
    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), "d3d9.dll") != 0) {
            continue;
        }
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* slots = reinterpret_cast<void**>(base + desc->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) {
                continue;
            }
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "Direct3DCreate9") == 0) {
                return &slots[i];
            }
        }
    }
    return nullptr;
}

}  // namespace

void d3d_install_early(HMODULE self) {
    g_self = self;
    void** slot = find_create_slot();
    if (!slot) {
        return;
    }
    g_create = reinterpret_cast<CreateFn>(*slot);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return;
    }
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hook_direct3d_create9));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
}

unsigned long long d3d_take_frame_hash(unsigned long long* small_hash, unsigned long long* big_hash) {
    const unsigned long long h = g_frame_hash;
    *small_hash = g_small_hash;
    *big_hash = g_big_hash;
    g_frame_hash = g_small_hash = g_big_hash = 1469598103934665603ull;
    return h;
}
