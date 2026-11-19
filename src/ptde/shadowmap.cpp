#include "shadowmap.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

// The side of the shadow map is one number in the game's data (0x12DFF34, 2048). The game reads it when it makes the
// shadow map (0x100A32B: a packed-depth colour target and its depth), for the viewport of each slice (0xEB4856: with
// four slices each has a quarter, half the side) and for the half-texel step of the matrix that looks the shadow up
// (0xF477DB). Everything else works in the map's own 0..1 coordinates, so a larger number gives the same shadows
// from more texels.
//
// A depth bias (ShadowBias, 2.3.0 to 2.5.0) is gone. It took a constant off the light's clip depth for the shadow
// lookup the game uses when a character or object lies inside one slice of the map (vertex constants c137..c140).
// The slices are perspective ones (map position and depth divided by the distance ahead of the camera): in Quelaag's
// Domain the depth row was 0.90 w - 0.138 y, so 0.006 off the depth was 0.0435 * w of height, about 22 cm at 5 m,
// and real shadows went: an egg carrier's top no longer shadowed its lower sacs. The game's other lookup (the slice
// picked per pixel, for whatever crosses a slice's edge; pixel constants c140..c160) had no bias, so a character lit
// up or darkened as it crossed those edges (2026-11-18). A larger map is what keeps the texel weave fine.
//
// Beside DSfix 3.0: the game draws each slice with a scissor rectangle around it (with four slices on a 4096 map:
// 0,0-2048,2048, then 2048,0-4096,2048, 0,2048-2048,4096 and 2048,2048-4096,4096). DSfix 3.0 (the 2026-11-16 build)
// hands the game a device of its own, and the last three of those rectangles never reached Direct3D through it: the
// device kept the first one, so the draws of the other three slices fell outside it and only the slice nearest the
// camera had shadows. Shadows further ahead then appeared only up close, at a hard line (Anor Londo, 2026-11-19; the
// game's own 2048 map is not affected, its slices all lie inside 0,0-2048,2048). So when the game's device is a
// wrapper's, every scissor rectangle the game sets while the shadow map is its render target is checked on the device
// beneath the wrapper and put back there when the wrapper did not pass it on.
namespace {
constexpr uintptr_t kShadowMapSize = 0x012DFF34;
constexpr int kSetRenderTargetSlot = 37;
constexpr int kSetScissorSlot = 75;

int g_size = 2048;
IDirect3DDevice9* g_real = nullptr;  // the device beneath a wrapper (one reference, for the device's life)
volatile bool g_on_map = false;      // the shadow map is render target 0
std::atomic<long> g_put_back{0};

using SetRtFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetScissorFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*);
SetRtFn g_set_rt = nullptr;
SetScissorFn g_set_scissor = nullptr;

void* patch_slot_here(void* object, int slot, void* replacement) {
    void** vtable = *reinterpret_cast<void***>(object);
    void* original = vtable[slot];
    if (original == replacement) return nullptr;
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    vtable[slot] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &ignored);
    return original;
}

HRESULT WINAPI hk_set_rt(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface) {
    const HRESULT hr = g_set_rt(dev, index, surface);
    if (index == 0) {
        D3DSURFACE_DESC d{};
        g_on_map = surface && SUCCEEDED(surface->GetDesc(&d)) && d.Width == static_cast<UINT>(g_size) &&
                   d.Height == static_cast<UINT>(g_size);
    }
    return hr;
}

HRESULT WINAPI hk_set_scissor(IDirect3DDevice9* dev, const RECT* r) {
    const HRESULT hr = g_set_scissor(dev, r);
    if (g_on_map && r && g_real) {
        RECT now{};
        if (SUCCEEDED(g_real->GetScissorRect(&now)) &&
            (now.left != r->left || now.top != r->top || now.right != r->right || now.bottom != r->bottom)) {
            g_real->SetScissorRect(r);
            if (g_put_back.fetch_add(1) == 0) {
                LOG_INFO("ShadowMapResolution: the shadow map's scissor rectangle %ld,%ld-%ld,%ld did not reach "
                         "Direct3D through the device wrapper (DSfix), which kept %ld,%ld-%ld,%ld; the game's "
                         "rectangles are put back beneath it so every slice of the %dx%d map is drawn",
                         r->left, r->top, r->right, r->bottom, now.left, now.top, now.right, now.bottom, g_size,
                         g_size);
            }
        }
    }
    return hr;
}
}  // namespace

bool shadowmap_set_resolution(int size) {
    if (size != 4096 && size != 8192) return size == 2048;
    auto* p = reinterpret_cast<int32_t*>(kShadowMapSize);
    if (*p != 2048) {
        LOG_ERROR("ShadowMapResolution: the shadow map size at %08X is %d, not 2048 (another build or mod?). Not "
                  "changing it.", static_cast<unsigned>(kShadowMapSize), *p);
        return false;
    }
    *p = size;
    g_size = size;
    LOG_INFO("ShadowMapResolution: the shadow map is %dx%d (the game's own: 2048x2048)", size, size);
    return true;
}

void shadowmap_hook_device(IDirect3DDevice9* dev) {
    if (!dev || g_size == 2048 || g_set_scissor) return;
    IDirect3DVertexBuffer9* vb = nullptr;
    if (FAILED(dev->CreateVertexBuffer(16, D3DUSAGE_WRITEONLY, 0, D3DPOOL_SYSTEMMEM, &vb, nullptr)) || !vb) return;
    IDirect3DDevice9* real = nullptr;
    if (FAILED(vb->GetDevice(&real))) real = nullptr;
    vb->Release();
    if (!real) return;
    if (real == dev) {  // no wrapper: the game's rectangles reach Direct3D as they are
        real->Release();
        return;
    }
    g_real = real;
    void* rt = patch_slot_here(dev, kSetRenderTargetSlot, reinterpret_cast<void*>(&hk_set_rt));
    void* sc = patch_slot_here(dev, kSetScissorSlot, reinterpret_cast<void*>(&hk_set_scissor));
    if (!rt || !sc) {
        LOG_ERROR("ShadowMapResolution: could not watch the shadow map's scissor rectangles under the device wrapper");
        if (rt) patch_slot_here(dev, kSetRenderTargetSlot, rt);
        if (sc) patch_slot_here(dev, kSetScissorSlot, sc);
        return;
    }
    g_set_rt = reinterpret_cast<SetRtFn>(rt);
    g_set_scissor = reinterpret_cast<SetScissorFn>(sc);
    LOG_INFO("ShadowMapResolution: the game's device is a wrapper's (DSfix); the shadow map's scissor rectangles are "
             "checked beneath it");
}
