#include "texpreload.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>

namespace {

// The game makes its textures from the DDS data in memory with d3dx9_43!D3DXCreateTextureFromFileInMemory(Ex) and
// D3DXCreateCubeTextureFromFileInMemoryEx, on its loading thread while a map part streams in, in the managed pool.
// A managed texture reaches the graphics card only when something is first drawn with it, on the render thread, in
// the middle of a frame: as a map part appears, dozens of first draws each wait 3-8 ms in the driver, and the game
// thread waits for the render thread (the hitches on entering an area). Here a managed texture is uploaded
// (IDirect3DResource9::PreLoad) right after it is made, on the thread that made it, before anything draws with it.
// The device is multithreaded (D3DCREATE_MULTITHREADED), so this is allowed there. While the device is lost (the game
// in exclusive fullscreen and in the background) nothing is uploaded: the texture stays as the game made it and goes
// up at its first draw after the device is reset, as without this.
using MemFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const void*, UINT, IDirect3DTexture9**);
using MemExFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const void*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                 DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DTexture9**);
using CubeExFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const void*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD,
                                  DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DCubeTexture9**);
MemFn g_mem = nullptr;
MemExFn g_mem_ex = nullptr;
CubeExFn g_cube_ex = nullptr;

volatile LONG g_count = 0;
volatile LONG g_skipped = 0;  // made while the device was lost
volatile LONG64 g_ticks = 0;
LONGLONG g_freq = 0;

void preload(IDirect3DDevice9* dev, IDirect3DBaseTexture9* t) {
    if (dev && dev->TestCooperativeLevel() != D3D_OK) {
        InterlockedIncrement(&g_skipped);
        return;
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    t->PreLoad();
    QueryPerformanceCounter(&b);
    InterlockedIncrement(&g_count);
    InterlockedExchangeAdd64(&g_ticks, b.QuadPart - a.QuadPart);
}

HRESULT WINAPI hk_mem(IDirect3DDevice9* dev, const void* data, UINT size, IDirect3DTexture9** out) {
    const HRESULT hr = g_mem(dev, data, size, out);
    if (SUCCEEDED(hr) && out && *out) preload(dev, *out);  // D3DX's default pool: managed
    return hr;
}

HRESULT WINAPI hk_mem_ex(IDirect3DDevice9* dev, const void* data, UINT size, UINT w, UINT h, UINT mips, DWORD usage,
                         D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mip_filter, D3DCOLOR key, void* info,
                         PALETTEENTRY* pal, IDirect3DTexture9** out) {
    const HRESULT hr = g_mem_ex(dev, data, size, w, h, mips, usage, fmt, pool, filter, mip_filter, key, info, pal, out);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_MANAGED) preload(dev, *out);
    return hr;
}

HRESULT WINAPI hk_cube_ex(IDirect3DDevice9* dev, const void* data, UINT size, UINT edge, UINT mips, DWORD usage,
                          D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mip_filter, D3DCOLOR key, void* info,
                          PALETTEENTRY* pal, IDirect3DCubeTexture9** out) {
    const HRESULT hr = g_cube_ex(dev, data, size, edge, mips, usage, fmt, pool, filter, mip_filter, key, info, pal, out);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_MANAGED) preload(dev, *out);
    return hr;
}

// The game's import slot for dll!name, or null.
void** import_slot(const char* dll, const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        const auto* names =
            reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<void**>(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const auto* n = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(n->Name), name) == 0) return &slots[i];
        }
    }
    return nullptr;
}

// The slot's current value (whatever another hook put there, or D3DX itself) is kept and called.
template <typename Fn>
bool take(const char* name, void* hook, Fn* orig) {
    void** slot = import_slot("d3dx9_43.dll", name);
    DWORD old = 0;
    if (!slot || !*slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = reinterpret_cast<Fn>(*slot);
    InterlockedExchangePointer(slot, hook);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

}  // namespace

void texpreload_install() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    int n = 0;
    n += take("D3DXCreateTextureFromFileInMemory", reinterpret_cast<void*>(&hk_mem), &g_mem);
    n += take("D3DXCreateTextureFromFileInMemoryEx", reinterpret_cast<void*>(&hk_mem_ex), &g_mem_ex);
    n += take("D3DXCreateCubeTextureFromFileInMemoryEx", reinterpret_cast<void*>(&hk_cube_ex), &g_cube_ex);
    if (n) {
        LOG_INFO("PreloadTextures: textures go to the graphics card as they are loaded, not at their first draw (%d of 3 "
                 "D3DX imports)", n);
    } else {
        LOG_ERROR("PreloadTextures: the game's D3DX texture imports were not found");
    }
}

void texpreload_report() {
    const LONG n = InterlockedExchange(&g_count, 0);
    const LONG64 t = InterlockedExchange64(&g_ticks, 0);
    const LONG lost = InterlockedExchange(&g_skipped, 0);
    if (lost) LOG_INFO("PreloadTextures: %ld textures made while the device was lost (left for their first draw)", lost);
    if (n && g_freq) {
        LOG_INFO("PreloadTextures: %ld textures uploaded as they were loaded (%.1f ms on the loading threads)", n,
                 1000.0 * static_cast<double>(t) / static_cast<double>(g_freq));
    }
}
