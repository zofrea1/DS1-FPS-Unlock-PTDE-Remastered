#include "d3dperf.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>
#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
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
volatile LONG g_seen = 0;  // calls that reached the filter

// The device the filter sits on, its method table when hooked and the original functions there. In the game the
// device's table pointer was seen to change (the filter answered calls on the title screen and none once the game
// loaded): the table is checked every frame and a new one hooked when it still holds the original functions.
IDirect3DDevice9* g_filter_device = nullptr;
void** g_filter_table = nullptr;
int g_table_changes_logged = 0;

void describe(const void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module) &&
        module) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        std::snprintf(out, size, "%s+0x%X", name ? name + 1 : path,
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    } else {
        std::snprintf(out, size, "%p, not in a module", address);
    }
}

// FilterShaderConstants: what the device's vertex shader float constants hold (256 registers), as last set through the
// filter. Calls that repeat it are answered here, and a call that changes only some registers is cut to the changed
// run. d3d9 has two versions of the method: the class table's (it records into a state block while one is recorded)
// and a fast path the device puts in its own table outside recording; each hooked copy calls the version it replaced.
constexpr int kSetVsConstSlot = 94;
constexpr UINT kVsRegs = 256;
using SetVsConstFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
SetVsConstFn g_vsc_a = nullptr;  // the live table's (normally the fast path)
SetVsConstFn g_vsc_b = nullptr;  // the class table's, when it differs
bool g_vsc_enabled = false;
bool g_ss_enabled = false;  // FilterSamplerStates (the hook is also there when only the constants are filtered)
alignas(16) uint32_t g_vs[kVsRegs][4];
uint8_t g_vs_known[kVsRegs];
volatile LONG g_vsc_calls = 0, g_vsc_regs = 0, g_vsc_skipped = 0, g_vsc_sent = 0;

void forget_samplers() {
    for (auto& bits : g_known) {
        bits = 0;
    }
    std::memset(g_vs_known, 0, sizeof(g_vs_known));
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
    g_seen = g_seen + 1;
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
    if (!g_ss_enabled) {
        return g_set_sampler(device, sampler, type, value);
    }
    if (index < 0 || type <= 0 || type >= kTypes) {
        return g_set_sampler(device, sampler, type, value);
    }
    const uint32_t bit = 1u << type;
    if ((g_known[index] & bit) && g_value[index][type] == value) {
        g_filtered = g_filtered + 1;
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

HRESULT set_vsc(SetVsConstFn original, IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    g_vsc_calls = g_vsc_calls + 1;
    g_vsc_regs = g_vsc_regs + static_cast<LONG>(count);
    const DWORD self = GetCurrentThreadId();
    if (self != g_owner) {
        g_owner = self;
        forget_samplers();
        InterlockedIncrement(&g_switches);
    }
    if (g_recording || !data || count == 0 || start >= kVsRegs || count > kVsRegs - start) {
        if (!g_recording) {
            // Out of the range the cache covers (or nothing to set): forget what it could change, pass it on.
            for (UINT r = start; r < kVsRegs && r - start < count; ++r) g_vs_known[r] = 0;
        }
        return original(device, start, data, count);
    }
    const uint32_t* src = reinterpret_cast<const uint32_t*>(data);
    UINT first = count, last = 0;
    for (UINT i = 0; i < count; ++i) {
        const UINT r = start + i;
        if (!g_vs_known[r] || std::memcmp(g_vs[r], src + i * 4, 16) != 0) {
            if (first == count) first = i;
            last = i;
        }
    }
    if (first == count) {
        g_vsc_skipped = g_vsc_skipped + 1;
        return D3D_OK;
    }
    const UINT n = last - first + 1;
    const HRESULT hr = original(device, start + first, data + first * 4, n);
    g_vsc_sent = g_vsc_sent + static_cast<LONG>(n);
    if (SUCCEEDED(hr)) {
        std::memcpy(g_vs[start + first], src + first * 4, n * 16);
        std::memset(&g_vs_known[start + first], 1, n);
    } else {
        std::memset(&g_vs_known[start + first], 0, n);
    }
    return hr;
}

HRESULT WINAPI hook_vsc_a(IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    return set_vsc(g_vsc_a, device, start, data, count);
}

HRESULT WINAPI hook_vsc_b(IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    return set_vsc(g_vsc_b, device, start, data, count);
}

// The hook for a copy of the method that holds `original`, or null when it is neither known version (a second
// unknown version is left alone).
void* vsc_hook_for(void* original) {
    if (!g_vsc_enabled || !original) return nullptr;
    if (original == reinterpret_cast<void*>(g_vsc_a)) return reinterpret_cast<void*>(&hook_vsc_a);
    if (!g_vsc_b) g_vsc_b = reinterpret_cast<SetVsConstFn>(original);
    if (original == reinterpret_cast<void*>(g_vsc_b)) return reinterpret_cast<void*>(&hook_vsc_b);
    return nullptr;
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

bool write_pointer(void** where, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(where, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    *where = value;
    DWORD ignored = 0;
    VirtualProtect(where, sizeof(void*), old, &ignored);
    return true;
}

// Windows' d3d9 gives each device its own copy of the method table, inside the device object, and rewrites it:
// BeginStateBlock copies the class's table over it (from a pointer the device keeps) and EndStateBlock puts back
// per-device "fast path" methods it also keeps in the device object, SetSamplerState among them. A hook in the copy
// alone was gone after the first state block was recorded (the game's DSfix records some when the game loads). So
// the class table the device points to and every other copy of the original SetSamplerState in the device object
// are hooked too; the offsets are found by searching the object, not assumed. Returns how many were hooked.
int hook_table_sources(IDirect3DDevice9* device, void** live) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(device, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        return 0;
    }
    auto* p = reinterpret_cast<void**>(device);
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t words = std::min<size_t>(0x6000 / sizeof(void*),
                                          (region_end - reinterpret_cast<uintptr_t>(device)) / sizeof(void*));
    void* const set = reinterpret_cast<void*>(g_set_sampler);
    void* const begin = reinterpret_cast<void*>(g_begin_block);
    void* const end = reinterpret_cast<void*>(g_end_block);
    int hooked = 0;
    for (size_t i = 0; i < words; ++i) {
        void** slot = p + i;
        if (slot >= live && slot < live + 119) {
            continue;  // the live table, hooked already
        }
        if (*slot == set) {
            // A per-device copy of the method (the fast path EndStateBlock puts back).
            if (write_pointer(slot, reinterpret_cast<void*>(&hook_set_sampler))) {
                ++hooked;
                LOG_INFO("FilterSamplerStates: device+0x%X holds SetSamplerState too: hooked",
                         static_cast<unsigned>(i * sizeof(void*)));
            }
            continue;
        }
        if (g_vsc_enabled && g_vsc_a && *slot == reinterpret_cast<void*>(g_vsc_a)) {
            // The fast path copy of SetVertexShaderConstantF.
            if (write_pointer(slot, reinterpret_cast<void*>(&hook_vsc_a))) {
                ++hooked;
                LOG_INFO("FilterShaderConstants: device+0x%X holds SetVertexShaderConstantF too: hooked",
                         static_cast<unsigned>(i * sizeof(void*)));
            }
            continue;
        }
        // A pointer to a whole table with the original methods: the class table BeginStateBlock copies.
        void** t = reinterpret_cast<void**>(*slot);
        if (!t || t == live) continue;
        MEMORY_BASIC_INFORMATION tm{};
        if (!VirtualQuery(t, &tm, sizeof(tm)) || tm.State != MEM_COMMIT ||
            reinterpret_cast<uintptr_t>(tm.BaseAddress) + tm.RegionSize < reinterpret_cast<uintptr_t>(t + 119) ||
            (tm.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            continue;
        }
        if (t[kSetSamplerSlot] == set && t[kBeginBlockSlot] == begin && t[kEndBlockSlot] == end) {
            const bool ok = write_pointer(&t[kBeginBlockSlot], reinterpret_cast<void*>(&hook_begin_block)) &&
                            write_pointer(&t[kEndBlockSlot], reinterpret_cast<void*>(&hook_end_block)) &&
                            write_pointer(&t[kSetSamplerSlot], reinterpret_cast<void*>(&hook_set_sampler));
            if (ok) {
                ++hooked;
                char where[160];
                describe(t, where, sizeof(where));
                LOG_INFO("FilterSamplerStates: device+0x%X points to a method table (%s) the device copies from: "
                         "hooked",
                         static_cast<unsigned>(i * sizeof(void*)), where);
                // Its SetVertexShaderConstantF: the version that records while a state block is recorded.
                if (void* vh = vsc_hook_for(t[kSetVsConstSlot])) {
                    char fn[160];
                    describe(t[kSetVsConstSlot], fn, sizeof(fn));
                    if (write_pointer(&t[kSetVsConstSlot], vh)) {
                        LOG_INFO("FilterShaderConstants: SetVertexShaderConstantF in that table (%s) hooked", fn);
                    }
                }
            }
        }
    }
    return hooked;
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
    if (g_vsc_enabled) {
        void* vsc = patch_slot(real, kSetVsConstSlot, reinterpret_cast<void*>(&hook_vsc_a));
        if (vsc && !g_vsc_a) g_vsc_a = reinterpret_cast<SetVsConstFn>(vsc);
        if (g_vsc_a) {
            char fn[160];
            describe(reinterpret_cast<const void*>(g_vsc_a), fn, sizeof(fn));
            LOG_INFO("FilterShaderConstants: vertex shader constants that repeat what the device holds skip Direct3D "
                     "(SetVertexShaderConstantF %s)",
                     fn);
        } else {
            g_vsc_enabled = false;
        }
    }
    g_filter_device = real;
    g_filter_table = *reinterpret_cast<void***>(real);
    const int sources = hook_table_sources(real, g_filter_table);
    char table[160], fn[160];
    describe(g_filter_table, table, sizeof(table));
    describe(reinterpret_cast<const void*>(g_set_sampler), fn, sizeof(fn));
    LOG_INFO("FilterSamplerStates: sampler states that repeat the value in place skip Direct3D (device %p%s; method "
             "table %s, SetSamplerState %s; %d of the device's own copies of these methods hooked as well)",
             real, real == game_device ? ", under the wrapper the game holds" : "", table, fn, sources);
    real->Release();
    return g_set_sampler != nullptr;
}

}  // namespace

void d3dperf_hook_device(IDirect3DDevice9* game_device, bool readback, bool filter, bool constants) {
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
    if ((filter || constants) && !g_set_sampler) {
        g_vsc_enabled = constants;
        g_ss_enabled = filter;
        install_filter(game_device);
    }
}

void d3dperf_frame() {
    if (!g_filter_device || !g_set_sampler) {
        return;
    }
    void** table = *reinterpret_cast<void***>(g_filter_device);
    if (table == g_filter_table && table[kSetSamplerSlot] == reinterpret_cast<void*>(&hook_set_sampler)) {
        return;
    }
    forget_samplers();
    const bool pristine = table[kSetSamplerSlot] == reinterpret_cast<void*>(g_set_sampler) &&
                          (table[kBeginBlockSlot] == reinterpret_cast<void*>(g_begin_block) ||
                           table[kBeginBlockSlot] == reinterpret_cast<void*>(&hook_begin_block)) &&
                          (table[kEndBlockSlot] == reinterpret_cast<void*>(g_end_block) ||
                           table[kEndBlockSlot] == reinterpret_cast<void*>(&hook_end_block));
    if (g_table_changes_logged < 8) {
        ++g_table_changes_logged;
        char was[160], now[160], fn[160];
        describe(g_filter_table, was, sizeof(was));
        describe(table, now, sizeof(now));
        describe(table[kSetSamplerSlot], fn, sizeof(fn));
        LOG_INFO("FilterSamplerStates: the device's method table is now %s (was %s); its SetSamplerState is %s: %s",
                 now, was, fn,
                 pristine ? "hooked there too" : "another hook's, left alone (sampler states are not filtered)");
    }
    g_filter_table = table;
    if (pristine) {
        patch_slot(g_filter_device, kBeginBlockSlot, reinterpret_cast<void*>(&hook_begin_block));
        patch_slot(g_filter_device, kEndBlockSlot, reinterpret_cast<void*>(&hook_end_block));
        patch_slot(g_filter_device, kSetSamplerSlot, reinterpret_cast<void*>(&hook_set_sampler));
    }
    // The constants' slot, when the table holds a known original version again.
    if (g_vsc_enabled) {
        void** live = *reinterpret_cast<void***>(g_filter_device);
        void* current = live[kSetVsConstSlot];
        if (current != reinterpret_cast<void*>(&hook_vsc_a) && current != reinterpret_cast<void*>(&hook_vsc_b)) {
            if (void* vh = vsc_hook_for(current)) patch_slot(g_filter_device, kSetVsConstSlot, vh);
        }
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

unsigned long d3dperf_seen_samplers() {
    return static_cast<unsigned long>(g_seen);
}

void d3dperf_filter_stats(unsigned long* switches, unsigned long* applies, unsigned long* recorded) {
    *switches = static_cast<unsigned long>(g_switches);
    *applies = static_cast<unsigned long>(g_applies);
    *recorded = static_cast<unsigned long>(g_recorded);
}

void d3dperf_constant_stats(unsigned long* calls, unsigned long* regs, unsigned long* skipped, unsigned long* sent) {
    *calls = static_cast<unsigned long>(g_vsc_calls);
    *regs = static_cast<unsigned long>(g_vsc_regs);
    *skipped = static_cast<unsigned long>(g_vsc_skipped);
    *sent = static_cast<unsigned long>(g_vsc_sent);
}
