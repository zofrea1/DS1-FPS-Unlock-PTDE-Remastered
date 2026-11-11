#include "d3dperf.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Shared

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

double now_ms() {
    static const double scale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * scale;
}

// ---------------------------------------------------------------------------------------------------------------
// AsyncReadback

constexpr int kGetRtDataSlot = 32;  // IDirect3DDevice9::GetRenderTargetData
using GetRtDataFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
GetRtDataFn g_get_rt_data = nullptr;

constexpr int kRing = 3;              // the copy handed out was asked for two readbacks earlier
constexpr int kMaxSources = 8;
constexpr double kFreshMs = 100.0;    // older than this: the source is not read every frame
constexpr UINT kMaxBytes = 16u << 20;  // bigger copies keep the original path (three of them in a 32-bit process)

struct Source {
    IDirect3DSurface9* src = nullptr;  // identity only, not referenced
    UINT width = 0;
    UINT height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    IDirect3DSurface9* copies[kRing] = {};
    double requested[kRing] = {};  // when each copy was asked for; 0 = never
    int next = 0;
    double used = 0.0;
    bool broken = false;  // the copies could not be made: original path
};

SRWLOCK g_readback_lock = SRWLOCK_INIT;
Source g_sources[kMaxSources];
int g_sources_logged = 0;
volatile LONG g_async = 0;
volatile LONG g_waits = 0;

UINT bytes_per_pixel(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A32B32G32R32F:
        return 16;
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_G32R32F:
        return 8;
    case D3DFMT_R16F:
    case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_L16:
        return 2;
    case D3DFMT_A8:
    case D3DFMT_L8:
        return 1;
    default:
        return 4;
    }
}

void release_source(Source& s) {
    for (auto*& copy : s.copies) {
        if (copy) {
            copy->Release();
            copy = nullptr;
        }
    }
    s = Source{};
}

Source* find_source(IDirect3DDevice9* device, IDirect3DSurface9* src, const D3DSURFACE_DESC& dd, double now) {
    Source* lru = &g_sources[0];
    for (auto& s : g_sources) {
        if (s.src == src && s.width == dd.Width && s.height == dd.Height && s.format == dd.Format) {
            return &s;
        }
        if (s.used < lru->used) {
            lru = &s;
        }
    }
    release_source(*lru);
    Source& s = *lru;
    s.src = src;
    s.width = dd.Width;
    s.height = dd.Height;
    s.format = dd.Format;
    s.used = now;
    for (auto*& copy : s.copies) {
        if (FAILED(device->CreateOffscreenPlainSurface(dd.Width, dd.Height, dd.Format, D3DPOOL_SYSTEMMEM, &copy,
                                                      nullptr))) {
            copy = nullptr;
            s.broken = true;
        }
    }
    if (g_sources_logged < kMaxSources) {
        ++g_sources_logged;
        LOG_INFO("AsyncReadback: render target %p (%ux%u, format %u) read back by the game at %p%s", src, dd.Width,
                 dd.Height, static_cast<unsigned>(dd.Format), _ReturnAddress(),
                 s.broken ? "; could not make the copies, left as it was" : "");
    }
    return &s;
}

bool copy_surface(IDirect3DSurface9* from, IDirect3DSurface9* to, UINT height) {
    D3DLOCKED_RECT a{};
    HRESULT hr = from->LockRect(&a, nullptr, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
    if (hr == D3DERR_WASSTILLDRAWING) {
        InterlockedIncrement(&g_waits);
        hr = from->LockRect(&a, nullptr, D3DLOCK_READONLY);
    }
    if (FAILED(hr)) {
        return false;
    }
    D3DLOCKED_RECT b{};
    if (FAILED(to->LockRect(&b, nullptr, 0))) {
        from->UnlockRect();
        return false;
    }
    const UINT row = static_cast<UINT>(a.Pitch < b.Pitch ? a.Pitch : b.Pitch);
    const auto* src = static_cast<const uint8_t*>(a.pBits);
    auto* dst = static_cast<uint8_t*>(b.pBits);
    if (a.Pitch == b.Pitch) {
        std::memcpy(dst, src, static_cast<size_t>(row) * height);
    } else {
        for (UINT y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * b.Pitch, src + static_cast<size_t>(y) * a.Pitch, row);
        }
    }
    to->UnlockRect();
    from->UnlockRect();
    return true;
}

HRESULT WINAPI hook_get_rt_data(IDirect3DDevice9* device, IDirect3DSurface9* src, IDirect3DSurface9* dst) {
    if (!src || !dst) {
        return g_get_rt_data(device, src, dst);
    }
    D3DSURFACE_DESC dd{};
    if (FAILED(dst->GetDesc(&dd)) || dd.Pool != D3DPOOL_SYSTEMMEM ||
        static_cast<unsigned long long>(dd.Width) * dd.Height * bytes_per_pixel(dd.Format) > kMaxBytes) {
        return g_get_rt_data(device, src, dst);
    }
    AcquireSRWLockExclusive(&g_readback_lock);
    const double now = now_ms();
    Source* s = find_source(device, src, dd, now);
    HRESULT result = E_FAIL;
    bool done = false;
    if (!s->broken) {
        s->used = now;
        const int slot = s->next;
        if (SUCCEEDED(g_get_rt_data(device, src, s->copies[slot]))) {
            s->requested[slot] = now;
            s->next = (slot + 1) % kRing;
            const int oldest = s->next;
            if (s->requested[oldest] > 0.0 && now - s->requested[oldest] <= kFreshMs * (kRing - 1) &&
                copy_surface(s->copies[oldest], dst, dd.Height)) {
                InterlockedIncrement(&g_async);
                result = D3D_OK;
                done = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_readback_lock);
    // Warming up, a source read only now and then, or a failure: the original, exact copy.
    return done ? result : g_get_rt_data(device, src, dst);
}

// ---------------------------------------------------------------------------------------------------------------
// FilterSamplerStates

constexpr int kSetSamplerSlot = 69;    // IDirect3DDevice9::SetSamplerState
constexpr int kBeginBlockSlot = 60;    // IDirect3DDevice9::BeginStateBlock
constexpr int kEndBlockSlot = 61;      // IDirect3DDevice9::EndStateBlock
constexpr int kBlockApplySlot = 5;     // IDirect3DStateBlock9::Apply

using SetSamplerFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using BeginBlockFn = HRESULT(WINAPI*)(IDirect3DDevice9*);
using EndBlockFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DStateBlock9**);
using ApplyFn = HRESULT(WINAPI*)(IDirect3DStateBlock9*);
SetSamplerFn g_set_sampler = nullptr;
BeginBlockFn g_begin_block = nullptr;
EndBlockFn g_end_block = nullptr;
ApplyFn g_apply = nullptr;

// 16 pixel samplers, the displacement map sampler (256) and four vertex samplers (257..260); types 1..13.
constexpr int kSamplers = 21;
constexpr int kTypes = 14;
DWORD g_value[kSamplers][kTypes];
uint32_t g_known[kSamplers];  // a bit per type: g_value holds what the device has
volatile bool g_recording = false;
// The thread whose sampler states the cache follows: the last one to set any. The title screen's are set by another
// thread than the game's render thread; a cache kept for the first thread only filtered nothing once in the game.
volatile DWORD g_owner = 0;
volatile LONG g_filtered = 0;
volatile LONG g_switches = 0;  // the setting thread changed (the cache starts over)
volatile LONG g_applies = 0;   // state blocks applied (the cache starts over)
volatile LONG g_recorded = 0;  // calls recorded into a state block (passed through)
int g_switches_logged = 0;

void forget_samplers() {
    for (auto& bits : g_known) {
        bits = 0;
    }
}

int sampler_index(DWORD sampler) {
    if (sampler < 16) {
        return static_cast<int>(sampler);
    }
    if (sampler >= D3DDMAPSAMPLER && sampler <= D3DVERTEXTEXTURESAMPLER3) {
        return 16 + static_cast<int>(sampler - D3DDMAPSAMPLER);
    }
    return -1;
}

HRESULT WINAPI hook_set_sampler(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    const int index = sampler_index(sampler);
    const DWORD self = GetCurrentThreadId();
    if (self != g_owner) {
        if (g_owner != 0 && g_switches_logged < 4) {
            ++g_switches_logged;
            LOG_INFO("FilterSamplerStates: sampler states now come from thread %lu (were thread %lu)", self, g_owner);
        }
        g_owner = self;
        forget_samplers();
        InterlockedIncrement(&g_switches);
    }
    if (g_recording) {
        InterlockedIncrement(&g_recorded);
        return g_set_sampler(device, sampler, type, value);
    }
    if (index < 0 || type <= 0 || type >= kTypes) {
        return g_set_sampler(device, sampler, type, value);
    }
    const uint32_t bit = 1u << type;
    if ((g_known[index] & bit) && g_value[index][type] == value) {
        InterlockedIncrement(&g_filtered);
        return D3D_OK;
    }
    const HRESULT hr = g_set_sampler(device, sampler, type, value);
    if (SUCCEEDED(hr)) {
        g_value[index][type] = value;
        g_known[index] |= bit;
    } else {
        g_known[index] &= ~bit;
    }
    return hr;
}

HRESULT WINAPI hook_apply(IDirect3DStateBlock9* block) {
    const HRESULT hr = g_apply(block);
    forget_samplers();
    InterlockedIncrement(&g_applies);
    return hr;
}

void hook_block_class(IDirect3DStateBlock9* block) {
    if (!block) {
        return;
    }
    void* original = patch_slot(block, kBlockApplySlot, reinterpret_cast<void*>(&hook_apply));
    if (original && !g_apply) {
        g_apply = reinterpret_cast<ApplyFn>(original);
    }
}

HRESULT WINAPI hook_begin_block(IDirect3DDevice9* device) {
    const HRESULT hr = g_begin_block(device);
    if (SUCCEEDED(hr)) {
        g_recording = true;
    }
    return hr;
}

HRESULT WINAPI hook_end_block(IDirect3DDevice9* device, IDirect3DStateBlock9** out) {
    const HRESULT hr = g_end_block(device, out);
    g_recording = false;
    if (SUCCEEDED(hr) && out && *out && !g_apply) {
        hook_block_class(*out);
    }
    return hr;
}

// The device that does the work: a resource's GetDevice answers with it even when DSfix (or another wrapper)
// hands the game its own device object.
IDirect3DDevice9* real_device(IDirect3DDevice9* game_device) {
    IDirect3DVertexBuffer9* vb = nullptr;
    if (FAILED(game_device->CreateVertexBuffer(16, D3DUSAGE_WRITEONLY, 0, D3DPOOL_SYSTEMMEM, &vb, nullptr)) || !vb) {
        return nullptr;
    }
    IDirect3DDevice9* real = nullptr;
    if (FAILED(vb->GetDevice(&real))) {
        real = nullptr;
    }
    vb->Release();
    return real;
}

bool install_filter(IDirect3DDevice9* game_device) {
    IDirect3DDevice9* real = real_device(game_device);
    if (!real) {
        LOG_ERROR("FilterSamplerStates: could not reach the Direct3D device; sampler states are not filtered");
        return false;
    }
    IDirect3DStateBlock9* block = nullptr;
    if (FAILED(real->CreateStateBlock(D3DSBT_PIXELSTATE, &block)) || !block) {
        LOG_ERROR("FilterSamplerStates: could not make a state block; sampler states are not filtered");
        real->Release();
        return false;
    }
    hook_block_class(block);
    block->Release();
    if (!g_apply) {
        LOG_ERROR("FilterSamplerStates: could not hook state blocks; sampler states are not filtered");
        real->Release();
        return false;
    }
    forget_samplers();
    void* begin = patch_slot(real, kBeginBlockSlot, reinterpret_cast<void*>(&hook_begin_block));
    void* end = patch_slot(real, kEndBlockSlot, reinterpret_cast<void*>(&hook_end_block));
    if (begin && !g_begin_block) g_begin_block = reinterpret_cast<BeginBlockFn>(begin);
    if (end && !g_end_block) g_end_block = reinterpret_cast<EndBlockFn>(end);
    if (!g_begin_block || !g_end_block) {
        LOG_ERROR("FilterSamplerStates: could not hook state block recording; sampler states are not filtered");
        real->Release();
        return false;
    }
    void* set = patch_slot(real, kSetSamplerSlot, reinterpret_cast<void*>(&hook_set_sampler));
    if (set && !g_set_sampler) g_set_sampler = reinterpret_cast<SetSamplerFn>(set);
    LOG_INFO("FilterSamplerStates: sampler states that repeat the value in place skip Direct3D (device %p%s)", real,
             real == game_device ? "" : ", under the wrapper the game holds");
    real->Release();
    return g_set_sampler != nullptr;
}

}  // namespace

void d3dperf_hook_device(IDirect3DDevice9* game_device, bool readback, bool filter) {
    if (!game_device) {
        return;
    }
    if (readback) {
        void* original = patch_slot(game_device, kGetRtDataSlot, reinterpret_cast<void*>(&hook_get_rt_data));
        if (original && !g_get_rt_data) {
            g_get_rt_data = reinterpret_cast<GetRtDataFn>(original);
        }
        if (g_get_rt_data) {
            LOG_INFO("AsyncReadback: render targets the game reads back every frame are copied two frames late "
                     "instead of waiting for the GPU");
        }
    }
    if (filter && !g_set_sampler) {
        install_filter(game_device);
    }
}

void d3dperf_reset() {
    AcquireSRWLockExclusive(&g_readback_lock);
    for (auto& s : g_sources) {
        release_source(s);
    }
    ReleaseSRWLockExclusive(&g_readback_lock);
    forget_samplers();
}

unsigned long d3dperf_filtered_samplers() {
    return static_cast<unsigned long>(g_filtered);
}

unsigned long d3dperf_readback_async() {
    return static_cast<unsigned long>(g_async);
}

unsigned long d3dperf_readback_waits() {
    return static_cast<unsigned long>(g_waits);
}

void d3dperf_filter_stats(unsigned long* switches, unsigned long* applies, unsigned long* recorded) {
    *switches = static_cast<unsigned long>(g_switches);
    *applies = static_cast<unsigned long>(g_applies);
    *recorded = static_cast<unsigned long>(g_recorded);
}
