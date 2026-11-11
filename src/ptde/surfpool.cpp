#include "surfpool.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>

#include <cstdint>

namespace {

constexpr int kCreateDepthSlot = 29;      // IDirect3DDevice9::CreateDepthStencilSurface
constexpr int kCreateOffscreenSlot = 36;  // IDirect3DDevice9::CreateOffscreenPlainSurface
constexpr int kReleaseSlot = 2;           // IUnknown::Release

using CreateDepthFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                       IDirect3DSurface9**, HANDLE*);
using CreateOffscreenFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**,
                                           HANDLE*);
using ReleaseFn = ULONG(WINAPI*)(IUnknown*);
CreateDepthFn g_create_depth = nullptr;
CreateOffscreenFn g_create_offscreen = nullptr;

// Release per surface class (a class's method table is shared by all its objects).
constexpr int kMaxClasses = 4;
void** g_class_tables[kMaxClasses] = {};
ReleaseFn g_class_release[kMaxClasses] = {};
int g_classes = 0;

struct Entry {
    IDirect3DSurface9* surface;  // with one reference of ours
    UINT width, height;
    D3DFORMAT format;
    DWORD multisample;  // depth: D3DMULTISAMPLE_TYPE; offscreen: the D3DPOOL
    DWORD quality;
    BOOL discard;
    int kind;  // 0 depth-stencil, 1 offscreen
    bool in_use;
};
constexpr int kMaxEntries = 16;
Entry g_entries[kMaxEntries];
IUnknown* volatile g_pooled[kMaxEntries];  // the surfaces, for Release's quick check without the lock
int g_count = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_reused = 0, g_created = 0;

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

ReleaseFn release_for(IUnknown* object) {
    void** table = *reinterpret_cast<void***>(object);
    for (int i = 0; i < g_classes; ++i) {
        if (g_class_tables[i] == table) return g_class_release[i];
    }
    return nullptr;
}

ULONG WINAPI hook_release(IUnknown* object) {
    const ReleaseFn original = release_for(object);
    bool pooled = false;
    for (int i = 0; i < kMaxEntries && !pooled; ++i) pooled = g_pooled[i] == object;
    if (!pooled) return original(object);
    AcquireSRWLockExclusive(&g_lock);
    const ULONG left = original(object);
    if (left == 1) {
        // Only our reference is left: the game is done with it.
        for (int i = 0; i < g_count; ++i) {
            if (g_entries[i].surface == object) g_entries[i].in_use = false;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return left > 0 ? left - 1 : 0;  // the game's view, without our reference
}

// Hooks Release in the surface's class table (once per class).
void hook_class(IDirect3DSurface9* surface) {
    void** table = *reinterpret_cast<void***>(surface);
    for (int i = 0; i < g_classes; ++i) {
        if (g_class_tables[i] == table) return;
    }
    if (g_classes >= kMaxClasses) return;
    // The original first: other threads release surfaces of this class while the slot changes.
    g_class_tables[g_classes] = table;
    g_class_release[g_classes] = reinterpret_cast<ReleaseFn>(table[kReleaseSlot]);
    MemoryBarrier();
    ++g_classes;
    patch_slot(surface, kReleaseSlot, reinterpret_cast<void*>(&hook_release));
}

IDirect3DSurface9* take(int kind, UINT w, UINT h, D3DFORMAT f, DWORD ms, DWORD q, BOOL discard) {
    for (int i = 0; i < g_count; ++i) {
        Entry& e = g_entries[i];
        if (!e.in_use && e.kind == kind && e.width == w && e.height == h && e.format == f && e.multisample == ms &&
            e.quality == q && e.discard == discard) {
            e.in_use = true;
            e.surface->AddRef();
            return e.surface;
        }
    }
    return nullptr;
}

void keep(IDirect3DSurface9* s, int kind, UINT w, UINT h, D3DFORMAT f, DWORD ms, DWORD q, BOOL discard) {
    if (g_count >= kMaxEntries) return;
    hook_class(s);
    if (!release_for(s)) return;  // its class could not be hooked: not pooled
    s->AddRef();
    g_entries[g_count] = Entry{s, w, h, f, ms, q, discard, kind, true};
    g_pooled[g_count] = s;
    ++g_count;
}

HRESULT WINAPI hook_create_depth(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                 DWORD q, BOOL discard, IDirect3DSurface9** out, HANDLE* shared) {
    if (!out || shared) return g_create_depth(device, w, h, f, ms, q, discard, out, shared);
    AcquireSRWLockExclusive(&g_lock);
    IDirect3DSurface9* s = take(0, w, h, f, ms, q, discard);
    ReleaseSRWLockExclusive(&g_lock);
    if (s) {
        *out = s;
        InterlockedIncrement(&g_reused);
        return D3D_OK;
    }
    const HRESULT hr = g_create_depth(device, w, h, f, ms, q, discard, out, shared);
    InterlockedIncrement(&g_created);
    if (SUCCEEDED(hr) && *out) {
        AcquireSRWLockExclusive(&g_lock);
        keep(*out, 0, w, h, f, ms, q, discard);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return hr;
}

HRESULT WINAPI hook_create_offscreen(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT f, D3DPOOL pool,
                                     IDirect3DSurface9** out, HANDLE* shared) {
    if (!out || shared) return g_create_offscreen(device, w, h, f, pool, out, shared);
    AcquireSRWLockExclusive(&g_lock);
    IDirect3DSurface9* s = take(1, w, h, f, pool, 0, FALSE);
    ReleaseSRWLockExclusive(&g_lock);
    if (s) {
        *out = s;
        InterlockedIncrement(&g_reused);
        return D3D_OK;
    }
    const HRESULT hr = g_create_offscreen(device, w, h, f, pool, out, shared);
    InterlockedIncrement(&g_created);
    if (SUCCEEDED(hr) && *out) {
        AcquireSRWLockExclusive(&g_lock);
        keep(*out, 1, w, h, f, pool, 0, FALSE);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return hr;
}

}  // namespace

void surfpool_hook_device(IDirect3DDevice9* game_device) {
    if (!game_device || g_create_depth) return;
    void* depth = patch_slot(game_device, kCreateDepthSlot, reinterpret_cast<void*>(&hook_create_depth));
    void* offscreen = patch_slot(game_device, kCreateOffscreenSlot, reinterpret_cast<void*>(&hook_create_offscreen));
    if (depth) g_create_depth = reinterpret_cast<CreateDepthFn>(depth);
    if (offscreen) g_create_offscreen = reinterpret_cast<CreateOffscreenFn>(offscreen);
    if (g_create_depth && g_create_offscreen) {
        LOG_INFO("ReuseSurfaces: depth-stencil and offscreen surfaces the game creates and releases every frame are "
                 "kept and handed out again");
    }
}

void surfpool_reset() {
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_count; ++i) {
        IUnknown* s = g_entries[i].surface;
        g_pooled[i] = nullptr;
        // Our reference goes; a surface the game still holds lives on until the game releases it.
        if (const ReleaseFn original = release_for(s)) original(s);
    }
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void surfpool_stats(unsigned long* reused, unsigned long* created) {
    *reused = static_cast<unsigned long>(g_reused);
    *created = static_cast<unsigned long>(g_created);
}
