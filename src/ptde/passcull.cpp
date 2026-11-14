#include "passcull.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdint>

// In some maps (Blighttown: 643 of the 2,600 draws of a heavy view) the game draws the scene a second time, small
// (256x180 with its own depth, cleared to grey 0x404040), right after the shadow map: mirrored and cut off at a
// plane, the picture for a mirroring surface. It draws it every frame, also when nothing on screen shows such a
// surface; in Blighttown nothing was seen to read it at all (the heavy view from the upper level, the swamp). The
// picture is in one of the two 256x180 targets the post-processing later uses for its blurs, so it only lives
// until that stage of the same frame.
//
// Measured in the heavy Blighttown view (Direct3D 9, 3840x2160): 113.4 / 116.1 fps drawn, 119.5 / 120.1 skipped.
//
// SkipUnusedReflection: when no draw of the last frames had that picture bound between the pass and the blurs that
// overwrite it, the pass's clear and draws are not sent (its state changes are, so the device is where the game
// thinks it is). Every frame is watched for a draw that has the texture bound; at the first one the pass is drawn
// again from the next frame, and that one frame samples a kept copy of the last picture that was drawn instead of
// the blur left in the target.
namespace {

using SetRtFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using ClearFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetTexFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIdxFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIdxUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                     const void*, UINT);

SetRtFn o_set_rt = nullptr;
ClearFn o_clear = nullptr;
SetTexFn o_set_tex = nullptr;
DrawFn o_draw = nullptr;
DrawIdxFn o_draw_idx = nullptr;
DrawUpFn o_draw_up = nullptr;
DrawIdxUpFn o_draw_idx_up = nullptr;

constexpr UINT kPassW = 256, kPassH = 180;
constexpr D3DCOLOR kPassClear = 0x00404040;
constexpr int kStages = 16;
constexpr int kUnusedFramesToSkip = 3;

// The render thread's.
volatile LONG g_mode = 0;                        // 0 off, 1 skip when unused, 2 always skip (development)
IDirect3DSurface9* g_rt0 = nullptr;              // the device's target 0 (identity only)
bool g_rt0_small = false;
IDirect3DSurface9* g_small_seen[4] = {};         // surfaces already measured: 256x180 ones
IDirect3DSurface9* g_other_seen[8] = {};         // and others
int g_small_next = 0, g_other_next = 0;
IDirect3DSurface9* g_pass_surface = nullptr;     // the reflection's target (identity only)
IDirect3DBaseTexture9* g_pass_texture = nullptr; // its texture (identity only)
IDirect3DBaseTexture9* g_bound[kStages] = {};    // what the game last set (identity only)
uint32_t g_bound_mask = 0;                       // stages where the game has the reflection's texture
uint32_t g_sub_mask = 0;                         // stages where the device has our copy instead
bool g_in_pass = false, g_skipping = false;
bool g_window = false;                           // after the pass, until its target is drawn over
bool g_window_skipped = false;                   // the pass of this window was not drawn
bool g_used = false;
int g_unused_frames = 0;
IDirect3DTexture9* g_copy = nullptr;             // the last reflection that was drawn (ours)
bool g_copy_valid = false;
volatile LONG g_stat_skipped = 0, g_stat_drawn = 0, g_stat_stale = 0;
bool g_logged = false;

bool is_small(IDirect3DSurface9* s) {
    if (!s) return false;
    for (IDirect3DSurface9* k : g_small_seen) {
        if (k == s) return true;
    }
    for (IDirect3DSurface9* k : g_other_seen) {
        if (k == s) return false;
    }
    D3DSURFACE_DESC d{};
    const bool fits = SUCCEEDED(s->GetDesc(&d)) && d.Width == kPassW && d.Height == kPassH &&
                       (d.Usage & D3DUSAGE_RENDERTARGET) != 0;
    if (fits) {
        g_small_seen[g_small_next++ & 3] = s;
    } else {
        g_other_seen[g_other_next++ & 7] = s;
    }
    return fits;
}

void end_window() {
    if (!g_window) return;
    g_window = false;
    if (g_used) {
        g_unused_frames = 0;
    } else if (g_unused_frames < 1000) {
        ++g_unused_frames;
    }
    g_used = false;
}

// A draw while the reflection's texture may be read.
inline void before_draw(IDirect3DDevice9* dev) {
    if (!g_window || !g_bound_mask) return;
    g_used = true;
    if (!g_window_skipped) return;
    // The pass was not drawn this frame: the target holds a blur. Our copy of the last drawn picture instead.
    const uint32_t need = g_bound_mask & ~g_sub_mask;
    if (!need) return;
    if (!g_copy_valid) {
        InterlockedIncrement(&g_stat_stale);
        return;
    }
    for (int s = 0; s < kStages; ++s) {
        if (need & (1u << s)) o_set_tex(dev, s, g_copy);
    }
    g_sub_mask |= need;
    InterlockedIncrement(&g_stat_stale);
}

void restore_bindings(IDirect3DDevice9* dev) {
    if (!g_sub_mask) return;
    for (int s = 0; s < kStages; ++s) {
        if ((g_sub_mask & (1u << s)) && g_bound[s] == g_pass_texture) o_set_tex(dev, s, g_pass_texture);
    }
    g_sub_mask = 0;
}

// After a pass that was drawn: keep its picture.
void keep_copy(IDirect3DDevice9* dev) {
    if (!g_copy) {
        if (FAILED(dev->CreateTexture(kPassW, kPassH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                      &g_copy, nullptr))) {
            g_copy = nullptr;
            return;
        }
    }
    IDirect3DSurface9* dst = nullptr;
    if (SUCCEEDED(g_copy->GetSurfaceLevel(0, &dst)) && dst) {
        g_copy_valid = SUCCEEDED(dev->StretchRect(g_pass_surface, nullptr, dst, nullptr, D3DTEXF_NONE));
        dst->Release();
    }
}

HRESULT WINAPI hk_set_rt(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface) {
    if (index == 0) {
        if (g_in_pass && surface != g_pass_surface) {
            g_in_pass = false;
            g_window = true;
            g_window_skipped = g_skipping;
            g_used = false;
            if (g_skipping) {
                InterlockedIncrement(&g_stat_skipped);
            } else {
                InterlockedIncrement(&g_stat_drawn);
                if (g_mode == 1) keep_copy(dev);
            }
            g_skipping = false;
        } else if (g_window && surface == g_pass_surface) {
            // The post-processing takes the target for its blurs: the picture is gone.
            restore_bindings(dev);
            end_window();
        }
        g_rt0 = surface;
        g_rt0_small = g_mode != 0 && is_small(surface);
    }
    return o_set_rt(dev, index, surface);
}

HRESULT WINAPI hk_clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                        DWORD stencil) {
    if (g_rt0_small && !g_in_pass && g_mode != 0 && count == 0 &&
        flags == (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL) && (color & 0x00FFFFFF) == kPassClear) {
        if (g_window) {  // a frame without the blurs
            restore_bindings(dev);
            end_window();
        }
        D3DSURFACE_DESC d{};  // measured again here: the list above is by address
        if (FAILED(g_rt0->GetDesc(&d)) || d.Width != kPassW || d.Height != kPassH) {
            return o_clear(dev, count, rects, flags, color, z, stencil);
        }
        if (g_pass_surface != g_rt0) {
            g_pass_surface = g_rt0;
            g_pass_texture = nullptr;
            void* container = nullptr;
            if (SUCCEEDED(g_rt0->GetContainer(__uuidof(IDirect3DTexture9), &container)) && container) {
                g_pass_texture = static_cast<IDirect3DTexture9*>(container);
                g_pass_texture->Release();  // identity only: the game holds it
            }
            g_bound_mask = 0;
            for (int s = 0; s < kStages; ++s) {
                if (g_bound[s] && g_bound[s] == g_pass_texture) g_bound_mask |= 1u << s;
            }
            g_unused_frames = 0;
        }
        g_in_pass = true;
        const LONG mode = g_mode;
        g_skipping = g_pass_texture && (mode == 2 || (mode == 1 && g_unused_frames >= kUnusedFramesToSkip));
        if (g_skipping) {
            if (!g_logged) {
                g_logged = true;
                LOG_INFO("SkipUnusedReflection: the small mirrored scene (256x180) is not drawn while nothing "
                         "reads it");
            }
            return D3D_OK;
        }
    }
    return o_clear(dev, count, rects, flags, color, z, stencil);
}

HRESULT WINAPI hk_set_tex(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
    if (stage < kStages) {
        g_bound[stage] = tex;
        const uint32_t bit = 1u << stage;
        if (tex && tex == g_pass_texture) {
            g_bound_mask |= bit;
        } else {
            g_bound_mask &= ~bit;
        }
        g_sub_mask &= ~bit;
    }
    return o_set_tex(dev, stage, tex);
}

HRESULT WINAPI hk_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    if (g_in_pass && g_skipping) return D3D_OK;
    before_draw(dev);
    return o_draw(dev, type, start, count);
}

HRESULT WINAPI hk_draw_idx(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT vertices,
                           UINT start, UINT count) {
    if (g_in_pass && g_skipping) return D3D_OK;
    before_draw(dev);
    return o_draw_idx(dev, type, base, min_index, vertices, start, count);
}

HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    if (g_in_pass && g_skipping) return D3D_OK;
    before_draw(dev);
    return o_draw_up(dev, type, count, data, stride);
}

HRESULT WINAPI hk_draw_idx_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count,
                              const void* indices, D3DFORMAT format, const void* data, UINT stride) {
    if (g_in_pass && g_skipping) return D3D_OK;
    before_draw(dev);
    return o_draw_idx_up(dev, type, min_index, vertices, count, indices, format, data, stride);
}

void* patch_slot(void* object, int slot, void* replacement) {
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

template <typename Fn>
void hook(IDirect3DDevice9* dev, int slot, void* ours, Fn& original) {
    void* p = patch_slot(dev, slot, ours);
    if (p && !original) original = reinterpret_cast<Fn>(p);
}

}  // namespace

void passcull_set_mode(int mode) {
    g_unused_frames = 0;  // from the remote thread: the next passes are drawn (and kept) before any is skipped
    InterlockedExchange(&g_mode, mode < 0 ? 0 : (mode > 2 ? 2 : mode));
}

int passcull_mode() { return static_cast<int>(g_mode); }

void passcull_hook_device(IDirect3DDevice9* dev) {
    if (!dev) return;
    hook(dev, 37, &hk_set_rt, o_set_rt);
    hook(dev, 43, &hk_clear, o_clear);
    hook(dev, 65, &hk_set_tex, o_set_tex);
    hook(dev, 81, &hk_draw, o_draw);
    hook(dev, 82, &hk_draw_idx, o_draw_idx);
    hook(dev, 83, &hk_draw_up, o_draw_up);
    hook(dev, 84, &hk_draw_idx_up, o_draw_idx_up);
    passcull_reset();
}

void passcull_reset() {
    if (g_copy) {
        g_copy->Release();
        g_copy = nullptr;
    }
    g_copy_valid = false;
    g_rt0 = nullptr;
    g_rt0_small = false;
    for (auto& s : g_small_seen) s = nullptr;
    for (auto& s : g_other_seen) s = nullptr;
    g_pass_surface = nullptr;
    g_pass_texture = nullptr;
    for (auto& t : g_bound) t = nullptr;
    g_bound_mask = g_sub_mask = 0;
    g_in_pass = g_skipping = g_window = g_window_skipped = g_used = false;
    g_unused_frames = 0;
}

IDirect3DTexture9* passcull_copy() { return g_copy_valid ? g_copy : nullptr; }

int passcull_unused_frames() { return g_unused_frames; }

void passcull_take_stats(long* skipped, long* drawn, long* stale) {
    *skipped = InterlockedExchange(&g_stat_skipped, 0);
    *drawn = InterlockedExchange(&g_stat_drawn, 0);
    *stale = InterlockedExchange(&g_stat_stale, 0);
}
