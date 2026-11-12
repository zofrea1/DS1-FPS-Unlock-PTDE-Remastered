#include "profile.h"
#include "remote.h"
#include "streamwatch.h"
#include "texpreload.h"
#include "dxgipresent.h"

#include "d3dperf.h"
#include "surfpool.h"
#include "fpsu_buildcfg.h"

#if FPSU_DIAGNOSTICS

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "user32.lib")

volatile DWORD g_render_thread_id = 0;
volatile DWORD g_game_thread_id = 0;

namespace {

constexpr int kCaptureSeconds = 10;
constexpr int kReportSeconds = 5;
constexpr int kMaxPicked = 12;      // threads sampled during a capture
constexpr int kMaxRets = 20;        // game return addresses kept per sample
constexpr int kStackDwords = 2048;  // how far above ESP the stack is searched for them (8 KB)
constexpr int kMaxSamples = 125000; // 10 s at 1 kHz for up to 12 threads

bool g_enabled = false;

// The effects library (FFX), linked as a debug build: 0x960000..0xBA6000 less the non-debug code between.
bool in_ffx(uint32_t a) {
    const uint32_t b = a >> 16;
    if (b >= 0x96 && b <= 0x9E) return true;
    if (b == 0xA0 || (b >= 0xA3 && b <= 0xA8)) return true;
    if (b >= 0xB7 && b <= 0xB9) return true;
    return a >= 0xBA0000 && a < 0xBA6000;
}
uint32_t g_text_lo = 0, g_text_hi = 0;  // the game's code (.text), fixed base 0x400000

// ---------------------------------------------------------------------------------------------------------------
// Direct3D 9 census. Each device method the game calls goes through a 12-byte thunk that counts it, except a few
// with their own hook that also counts primitives, registers or time.
// ---------------------------------------------------------------------------------------------------------------
constexpr int kDeviceSlots = 119;  // IDirect3DDevice9: QueryInterface (0) .. CreateQuery (118)
enum Slot : int {
    kPresent = 17, kCreateTexture = 23, kGetRtData = 32, kStretchRect = 34, kSetRenderTarget = 37, kSetDepthStencil = 39, kClear = 43,
    kSetTransform = 44, kSetViewport = 47, kSetRenderState = 57, kGetTexture = 64, kSetTexture = 65,
    kSetSamplerState = 69, kSetScissor = 75, kDrawPrimitive = 81, kDrawIndexed = 82, kDrawUp = 83,
    kDrawIndexedUp = 84, kSetVertexDecl = 87, kSetFvf = 89, kSetVertexShader = 92, kSetVsConstF = 94,
    kSetVsConstI = 96, kSetVsConstB = 98, kSetStreamSource = 100, kSetIndices = 104, kSetPixelShader = 107,
    kSetPsConstF = 109, kSetPsConstI = 111, kSetPsConstB = 113, kCreateQuery = 118,
};

volatile uint32_t g_calls[kDeviceSlots];
void* g_orig[kDeviceSlots];
uint8_t* g_thunks = nullptr;

struct Timed {
    volatile uint32_t calls = 0;
    volatile uint32_t extra = 0;     // primitives, registers, S_FALSE results
    volatile int64_t ticks = 0;      // QPC ticks inside the call
    volatile int64_t max_ticks = 0;
};
Timed g_present, g_rtdata, g_vsc, g_psc, g_dip, g_dp, g_lock_vb, g_lock_ib, g_lock_tex, g_lock_surf, g_getdata;
LARGE_INTEGER g_freq{};

inline int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline void add_time(Timed& t, int64_t ticks) {
    t.ticks += ticks;
    if (ticks > t.max_ticks) t.max_ticks = ticks;
}

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using GetRtDataFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
using ConstFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using DipFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using BufLockFn = HRESULT(WINAPI*)(void*, UINT, UINT, void**, DWORD);
using TexLockFn = HRESULT(WINAPI*)(void*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using SurfLockFn = HRESULT(WINAPI*)(void*, D3DLOCKED_RECT*, const RECT*, DWORD);
using GetDataFn = HRESULT(WINAPI*)(void*, void*, DWORD, DWORD);

HRESULT WINAPI hook_present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    ++g_calls[kPresent];
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<PresentFn>(g_orig[kPresent])(d, a, b, w, r);
    ++g_present.calls;
    add_time(g_present, qpc() - t0);
    return hr;
}
HRESULT WINAPI hook_get_rt_data(IDirect3DDevice9* d, IDirect3DSurface9* rt, IDirect3DSurface9* dst) {
    ++g_calls[kGetRtData];
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<GetRtDataFn>(g_orig[kGetRtData])(d, rt, dst);
    ++g_rtdata.calls;
    add_time(g_rtdata, qpc() - t0);
    return hr;
}
HRESULT WINAPI hook_vsc(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    ++g_calls[kSetVsConstF];
    g_vsc.extra += count;
    return reinterpret_cast<ConstFn>(g_orig[kSetVsConstF])(d, start, data, count);
}
HRESULT WINAPI hook_psc(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    ++g_calls[kSetPsConstF];
    g_psc.extra += count;
    return reinterpret_cast<ConstFn>(g_orig[kSetPsConstF])(d, start, data, count);
}
// Slow calls: any of these that blocks for 3 ms or more is logged with the shader pair bound and whether that pair
// has been drawn before in this session (the area-load hitches: the render thread waits inside the driver while it
// compiles; a warm-up can only help with pairs drawn for the first time).
void* volatile g_cur_vs = nullptr;
void* volatile g_cur_ps = nullptr;
void* volatile g_cur_decl = nullptr;
std::mutex g_pairs_lock;
std::unordered_set<uint64_t> g_pairs_drawn;
std::unordered_set<uint64_t> g_combos_drawn;  // pair + vertex declaration
volatile LONG g_draw_kind = 0;               // the last draw's: 0 pair before, 1 pair new, 2 pair before but combo new
void* volatile g_stage_tex[8] = {};
std::unordered_set<void*> g_tex_drawn;       // textures drawn with at least once
volatile LONG g_draw_new_tex = 0;            // the last draw's textures used for the first time
volatile LONG g_slow_logged = 0;
constexpr double kSlowCallMs = 3.0;
constexpr LONG kSlowCallLines = 400;

uint64_t pair_key() {
    return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_cur_vs)) << 32) |
           static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_cur_ps));
}

// Before a draw: whether its pair is new, or its pair with this vertex declaration (and remember both). 1 new pair,
// 2 known pair with a new declaration, 0 both seen.
bool pair_first_draw() {
    std::lock_guard<std::mutex> lock(g_pairs_lock);
    const uint64_t p = pair_key();
    const bool new_pair = g_pairs_drawn.insert(p).second;
    const uint64_t c = p * 1099511628211ULL ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_cur_decl));
    const bool new_combo = g_combos_drawn.insert(c).second;
    g_draw_kind = new_pair ? 1 : new_combo ? 2 : 0;
    LONG fresh = 0;
    for (int i = 0; i < 8; ++i) {
        void* t = g_stage_tex[i];
        if (t && g_tex_drawn.insert(t).second) ++fresh;
    }
    g_draw_new_tex = fresh;
    return new_pair;
}

void note_slow_call(const char* what, int64_t ticks, int first) {
    const double ms = 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq.QuadPart);
    if (ms < kSlowCallMs || InterlockedIncrement(&g_slow_logged) > kSlowCallLines) return;
    const LONG kind = g_draw_kind;
    LOG_INFO("[slowcall] %s %.1f ms on thread %lu: vs %s ps %s decl %p%s, %ld texture(s) used for the first time", what,
             ms, GetCurrentThreadId(), remote_shader_name(g_cur_vs).c_str(), remote_shader_name(g_cur_ps).c_str(),
             g_cur_decl, first < 0 ? "" : kind == 1 ? " (NEW pair)" : kind == 2 ? " (pair drawn before, NEW declaration)" :
                                          " (pair and declaration drawn before)", first < 0 ? 0L : static_cast<long>(g_draw_new_tex));
}

HRESULT WINAPI hook_dip(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT base, UINT min, UINT num, UINT start,
                        UINT prims) {
    ++g_calls[kDrawIndexed];
    g_dip.extra += prims;
    const bool first = pair_first_draw();
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<DipFn>(g_orig[kDrawIndexed])(d, type, base, min, num, start, prims);
    note_slow_call("DrawIndexedPrimitive", qpc() - t0, first ? 1 : 0);
    return hr;
}
HRESULT WINAPI hook_dp(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
    ++g_calls[kDrawPrimitive];
    g_dp.extra += prims;
    const bool first = pair_first_draw();
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<DpFn>(g_orig[kDrawPrimitive])(d, type, start, prims);
    note_slow_call("DrawPrimitive", qpc() - t0, first ? 1 : 0);
    return hr;
}
using DpUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DipUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                 const void*, UINT);
using SetShaderFn = HRESULT(WINAPI*)(IDirect3DDevice9*, void*);
using SetTextureSlowFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
HRESULT WINAPI hook_dpup(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, UINT prims, const void* data, UINT stride) {
    ++g_calls[kDrawUp];
    const bool first = pair_first_draw();
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<DpUpFn>(g_orig[kDrawUp])(d, type, prims, data, stride);
    note_slow_call("DrawPrimitiveUP", qpc() - t0, first ? 1 : 0);
    return hr;
}
HRESULT WINAPI hook_dipup(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, UINT min, UINT num, UINT prims,
                          const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
    ++g_calls[kDrawIndexedUp];
    const bool first = pair_first_draw();
    const int64_t t0 = qpc();
    const HRESULT hr =
        reinterpret_cast<DipUpFn>(g_orig[kDrawIndexedUp])(d, type, min, num, prims, idx, fmt, data, stride);
    note_slow_call("DrawIndexedPrimitiveUP", qpc() - t0, first ? 1 : 0);
    return hr;
}
HRESULT WINAPI hook_set_vs(IDirect3DDevice9* d, void* sh) {
    ++g_calls[kSetVertexShader];
    g_cur_vs = sh;
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<SetShaderFn>(g_orig[kSetVertexShader])(d, sh);
    note_slow_call("SetVertexShader", qpc() - t0, -1);
    return hr;
}
HRESULT WINAPI hook_set_ps(IDirect3DDevice9* d, void* sh) {
    ++g_calls[kSetPixelShader];
    g_cur_ps = sh;
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<SetShaderFn>(g_orig[kSetPixelShader])(d, sh);
    note_slow_call("SetPixelShader", qpc() - t0, -1);
    return hr;
}
HRESULT WINAPI hook_set_decl(IDirect3DDevice9* d, void* decl) {
    ++g_calls[kSetVertexDecl];
    g_cur_decl = decl;
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<SetShaderFn>(g_orig[kSetVertexDecl])(d, decl);
    note_slow_call("SetVertexDeclaration", qpc() - t0, -1);
    return hr;
}
// Texture creation by thread and pool, summed every 5 s while textures are being made.
using CreateTexFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**,
                                     HANDLE*);
std::mutex g_ctex_lock;
std::unordered_map<uint64_t, int> g_ctex;  // (thread << 8 | pool) -> count
int64_t g_ctex_next = 0;
HRESULT WINAPI hook_create_tex(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                               IDirect3DTexture9** out, HANDLE* shared) {
    ++g_calls[kCreateTexture];
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<CreateTexFn>(g_orig[kCreateTexture])(d, w, h, levels, usage, f, pool, out, shared);
    const int64_t t1 = qpc();
    char line[512] = "";
    {
        std::lock_guard<std::mutex> lock(g_ctex_lock);
        ++g_ctex[(static_cast<uint64_t>(GetCurrentThreadId()) << 8) | static_cast<uint64_t>(pool)];
        if (!g_ctex_next) g_ctex_next = t1 + g_freq.QuadPart * 5;
        if (t1 >= g_ctex_next) {
            int o = std::snprintf(line, sizeof(line), "[textures] made in the last 5 s, by thread and pool:");
            for (const auto& kv : g_ctex) {
                o += std::snprintf(line + o, sizeof(line) - o, " thread %lu pool %d: %d;",
                                   static_cast<unsigned long>(kv.first >> 8), static_cast<int>(kv.first & 0xFF), kv.second);
                if (o >= static_cast<int>(sizeof(line)) - 1) break;
            }
            g_ctex.clear();
            g_ctex_next = t1 + g_freq.QuadPart * 5;
        }
    }
    if (line[0]) LOG_INFO("%s", line);
    note_slow_call("CreateTexture", t1 - t0, -1);
    return hr;
}
HRESULT WINAPI hook_set_tex_slow(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t) {
    ++g_calls[kSetTexture];
    if (stage < 8) g_stage_tex[stage] = t;
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<SetTextureSlowFn>(g_orig[kSetTexture])(d, stage, t);
    note_slow_call("SetTexture", qpc() - t0, -1);
    return hr;
}

// Resource locks and query results: one hook per distinct class vtable (the runtime uses a few classes per kind,
// by pool and usage), found by creating a throwaway resource of each kind.
struct ResHook {
    void** vtable;
    void* orig;
};
constexpr int kMaxResHooks = 24;
ResHook g_res[kMaxResHooks];
int g_res_count = 0;

void* res_orig(void* object, int slot) {
    void** vt = *reinterpret_cast<void***>(object);
    for (int i = 0; i < g_res_count; ++i) {
        if (g_res[i].vtable == vt) return g_res[i].orig;
    }
    return vt[slot];  // not hooked through this vtable (cannot happen once installed)
}

HRESULT WINAPI hook_vb_lock(void* self, UINT off, UINT size, void** data, DWORD flags) {
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<BufLockFn>(res_orig(self, 11))(self, off, size, data, flags);
    ++g_lock_vb.calls;
    if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == 0) ++g_lock_vb.extra;
    add_time(g_lock_vb, qpc() - t0);
    return hr;
}
HRESULT WINAPI hook_ib_lock(void* self, UINT off, UINT size, void** data, DWORD flags) {
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<BufLockFn>(res_orig(self, 11))(self, off, size, data, flags);
    ++g_lock_ib.calls;
    if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == 0) ++g_lock_ib.extra;
    add_time(g_lock_ib, qpc() - t0);
    return hr;
}
HRESULT WINAPI hook_tex_lock(void* self, UINT level, D3DLOCKED_RECT* rect, const RECT* area, DWORD flags) {
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<TexLockFn>(res_orig(self, 19))(self, level, rect, area, flags);
    ++g_lock_tex.calls;
    add_time(g_lock_tex, qpc() - t0);
    return hr;
}
HRESULT WINAPI hook_surf_lock(void* self, D3DLOCKED_RECT* rect, const RECT* area, DWORD flags) {
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<SurfLockFn>(res_orig(self, 13))(self, rect, area, flags);
    ++g_lock_surf.calls;
    add_time(g_lock_surf, qpc() - t0);
    return hr;
}
HRESULT WINAPI hook_get_data(void* self, void* data, DWORD size, DWORD flags) {
    const int64_t t0 = qpc();
    const HRESULT hr = reinterpret_cast<GetDataFn>(res_orig(self, 7))(self, data, size, flags);
    ++g_getdata.calls;
    if (hr == S_FALSE) ++g_getdata.extra;
    add_time(g_getdata, qpc() - t0);
    return hr;
}

bool patch_vtable_slot(void** vt, int slot, void* replacement, void** original) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = vt[slot];
    vt[slot] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vt[slot], sizeof(void*), old, &ignored);
    return true;
}

void hook_resource(IUnknown* object, int slot, void* hook, const char* what) {
    if (!object) return;
    void** vt = *reinterpret_cast<void***>(object);
    bool seen = false;
    for (int i = 0; i < g_res_count; ++i) seen = seen || g_res[i].vtable == vt;
    if (!seen && g_res_count < kMaxResHooks && vt[slot] != hook) {
        void* orig = nullptr;
        if (patch_vtable_slot(vt, slot, hook, &orig)) {
            g_res[g_res_count].vtable = vt;
            g_res[g_res_count].orig = orig;
            ++g_res_count;
            LOG_INFO("[perf] %s hooked (class vtable %p)", what, static_cast<void*>(vt));
        }
    }
    object->Release();
}

void hook_resources(IDirect3DDevice9* dev) {
    using CreateVbFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**,
                                        HANDLE*);
    using CreateIbFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**,
                                        HANDLE*);
    using CreateTexFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DTexture9**, HANDLE*);
    using CreateQueryFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
    using CreateOffscreenFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL,
                                               IDirect3DSurface9**, HANDLE*);
    // Through the original slots, so the counts start clean.
    const auto create_vb = reinterpret_cast<CreateVbFn>(g_orig[26]);
    const auto create_ib = reinterpret_cast<CreateIbFn>(g_orig[27]);
    const auto create_tex = reinterpret_cast<CreateTexFn>(g_orig[23]);
    const auto create_query = reinterpret_cast<CreateQueryFn>(g_orig[kCreateQuery]);
    const auto create_off = reinterpret_cast<CreateOffscreenFn>(g_orig[36]);
    struct Pool {
        DWORD usage;
        D3DPOOL pool;
    } const pools[] = {{D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT},
                       {D3DUSAGE_WRITEONLY, D3DPOOL_DEFAULT},
                       {D3DUSAGE_WRITEONLY, D3DPOOL_MANAGED},
                       {0, D3DPOOL_SYSTEMMEM}};
    for (const Pool& p : pools) {
        IDirect3DVertexBuffer9* vb = nullptr;
        if (SUCCEEDED(create_vb(dev, 256, p.usage, 0, p.pool, &vb, nullptr)))
            hook_resource(vb, 11, reinterpret_cast<void*>(&hook_vb_lock), "Vertex buffer Lock");
        IDirect3DIndexBuffer9* ib = nullptr;
        if (SUCCEEDED(create_ib(dev, 256, p.usage, D3DFMT_INDEX16, p.pool, &ib, nullptr)))
            hook_resource(ib, 11, reinterpret_cast<void*>(&hook_ib_lock), "Index buffer Lock");
    }
    const Pool tex_pools[] = {{D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT}, {0, D3DPOOL_MANAGED}, {0, D3DPOOL_SYSTEMMEM}};
    for (const Pool& p : tex_pools) {
        IDirect3DTexture9* tex = nullptr;
        if (SUCCEEDED(create_tex(dev, 16, 16, 1, p.usage, D3DFMT_A8R8G8B8, p.pool, &tex, nullptr)))
            hook_resource(tex, 19, reinterpret_cast<void*>(&hook_tex_lock), "Texture LockRect");
    }
    IDirect3DSurface9* surf = nullptr;
    if (SUCCEEDED(create_off(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surf, nullptr)))
        hook_resource(surf, 13, reinterpret_cast<void*>(&hook_surf_lock), "Surface LockRect");
    const D3DQUERYTYPE kinds[] = {D3DQUERYTYPE_EVENT, D3DQUERYTYPE_OCCLUSION, D3DQUERYTYPE_TIMESTAMP};
    for (D3DQUERYTYPE k : kinds) {
        IDirect3DQuery9* q = nullptr;
        if (SUCCEEDED(create_query(dev, k, &q)))
            hook_resource(q, 7, reinterpret_cast<void*>(&hook_get_data), "Query GetData");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Frame timing accumulated between [perf] lines.
// ---------------------------------------------------------------------------------------------------------------
struct Accum {
    int count = 0;
    double sum = 0.0;
    double max = 0.0;
    void add(double v) {
        ++count;
        sum += v;
        if (v > max) max = v;
    }
};
Accum g_game_ms, g_render_ms, g_frame_ms;
int64_t g_last_boundary = 0;
volatile LONG g_frames = 0;

// ---------------------------------------------------------------------------------------------------------------
// Safe reads (VirtualQuery, so a bad pointer raises nothing for the crash handler to log).
// ---------------------------------------------------------------------------------------------------------------
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(p) + n <= end;
}
template <class T>
T rd(uintptr_t a, T fallback = T{}) {
    T v;
    if (!readable(reinterpret_cast<const void*>(a), sizeof(T))) return fallback;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}

// The player's position: [[[WorldChrMan] + 4]] is the player, +0x28 its controller, +0x1C its physics, +0x10 xyz.
bool player_position(float* xyz) {
    const uintptr_t wcm = rd<uintptr_t>(0x0137DC70);
    const uintptr_t first = wcm ? rd<uintptr_t>(wcm + 4) : 0;
    const uintptr_t chr = first ? rd<uintptr_t>(first) : 0;
    const uintptr_t ctrl = chr ? rd<uintptr_t>(chr + 0x28) : 0;
    const uintptr_t phys = ctrl ? rd<uintptr_t>(ctrl + 0x1C) : 0;
    if (!phys || !readable(reinterpret_cast<const void*>(phys + 0x10), 12)) return false;
    std::memcpy(xyz, reinterpret_cast<const void*>(phys + 0x10), 12);
    return true;
}

uint64_t thread_cycles(HANDLE h) {
    ULONG64 c = 0;
    return h && QueryThreadCycleTime(h, &c) ? c : 0;
}

// TSC cycles per second, measured against the performance counter.
double g_cycles_per_s = 0.0;
void calibrate_cycles() {
    const int64_t q0 = qpc();
    const uint64_t c0 = __rdtsc();
    Sleep(50);
    const int64_t q1 = qpc();
    const uint64_t c1 = __rdtsc();
    g_cycles_per_s = static_cast<double>(c1 - c0) / (static_cast<double>(q1 - q0) / g_freq.QuadPart);
}

// ---------------------------------------------------------------------------------------------------------------
// [perf] line.
// ---------------------------------------------------------------------------------------------------------------
uint32_t g_calls_prev[kDeviceSlots];
struct TimedSnap {
    uint32_t calls, extra;
    int64_t ticks;
};
TimedSnap snap(Timed& t) {
    TimedSnap s{t.calls, t.extra, t.ticks};
    return s;
}
TimedSnap g_prev_present, g_prev_rtdata, g_prev_vsc, g_prev_psc, g_prev_dip, g_prev_dp, g_prev_vb, g_prev_ib,
    g_prev_tex, g_prev_surf, g_prev_getdata;
HANDLE g_game_handle = nullptr, g_render_handle = nullptr;
uint64_t g_game_cycles_prev = 0, g_render_cycles_prev = 0;
int64_t g_report_prev = 0;
bool g_device_hooked = false;

double ms_of(int64_t ticks) { return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq.QuadPart); }

void report() {
    const int64_t now = qpc();
    double window = static_cast<double>(now - g_report_prev) / g_freq.QuadPart;
    // Right after a capture (g_report_prev 0): every counter is read and its baseline moved on, but nothing is
    // logged, so the next line does not count the capture's frames and CPU time.
    const bool quiet = g_report_prev == 0 || window <= 0.0;
    if (quiet) window = 1.0;
    g_report_prev = now;
    const LONG frames = InterlockedExchange(&g_frames, 0);
    const Accum game = g_game_ms, render = g_render_ms, frame = g_frame_ms;
    g_game_ms = Accum{};
    g_render_ms = Accum{};
    g_frame_ms = Accum{};

    if (!g_game_handle && g_game_thread_id)
        g_game_handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_game_thread_id);
    if (!g_render_handle && g_render_thread_id)
        g_render_handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_render_thread_id);
    const uint64_t gc = thread_cycles(g_game_handle), rc = thread_cycles(g_render_handle);
    const double game_cpu = g_game_cycles_prev && gc ? 100.0 * (gc - g_game_cycles_prev) / (g_cycles_per_s * window) : -1;
    const double render_cpu =
        g_render_cycles_prev && rc ? 100.0 * (rc - g_render_cycles_prev) / (g_cycles_per_s * window) : -1;
    g_game_cycles_prev = gc;
    g_render_cycles_prev = rc;

    char line[2048];
    int n = std::snprintf(line, sizeof(line), "[perf] %.1fs: %.1f fps", window, frames / window);
    if (frame.count) n += std::snprintf(line + n, sizeof(line) - n, " (frame avg %.2f max %.1f ms)", frame.sum / frame.count, frame.max);
    if (game.count)
        n += std::snprintf(line + n, sizeof(line) - n, " | game update avg %.2f max %.1f ms", game.sum / game.count, game.max);
    if (game_cpu >= 0) n += std::snprintf(line + n, sizeof(line) - n, ", game thread cpu %.0f%%", game_cpu);
    if (render.count)
        n += std::snprintf(line + n, sizeof(line) - n, " | render frame avg %.2f max %.1f ms", render.sum / render.count,
                           render.max);
    if (render_cpu >= 0) n += std::snprintf(line + n, sizeof(line) - n, ", render thread cpu %.0f%%", render_cpu);

    if (g_device_hooked && frames > 0) {
        uint32_t d[kDeviceSlots];
        for (int i = 0; i < kDeviceSlots; ++i) {
            const uint32_t c = g_calls[i];
            d[i] = c - g_calls_prev[i];
            g_calls_prev[i] = c;
        }
        const double f = static_cast<double>(frames);
        auto per = [&](int slot) { return d[slot] / f; };
        const TimedSnap present = snap(g_present), rtdata = snap(g_rtdata), vsc = snap(g_vsc), psc = snap(g_psc),
                        dip = snap(g_dip), dp = snap(g_dp), vb = snap(g_lock_vb), ib = snap(g_lock_ib),
                        tex = snap(g_lock_tex), surf = snap(g_lock_surf), gd = snap(g_getdata);
        uint32_t gets = 0, total = 0;
        for (int i = 3; i < kDeviceSlots; ++i) total += d[i];
        const int get_slots[] = {38, 40, 45, 48, 50, 52, 54, 56, 58, 63, 64, 66, 68, 76, 78, 88, 90, 93, 95, 97, 99,
                                 101, 103, 105, 108, 110, 112, 114};
        for (int s : get_slots) gets += d[s];
        const int64_t pmax = g_present.max_ticks;
        g_present.max_ticks = 0;
        n += std::snprintf(
            line + n, sizeof(line) - n,
            " | present avg %.2f max %.1f ms | d3d/frame: calls %.0f, draws %.0f (indexed %.0f, %.0fk prims; plain %.0f; "
            "UP %.0f) vs-const %.0f (%.0f regs) ps-const %.0f (%.0f regs) tex %.0f rs %.0f ss %.0f stream %.0f idx %.0f "
            "decl %.0f vs %.0f ps %.0f rt %.0f ds %.0f clear %.0f stretch %.1f viewport %.0f scissor %.0f transform %.0f "
            "gets %.0f",
            present.calls != g_prev_present.calls ? ms_of(present.ticks - g_prev_present.ticks) / (present.calls - g_prev_present.calls) : 0.0,
            ms_of(pmax), total / f, (d[kDrawIndexed] + d[kDrawPrimitive] + d[kDrawUp] + d[kDrawIndexedUp]) / f,
            per(kDrawIndexed), (dip.extra - g_prev_dip.extra) / f / 1000.0, per(kDrawPrimitive),
            (d[kDrawUp] + d[kDrawIndexedUp]) / f, per(kSetVsConstF), (vsc.extra - g_prev_vsc.extra) / f,
            per(kSetPsConstF), (psc.extra - g_prev_psc.extra) / f, per(kSetTexture), per(kSetRenderState),
            per(kSetSamplerState), per(kSetStreamSource), per(kSetIndices), per(kSetVertexDecl) + per(kSetFvf),
            per(kSetVertexShader), per(kSetPixelShader), per(kSetRenderTarget), per(kSetDepthStencil), per(kClear),
            per(kStretchRect), per(kSetViewport), per(kSetScissor), per(kSetTransform), gets / f);
        auto lock_part = [&](const char* name, const TimedSnap& now_s, const TimedSnap& prev, bool sync_count) {
            const uint32_t c = now_s.calls - prev.calls;
            if (!c) return;
            n += std::snprintf(line + n, sizeof(line) - n, " %s %.1f/frame %.3f ms/frame", name, c / f,
                               ms_of(now_s.ticks - prev.ticks) / f);
            if (sync_count) n += std::snprintf(line + n, sizeof(line) - n, " (%.1f without discard/nooverwrite)",
                                               (now_s.extra - prev.extra) / f);
        };
        // Resources created while the game runs (DXVK showed ~9% of the render thread under the draw plan's render
        // target creation, 0x590920 from HGSurfaceEntity 0xBAD310).
        n += std::snprintf(line + n, sizeof(line) - n,
                           " | creates/frame: rt %.2f ds %.2f tex %.2f vb %.2f ib %.2f offscreen %.2f", per(28), per(29),
                           per(23) + per(24) + per(25), per(26), per(27), per(36));
        n += std::snprintf(line + n, sizeof(line) - n, " | waits:");
        lock_part("vb-lock", vb, g_prev_vb, true);
        lock_part("ib-lock", ib, g_prev_ib, true);
        lock_part("tex-lock", tex, g_prev_tex, false);
        lock_part("surf-lock", surf, g_prev_surf, false);
        lock_part("rt-readback", rtdata, g_prev_rtdata, false);
        {
            static unsigned long prev_async = 0, prev_waits = 0, prev_filtered = 0;
            static unsigned long prev_switches = 0, prev_applies = 0, prev_recorded = 0, prev_seen = 0;
            const unsigned long seen = d3dperf_seen_samplers();
            const unsigned long a = d3dperf_readback_async(), w = d3dperf_readback_waits(),
                                fs = d3dperf_filtered_samplers();
            unsigned long sw = 0, ap = 0, rec = 0;
            d3dperf_filter_stats(&sw, &ap, &rec);
            if (a != prev_async || w != prev_waits)
                n += std::snprintf(line + n, sizeof(line) - n, " (async %.1f/frame, %.1f not ready)",
                                   (a - prev_async) / f, (w - prev_waits) / f);
            n += std::snprintf(line + n, sizeof(line) - n,
                               " | ss reaching the filter %.0f/frame, filtered %.0f/frame (restarts: %.1f thread "
                               "changes, %.1f state blocks applied a frame; %.0f recorded)",
                               (seen - prev_seen) / f, (fs - prev_filtered) / f, (sw - prev_switches) / f,
                               (ap - prev_applies) / f, (rec - prev_recorded) / f);
            prev_seen = seen;
            static unsigned long prev_vc = 0, prev_vr = 0, prev_vk = 0, prev_vs = 0;
            unsigned long vc = 0, vr = 0, vk = 0, vs = 0;
            d3dperf_constant_stats(&vc, &vr, &vk, &vs);
            if (vc != prev_vc)
                n += std::snprintf(line + n, sizeof(line) - n,
                                   " | vs constants reaching the filter %.0f calls %.0f regs/frame: %.0f calls answered, "
                                   "%.0f regs sent",
                                   (vc - prev_vc) / f, (vr - prev_vr) / f, (vk - prev_vk) / f, (vs - prev_vs) / f);
            static unsigned long prev_pr = 0, prev_pc = 0;
            unsigned long pr = 0, pc = 0;
            surfpool_stats(&pr, &pc);
            if (pr != prev_pr || pc != prev_pc)
                n += std::snprintf(line + n, sizeof(line) - n, " | surfaces reused %.2f/frame, created %.2f/frame",
                                   (pr - prev_pr) / f, (pc - prev_pc) / f);
            prev_pr = pr;
            prev_pc = pc;
            prev_vc = vc;
            prev_vr = vr;
            prev_vk = vk;
            prev_vs = vs;
            prev_async = a;
            prev_waits = w;
            prev_filtered = fs;
            prev_switches = sw;
            prev_applies = ap;
            prev_recorded = rec;
        }
        if (gd.calls != g_prev_getdata.calls)
            n += std::snprintf(line + n, sizeof(line) - n, " query-getdata %.1f/frame (%.1f not ready) %.3f ms/frame",
                               (gd.calls - g_prev_getdata.calls) / f, (gd.extra - g_prev_getdata.extra) / f,
                               ms_of(gd.ticks - g_prev_getdata.ticks) / f);
        g_prev_present = present;
        g_prev_rtdata = rtdata;
        g_prev_vsc = vsc;
        g_prev_psc = psc;
        g_prev_dip = dip;
        g_prev_dp = dp;
        g_prev_vb = vb;
        g_prev_ib = ib;
        g_prev_tex = tex;
        g_prev_surf = surf;
        g_prev_getdata = gd;
    }
    float xyz[3];
    if (player_position(xyz)) n += std::snprintf(line + n, sizeof(line) - n, " | pos %.1f %.1f %.1f", xyz[0], xyz[1], xyz[2]);
    if (!quiet) LOG_INFO("%s", line);
    texpreload_report();
    if (!quiet) dxgipresent_report();
}

// ---------------------------------------------------------------------------------------------------------------
// F10 capture: sample every busy thread with the game's return addresses found on its stack.
// ---------------------------------------------------------------------------------------------------------------
struct Sample {
    uint8_t thread;
    uint8_t nret;
    uint16_t pad;
    uint32_t eip;
    uint32_t ret[kMaxRets];
};

struct Picked {
    DWORD tid;
    HANDLE h;
    uintptr_t start;      // thread entry point
    uintptr_t stack_base; // top of its committed stack
    uint64_t cycles0, cycles1;
    int samples;
};

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
NtQueryInformationThreadFn nt_query_thread() {
    static NtQueryInformationThreadFn fn = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    return fn;
}

// `v` is a return address into the game's code when the bytes before it are a call instruction.
bool is_game_return(uint32_t v) {
    if (v < g_text_lo + 8 || v >= g_text_hi) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
    if (p[-5] == 0xE8) {  // call rel32 (calls into other modules go through import thunks in the game's code)
        int32_t rel = 0;
        std::memcpy(&rel, p - 4, 4);
        const uint32_t target = v + static_cast<uint32_t>(rel);
        return target >= g_text_lo && target < g_text_hi;
    }
    if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90) && p[-5] != 0x94) return true;  // [abs], [r+d32]
    if (p[-7] == 0xFF && p[-6] == 0x94) return true;                                                  // [r+r*s+d32]
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50 && p[-2] != 0x54) return true;                        // [r+d8]
    if (p[-4] == 0xFF && p[-3] == 0x54) return true;                                                  // [r+r*s+d8]
    if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || ((p[-1] & 0xF8) == 0x10 && p[-1] != 0x14 && p[-1] != 0x15)))
        return true;  // reg, [reg]
    return false;
}

Sample* g_samples = nullptr;

struct ModuleCache {
    std::unordered_map<uint32_t, HMODULE> by_page;
    HMODULE of(uint32_t a) {
        const uint32_t page = a >> 16;
        auto it = by_page.find(page);
        if (it != by_page.end()) return it->second;
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(a)), &m);
        by_page[page] = m;
        return m;
    }
};

void module_name(HMODULE module, char* out, size_t cap) {
    wchar_t path[MAX_PATH] = {};
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) {
        std::snprintf(out, cap, "(no module)");
        return;
    }
    const wchar_t* slash = wcsrchr(path, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, out, static_cast<int>(cap), nullptr, nullptr);
}

void describe_address(ModuleCache& mc, uint32_t a, char* out, size_t cap) {
    HMODULE m = mc.of(a);
    if (m == GetModuleHandleW(nullptr)) {
        std::snprintf(out, cap, "%08X", a);
        return;
    }
    char name[96];
    module_name(m, name, sizeof(name));
    std::snprintf(out, cap, "%s+%X", name, m ? a - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m)) : a);
}

template <class K>
std::vector<std::pair<K, int>> top_of(const std::unordered_map<K, int>& m, size_t n) {
    std::vector<std::pair<K, int>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > n) v.resize(n);
    return v;
}

void capture() {
    if (!g_samples) {
        g_samples = static_cast<Sample*>(VirtualAlloc(nullptr, sizeof(Sample) * kMaxSamples, MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_READWRITE));
        if (!g_samples) {
            LOG_ERROR("[prof] could not allocate the sample buffer");
            return;
        }
    }
    float xyz[3] = {};
    const bool have_pos = player_position(xyz);
    LOG_INFO("[prof] F10: sampling the busy threads for %d s%s", kCaptureSeconds, have_pos ? "" : " (no player yet)");
    if (have_pos) LOG_INFO("[prof] position %.2f %.2f %.2f", xyz[0], xyz[1], xyz[2]);

    // Every thread of the game, then the busy ones (by CPU cycles over 0.3 s).
    std::vector<Picked> all;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &te); ok; ok = Thread32Next(snapshot, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        Picked p{};
        p.tid = te.th32ThreadID;
        p.h = h;
        if (auto q = nt_query_thread()) {
            PVOID start = nullptr;
            q(h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr);
            p.start = reinterpret_cast<uintptr_t>(start);
        }
        // The top of its stack: the committed read/write region that holds ESP ends there (the pages above ESP are
        // all committed, and the stack's reservation ends at its base).
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(h) != static_cast<DWORD>(-1)) {
            const bool got = GetThreadContext(h, &ctx) != 0;
            ResumeThread(h);
            MEMORY_BASIC_INFORMATION mbi{};
            if (got && ctx.Esp &&
                VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx.Esp)), &mbi, sizeof(mbi)) &&
                mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE))
                p.stack_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
        p.cycles0 = thread_cycles(h);
        all.push_back(p);
    }
    CloseHandle(snapshot);
    const int64_t pick_t0 = qpc();
    Sleep(300);
    const double pick_s = static_cast<double>(qpc() - pick_t0) / g_freq.QuadPart;
    for (Picked& p : all) p.cycles1 = thread_cycles(p.h);
    std::sort(all.begin(), all.end(), [](const Picked& a, const Picked& b) {
        const bool a_key = a.tid == g_game_thread_id || a.tid == g_render_thread_id;
        const bool b_key = b.tid == g_game_thread_id || b.tid == g_render_thread_id;
        if (a_key != b_key) return a_key;
        return (a.cycles1 - a.cycles0) > (b.cycles1 - b.cycles0);
    });
    std::vector<Picked> picked;
    for (Picked& p : all) {
        const double share = (p.cycles1 - p.cycles0) / (g_cycles_per_s * pick_s);
        const bool key = p.tid == g_game_thread_id || p.tid == g_render_thread_id;
        if (picked.size() < static_cast<size_t>(kMaxPicked) && (key || share >= 0.02) && p.stack_base) {
            picked.push_back(p);
        } else {
            CloseHandle(p.h);
        }
    }
    LOG_INFO("[prof] %u threads in the game, %u busy enough to sample", static_cast<unsigned>(all.size()),
             static_cast<unsigned>(picked.size()));

    // Sample. Between Suspend and Resume nothing may allocate, log or fault: the thread could hold a lock. The stack
    // between ESP and the TEB's stack base is always committed, so copying it cannot fault.
    for (Picked& p : picked) {
        p.cycles0 = thread_cycles(p.h);
        p.samples = 0;
    }
    const int64_t t0 = qpc();
    const int64_t t_end = t0 + g_freq.QuadPart * kCaptureSeconds;
    int n = 0;
    uint32_t stack[kStackDwords];
    while (qpc() < t_end && n + static_cast<int>(picked.size()) <= kMaxSamples) {
        for (size_t i = 0; i < picked.size(); ++i) {
            Picked& p = picked[i];
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            int words = 0;
            if (SuspendThread(p.h) == static_cast<DWORD>(-1)) continue;
            const bool got = GetThreadContext(p.h, &ctx) != 0;
            if (got && ctx.Esp && ctx.Esp < p.stack_base && p.stack_base - ctx.Esp < 0x1000000) {
                words = static_cast<int>((p.stack_base - ctx.Esp) / 4);
                if (words > kStackDwords) words = kStackDwords;
                std::memcpy(stack, reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx.Esp)), words * 4);
            }
            ResumeThread(p.h);
            if (!got) continue;
            Sample& s = g_samples[n++];
            s.thread = static_cast<uint8_t>(i);
            s.eip = ctx.Eip;
            s.nret = 0;
            for (int w = 0; w < words && s.nret < kMaxRets; ++w) {
                if (is_game_return(stack[w])) s.ret[s.nret++] = stack[w];
            }
            ++p.samples;
        }
        Sleep(1);
    }
    const double seconds = static_cast<double>(qpc() - t0) / g_freq.QuadPart;
    for (Picked& p : picked) {
        p.cycles1 = thread_cycles(p.h);
        CloseHandle(p.h);
    }

    // Report.
    ModuleCache mc;
    const HMODULE game = GetModuleHandleW(nullptr);
    LOG_INFO("[prof] %d samples over %.1f s", n, seconds);
    for (size_t ti = 0; ti < picked.size(); ++ti) {
        const Picked& p = picked[ti];
        const double cpu = 100.0 * (p.cycles1 - p.cycles0) / (g_cycles_per_s * seconds);
        char start[160];
        describe_address(mc, static_cast<uint32_t>(p.start), start, sizeof(start));
        const char* role = p.tid == g_game_thread_id ? "game thread" : p.tid == g_render_thread_id ? "render thread" : "thread";
        LOG_INFO("[prof] == %s %lu (starts at %s): cpu %.1f%% of a core, %d samples", role, p.tid, start, cpu,
                 p.samples);
        if (p.samples < 50) continue;
        const double total = p.samples;

        std::unordered_map<HMODULE, int> modules;
        std::unordered_map<uint32_t, int> exclusive, inclusive, callers_outside;
        std::unordered_map<uint64_t, int> outside_by_module;  // (module, first game return)
        for (int k = 0; k < n; ++k) {
            const Sample& s = g_samples[k];
            if (s.thread != ti) continue;
            HMODULE m = mc.of(s.eip);
            ++modules[m];
            const bool in_game = m == game;
            ++exclusive[in_game ? (s.eip & ~0xFu) : ((s.eip & ~0xFFu) | 1u)];
            uint32_t seen[kMaxRets];
            int seen_n = 0;
            for (int r = 0; r < s.nret; ++r) {
                bool dup = false;
                for (int q = 0; q < seen_n; ++q) dup = dup || seen[q] == s.ret[r];
                if (!dup) {
                    seen[seen_n++] = s.ret[r];
                    ++inclusive[s.ret[r]];
                }
            }
            if (!in_game && s.nret > 0) {
                ++callers_outside[s.ret[0]];
                ++outside_by_module[(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m)) << 32) | s.ret[0]];
            }
        }
        int ffx_self = 0, ffx_incl = 0;
        for (int k = 0; k < n; ++k) {
            const Sample& s = g_samples[k];
            if (s.thread != ti) continue;
            bool under = in_ffx(s.eip);
            if (under) ++ffx_self;
            for (int r = 0; r < s.nret && !under; ++r) under = in_ffx(s.ret[r]);
            if (under) ++ffx_incl;
        }
        char line[1000];
        int len = 0;
        if (ffx_incl)
            LOG_INFO("[prof]   effects library (FFX, a debug build): %.1f%% self, %.1f%% with what it calls",
                     100.0 * ffx_self / total, 100.0 * ffx_incl / total);
        len = std::snprintf(line, sizeof(line), "[prof]   modules:");
        for (auto& e : top_of(modules, 8)) {
            char name[96];
            module_name(e.first, name, sizeof(name));
            len += std::snprintf(line + len, sizeof(line) - len, " %s %.1f%%", name, 100.0 * e.second / total);
        }
        LOG_INFO("%s", line);
        LOG_INFO("[prof]   hottest code (self time; game addresses are 16-byte blocks):");
        for (auto& e : top_of(exclusive, 30)) {
            if (100.0 * e.second / total < 0.4) break;
            char where[160];
            describe_address(mc, e.first & ~1u, where, sizeof(where));
            LOG_INFO("[prof]     %5.1f%%  %s", 100.0 * e.second / total, where);
        }
        LOG_INFO("[prof]   game call sites the time was spent under (return addresses, inclusive):");
        for (auto& e : top_of(inclusive, 60)) {
            if (100.0 * e.second / total < 0.8) break;
            LOG_INFO("[prof]     %5.1f%%  ret %08X", 100.0 * e.second / total, e.first);
        }
        LOG_INFO("[prof]   time outside the game's code, by the game call it came from:");
        for (auto& e : top_of(outside_by_module, 30)) {
            if (100.0 * e.second / total < 0.4) break;
            char name[96];
            module_name(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(e.first >> 32)), name, sizeof(name));
            LOG_INFO("[prof]     %5.1f%%  %s under ret %08X", 100.0 * e.second / total, name,
                     static_cast<uint32_t>(e.first));
        }
        // Every game code block with two samples or more, for adding up by function offline (the profile of the
        // effects library is flat: many small functions under 1% each).
        if (p.tid == g_render_thread_id || p.tid == g_game_thread_id) {
            std::vector<std::pair<uint32_t, int>> blocks;
            for (auto& e : exclusive)
                if (!(e.first & 1u) && e.second >= 2) blocks.push_back(e);
            std::sort(blocks.begin(), blocks.end());
            LOG_INFO("[prof]   self histogram (%u game code blocks, 16 bytes, samples of %d):",
                     static_cast<unsigned>(blocks.size()), p.samples);
            char hist[400];
            int hl = 0;
            for (size_t b = 0; b < blocks.size(); ++b) {
                hl += std::snprintf(hist + hl, sizeof(hist) - hl, " %X:%d", blocks[b].first, blocks[b].second);
                if (hl > 330 || b + 1 == blocks.size()) {
                    LOG_INFO("[prof]   hist%s", hist);
                    hl = 0;
                }
            }
        }
    }
    LOG_INFO("[prof] done");
}

// ---------------------------------------------------------------------------------------------------------------
// Spike watch: a game update that runs long (a hitch) is sampled while it runs, and reported when it ends. Besides the
// game thread, the game's other threads are looked at twice during the hitch (CPU used, where they were), since a game
// thread that waits is usually waiting for one of them. Everything slow (the thread and module lists, telling return
// addresses from data) is done between hitches or after one, so the first look comes ~1 ms after the hitch is noticed.
// ---------------------------------------------------------------------------------------------------------------
volatile LONGLONG g_update_start = 0;  // qpc at the start of the update in progress, 0 between updates
volatile LONGLONG g_last_start = 0;    // the last finished update: its start and length
volatile double g_last_ms = 0.0;
volatile double g_update_avg = 1.0;    // running average of the update (ms)
constexpr double kSpikeMinMs = 4.0;
constexpr int kSpikeSamples = 256;
constexpr int kSpikeReports = 400;     // per session
#ifndef SPIKE_DETAILED
#define SPIKE_DETAILED 80
#endif
constexpr int kSpikeDetailed = SPIKE_DETAILED;  // reports with the other threads (hitches under kDetailMaxMs)
constexpr double kDetailMaxMs = 150.0;
constexpr int kOrdered = 40;           // return addresses into any module, in stack order (innermost first)
constexpr int kRawSamples = 6;         // game-thread looks kept whole (stack in order, wait arguments)
constexpr int kRawDwords = 1536;       // 6 KB of stack each
constexpr int kOtherThreads = 96;
constexpr int kOtherRounds = 2;        // looks at the other threads: when the hitch is noticed and ~5 ms later
constexpr int kOtherRawDwords = 512;   // 2 KB of stack each
uint32_t g_own_lo = 0, g_own_hi = 0;   // this DLL

struct SpikeSample {
    uint32_t eip;
    uint8_t nret;
    uint32_t ret[kMaxRets];   // into the game or this DLL (counted)
};
SpikeSample g_spike[kSpikeSamples];
uint32_t g_spike_stack[kStackDwords];
uint32_t g_raw[kRawSamples][kRawDwords];
int g_raw_words[kRawSamples];
uint32_t g_raw_eip[kRawSamples], g_raw_esp[kRawSamples];
double g_raw_at[kRawSamples];          // ms into the update

struct OtherThread {
    DWORD tid;
    HANDLE h;
    uintptr_t start, stack_base;
    uint64_t cycles0, cycles1;
    int rounds;
    bool seen;
    uint32_t eip[kOtherRounds];
    int words[kOtherRounds];
};
OtherThread g_other[kOtherThreads];
int g_other_n = 0;
uint32_t g_other_raw[kOtherThreads][kOtherRounds][kOtherRawDwords];

struct ModRange {
    uint32_t lo, hi;
};
std::vector<ModRange> g_mods;  // loaded modules, sorted (spike thread only, refreshed between hitches)

void refresh_modules() {
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    const DWORD count = (std::min)(need / static_cast<DWORD>(sizeof(HMODULE)), static_cast<DWORD>(512));
    std::vector<ModRange> list;
    list.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        MODULEINFO mi{};
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        const uint32_t lo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll));
        list.push_back({lo, lo + mi.SizeOfImage});
    }
    std::sort(list.begin(), list.end(), [](const ModRange& a, const ModRange& b) { return a.lo < b.lo; });
    g_mods.swap(list);
}

// A return address into [lo, hi): the bytes before it are a call.
bool is_return_into(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo + 8 || v >= hi) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
    if (p[-5] == 0xE8) return true;
    if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90) && p[-5] != 0x94) return true;
    if (p[-7] == 0xFF && p[-6] == 0x94) return true;
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50 && p[-2] != 0x54) return true;
    if (p[-4] == 0xFF && p[-3] == 0x54) return true;
    if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || ((p[-1] & 0xF8) == 0x10 && p[-1] != 0x14 && p[-1] != 0x15)))
        return true;
    return false;
}

bool guarded_return_into(uint32_t v, uint32_t lo, uint32_t hi) {
    __try {
        return is_return_into(v, lo, hi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A return address into any loaded module (the game, Windows, drivers, overlays).
bool is_return_any(uint32_t v) {
    if (v >= g_text_lo && v < g_text_hi) return is_game_return(v);
    auto it = std::upper_bound(g_mods.begin(), g_mods.end(), v,
                               [](uint32_t a, const ModRange& r) { return a < r.lo; });
    if (it == g_mods.begin()) return false;
    --it;
    if (v < it->lo + 0x1000 || v >= it->hi) return false;
    return guarded_return_into(v, it->lo + 0x1000, it->hi);
}

int ordered_returns(const uint32_t* stack, int words, uint32_t* out, int cap) {
    int n = 0;
    for (int w = 0; w < words && n < cap; ++w) {
        if (is_return_any(stack[w])) out[n++] = stack[w];
    }
    return n;
}

uintptr_t thread_stack_base(HANDLE h) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(h) == static_cast<DWORD>(-1)) return 0;
    const bool got = GetThreadContext(h, &ctx) != 0;
    ResumeThread(h);
    MEMORY_BASIC_INFORMATION mbi{};
    if (got && ctx.Esp &&
        VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx.Esp)), &mbi, sizeof(mbi)) &&
        mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE))
        return reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return 0;
}

// NtGetNextThread walks this process's threads only (a Toolhelp snapshot lists every thread on the PC: ~120 ms).
using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

// Between hitches: the game's other threads, kept open. New ones get their stack's top (one brief suspend each).
void threads_refresh(DWORD game_tid) {
    static NtGetNextThreadFn next_thread = reinterpret_cast<NtGetNextThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread"));
    if (!next_thread) return;
    for (int i = 0; i < g_other_n; ++i) g_other[i].seen = false;
    const DWORD self = GetCurrentThreadId();
    HANDLE h = nullptr;
    for (;;) {
        HANDLE next = nullptr;
        const LONG status = next_thread(GetCurrentProcess(), h, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                        THREAD_QUERY_INFORMATION, 0, 0, &next);
        if (h) CloseHandle(h);
        if (status < 0 || !next) break;
        h = next;
        const DWORD tid = GetThreadId(h);
        if (tid == self || tid == game_tid) continue;
        int found = -1;
        for (int i = 0; i < g_other_n && found < 0; ++i) {
            if (g_other[i].tid == tid) found = i;
        }
        if (found >= 0) {
            g_other[found].seen = true;
            continue;
        }
        if (g_other_n >= kOtherThreads) continue;
        HANDLE keep = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &keep, 0, FALSE, DUPLICATE_SAME_ACCESS))
            continue;
        OtherThread& o = g_other[g_other_n++];
        o = OtherThread{};
        o.tid = tid;
        o.h = keep;
        o.seen = true;
        if (auto q = nt_query_thread()) {
            PVOID start = nullptr;
            q(keep, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr);
            o.start = reinterpret_cast<uintptr_t>(start);
        }
        o.stack_base = thread_stack_base(keep);
    }
    int kept = 0;
    for (int i = 0; i < g_other_n; ++i) {
        if (g_other[i].seen) {
            g_other[kept++] = g_other[i];
        } else {
            CloseHandle(g_other[i].h);
        }
    }
    g_other_n = kept;
}

// Copies the stack of a suspended-for-a-moment thread. Between Suspend and Resume nothing may allocate, log or fault
// (the thread could hold a lock); the stack between ESP and its top is committed, so copying it cannot fault.
bool look(HANDLE h, uintptr_t stack_base, uint32_t* out, int cap, int* words, uint32_t* eip, uint32_t* esp) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    *words = 0;
    if (SuspendThread(h) == static_cast<DWORD>(-1)) return false;
    const bool got = GetThreadContext(h, &ctx) != 0;
    if (got && stack_base && ctx.Esp && ctx.Esp < stack_base && stack_base - ctx.Esp < 0x1000000) {
        int n = static_cast<int>((stack_base - ctx.Esp) / 4);
        if (n > cap) n = cap;
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx.Esp)), n * 4);
        *words = n;
    }
    ResumeThread(h);
    if (!got) return false;
    *eip = ctx.Eip;
    if (esp) *esp = ctx.Esp;
    return true;
}

void others_begin() {
    for (int i = 0; i < g_other_n; ++i) {
        g_other[i].cycles0 = thread_cycles(g_other[i].h);
        g_other[i].rounds = 0;
    }
}

void others_look() {
    for (int i = 0; i < g_other_n; ++i) {
        OtherThread& o = g_other[i];
        if (o.rounds >= kOtherRounds) continue;
        const int r = o.rounds;
        if (look(o.h, o.stack_base, g_other_raw[i][r], kOtherRawDwords, &o.words[r], &o.eip[r], nullptr)) ++o.rounds;
    }
}

void others_end() {
    for (int i = 0; i < g_other_n; ++i) g_other[i].cycles1 = thread_cycles(g_other[i].h);
}

// What a thread stopped in a system call waits for, from the call's arguments on its stack (32-bit stubs: the
// thread sits on the stub's `ret`, ESP at the return address into its caller, the arguments above).
void describe_wait(uint32_t eip, const uint32_t* stack, int words, uint32_t esp, char* out, size_t cap) {
    out[0] = 0;
    static HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    static HMODULE win32u = GetModuleHandleW(L"win32u.dll");
    static const char* const kNt[] = {"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtDelayExecution",
                                      "NtWaitForAlertByThreadId", "NtReadFile", "NtWriteFile", "NtDeviceIoControlFile",
                                      "NtRemoveIoCompletion", "NtWaitForKeyedEvent", "NtRequestWaitReplyPort",
                                      "NtAlpcSendWaitReceivePort", "NtFsControlFile", "NtFlushBuffersFile",
                                      "NtQueryDirectoryFile", "NtCreateFile", "NtOpenFile"};
    static const char* const kWin32u[] = {"NtUserMsgWaitForMultipleObjectsEx", "NtUserPeekMessage", "NtUserGetMessage",
                                          "NtUserGetAsyncKeyState", "NtUserGetKeyState", "NtUserSetCursorPos",
                                          "NtUserGetRawInputData", "NtUserGetRawInputBuffer", "NtGdiDdDDIGetScanLine"};
    const char* call = nullptr;
    for (const char* name : kNt) {
        const uint32_t a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetProcAddress(ntdll, name)));
        if (a && eip >= a && eip < a + 0x10) call = name;
    }
    for (const char* name : kWin32u) {
        const uint32_t a = win32u ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetProcAddress(win32u, name))) : 0;
        if (a && eip >= a && eip < a + 0x10) call = name;
    }
    if (!call) return;
    int first = 0;  // the return address into the stub's caller; the arguments follow it
    while (first < 4 && first < words && !is_return_any(stack[first])) ++first;
    if (first >= 4 || first >= words) first = 0;
    auto arg = [&](int i) -> uint32_t { return first + i < words ? stack[first + i] : 0; };
    auto stack_word = [&](uint32_t address, int k) -> int64_t {  // a value the thread keeps on its own stack
        if (address < esp || address >= esp + static_cast<uint32_t>(words) * 4) return INT64_MIN;
        int64_t v = 0;
        std::memcpy(&v, reinterpret_cast<const uint8_t*>(stack) + (address - esp), sizeof(v));
        (void)k;
        return v;
    };
    auto timeout = [&](uint32_t p) -> double {
        if (!p) return -1.0;
        const int64_t v = stack_word(p, 0);
        if (v == INT64_MIN) return -2.0;
        return v < 0 ? static_cast<double>(-v) / 10000.0 : -3.0;
    };
    auto type_of = [](HANDLE h, char* t, size_t tcap) {
        std::snprintf(t, tcap, "?");
        using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
        static NtQueryObjectFn q = reinterpret_cast<NtQueryObjectFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
        alignas(8) uint8_t buf[512];
        if (!q || q(h, 2 /*ObjectTypeInformation*/, buf, sizeof(buf), nullptr) < 0) return;
        struct UStr {
            USHORT Length, MaximumLength;
            PWSTR Buffer;
        };
        const UStr* name = reinterpret_cast<const UStr*>(buf);
        WideCharToMultiByte(CP_UTF8, 0, name->Buffer, name->Length / 2, t, static_cast<int>(tcap) - 1, nullptr, nullptr);
        t[(std::min)(static_cast<size_t>(name->Length / 2), tcap - 1)] = 0;
    };
    char t[64];
    int n = std::snprintf(out, cap, " in %s", call);
    if (std::strcmp(call, "NtWaitForSingleObject") == 0) {
        type_of(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(arg(1))), t, sizeof(t));
        std::snprintf(out + n, cap - n, " (handle %X, type %s; timeout %.1f ms)", arg(1), t, timeout(arg(3)));
    } else if (std::strcmp(call, "NtWaitForMultipleObjects") == 0) {
        std::snprintf(out + n, cap - n, " (%u handles; timeout %.1f ms)", arg(1), timeout(arg(5)));
    } else if (std::strcmp(call, "NtDelayExecution") == 0) {
        std::snprintf(out + n, cap - n, " (Sleep %.1f ms)", timeout(arg(2)));
    } else if (std::strcmp(call, "NtWaitForAlertByThreadId") == 0) {
        std::snprintf(out + n, cap - n, " (a lock or condition at %X; timeout %.1f ms)", arg(1), timeout(arg(2)));
    } else if (std::strncmp(call, "NtRead", 6) == 0 || std::strncmp(call, "NtWrite", 7) == 0 ||
               std::strcmp(call, "NtDeviceIoControlFile") == 0 || std::strcmp(call, "NtFsControlFile") == 0) {
        type_of(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(arg(1))), t, sizeof(t));
        std::snprintf(out + n, cap - n, " (handle %X, type %s)", arg(1), t);
    }
}

int append_stack(char* line, int len, int cap, ModuleCache& mc, uint32_t eip, const uint32_t* ret, int nret) {
    char where[160];
    describe_address(mc, eip, where, sizeof(where));
    len += std::snprintf(line + len, cap - len, " %s", where);
    for (int r = 0; r < nret && len < cap - 1; ++r) {
        describe_address(mc, ret[r], where, sizeof(where));
        len += std::snprintf(line + len, cap - len, " < %s", where);
    }
    return (std::min)(len, cap - 1);
}

void spike_report(int n, int raw_n, double ms, double cpu_ms, double since_s, double at_s, int index, bool detailed) {
    ModuleCache mc;
    std::unordered_map<uint32_t, int> self, under;
    for (int k = 0; k < n; ++k) {
        const SpikeSample& s = g_spike[k];
        const bool in_game = s.eip >= g_text_lo && s.eip < g_text_hi;
        ++self[in_game ? (s.eip & ~0xFu) : ((s.eip & ~0xFFu) | 1u)];
        uint32_t seen[kMaxRets];
        int seen_n = 0;
        for (int r = 0; r < s.nret; ++r) {
            bool dup = false;
            for (int q = 0; q < seen_n; ++q) dup = dup || seen[q] == s.ret[r];
            if (!dup) {
                seen[seen_n++] = s.ret[r];
                ++under[s.ret[r]];
            }
        }
    }
    char line[2400];
    int len = std::snprintf(line, sizeof(line),
                            "[spike] #%d at %.2f s: game update %.1f ms (cpu %.1f ms), %.2f s after the last, %d samples | self:",
                            index, at_s, ms, cpu_ms, since_s, n);
    for (auto& e : top_of(self, 6)) {
        char where[160];
        describe_address(mc, e.first & ~1u, where, sizeof(where));
        len += std::snprintf(line + len, sizeof(line) - len, " %s x%d", where, e.second);
    }
    len += std::snprintf(line + len, sizeof(line) - len, " | under:");
    for (auto& e : top_of(under, 16)) {
        char where[160];
        describe_address(mc, e.first, where, sizeof(where));
        len += std::snprintf(line + len, sizeof(line) - len, " %s x%d", where, e.second);
    }
    LOG_INFO("%s", line);
    if (!detailed || raw_n == 0) return;
    // The game thread in order (innermost first): its first look, and the last kept one when it was elsewhere.
    uint32_t ord[kOrdered];
    for (int k = 0; k < raw_n; ++k) {
        if (k != 0 && k != raw_n - 1) continue;
        if (k != 0 && (g_raw_eip[k] & ~0xFFu) == (g_raw_eip[0] & ~0xFFu)) continue;
        char wait[200];
        describe_wait(g_raw_eip[k], g_raw[k], g_raw_words[k], g_raw_esp[k], wait, sizeof(wait));
        const int nord = ordered_returns(g_raw[k], g_raw_words[k], ord, kOrdered);
        len = std::snprintf(line, sizeof(line), "[spike]   game thread %.1f ms into the update%s:", g_raw_at[k], wait);
        append_stack(line, len, sizeof(line), mc, g_raw_eip[k], ord, nord);
        LOG_INFO("%s", line);
    }
    // The other threads that ran during the hitch, busiest first, and the render thread.
    int order[kOtherThreads];
    int count = 0;
    for (int i = 0; i < g_other_n; ++i) order[count++] = i;
    std::sort(order, order + count, [](int a, int b) {
        return g_other[a].cycles1 - g_other[a].cycles0 > g_other[b].cycles1 - g_other[b].cycles0;
    });
    int shown = 0;
    for (int k = 0; k < count; ++k) {
        const int i = order[k];
        const OtherThread& o = g_other[i];
        const double used = 1000.0 * static_cast<double>(o.cycles1 - o.cycles0) / g_cycles_per_s;
        const bool render = o.tid == g_render_thread_id;
        if (!render && (shown >= 4 || used < 0.3)) continue;
        if (!render) ++shown;
        char start[160];
        describe_address(mc, static_cast<uint32_t>(o.start), start, sizeof(start));
        len = std::snprintf(line, sizeof(line), "[spike]   %s %lu (starts at %s): cpu %.1f ms during the hitch;",
                            render ? "render thread" : "thread", o.tid, start, used);
        for (int r = 0; r < o.rounds && len < static_cast<int>(sizeof(line)) - 300; ++r) {
            if (r > 0 && (o.eip[r] & ~0xFFu) == (o.eip[0] & ~0xFFu)) continue;
            const int nord = ordered_returns(g_other_raw[i][r], o.words[r], ord, r == 0 ? 12 : 4);
            len += std::snprintf(line + len, sizeof(line) - len, "%s", r == 0 ? "" : " | later:");
            len = append_stack(line, len, sizeof(line), mc, o.eip[r], ord, nord);
        }
        LOG_INFO("%s", line);
    }
}

DWORD WINAPI spike_thread(void*) {
    HANDLE h = nullptr;
    uintptr_t stack_base = 0;
    LONGLONG sampling = 0, others_next = 0, maintain_next = 0;
    int n = 0, raw_n = 0, reports = 0, detailed = 0;
    bool others = false;
    uint64_t cycles0 = 0;
    LONGLONG last_spike = 0;
    const LONGLONG t_begin = qpc();
    for (;;) {
        Sleep(1);
        if (reports >= kSpikeReports) return 0;
        if (!h) {
            if (!g_game_thread_id || g_cycles_per_s <= 0.0) continue;
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                           g_game_thread_id);
            if (!h) return 0;
            stack_base = thread_stack_base(h);
            if (!stack_base) return 0;
            LOG_INFO("[spike] watching the game thread: a game update over %.0f ms (or 3x the average) is sampled "
                     "every millisecond while it runs, the game's other threads twice", kSpikeMinMs);
        }
        const LONGLONG start = g_update_start;
        const LONGLONG now = qpc();
        if (sampling && start != sampling) {
            // That update has ended (the game thread stores its length before clearing the start).
            const double ms = g_last_start == sampling ? g_last_ms : ms_of(now - sampling);
            const double cpu_ms = 1000.0 * static_cast<double>(thread_cycles(h) - cycles0) / g_cycles_per_s;
            const double since = last_spike ? static_cast<double>(sampling - last_spike) / g_freq.QuadPart : 0.0;
            if (others) others_end();
            const bool detail = others && ms < kDetailMaxMs;
            if (detail) ++detailed;
            spike_report(n, raw_n, ms, cpu_ms, since, static_cast<double>(sampling - t_begin) / g_freq.QuadPart,
                         ++reports, detail);
            last_spike = sampling;
            sampling = 0;
            n = raw_n = 0;
            others = false;
            maintain_next = 0;
        }
        if (!start) {
            if (now >= maintain_next) {  // between hitches: keep the lists ready
                refresh_modules();
                threads_refresh(g_game_thread_id);
                maintain_next = now + g_freq.QuadPart * 2;
            }
            continue;
        }
        const double threshold = (std::max)(kSpikeMinMs, 3.0 * g_update_avg);
        if (ms_of(now - start) < threshold) continue;
        if (!sampling) {
            sampling = start;
            n = raw_n = 0;
            cycles0 = thread_cycles(h);
            others = detailed < kSpikeDetailed && g_other_n > 0;
            if (others) others_begin();
            others_next = 0;
        }
        if (n < kSpikeSamples) {
            SpikeSample& s = g_spike[n];
            uint32_t eip = 0, esp = 0;
            int words = 0;
            uint32_t* buf = raw_n < kRawSamples ? g_raw[raw_n] : g_spike_stack;
            const int cap = raw_n < kRawSamples ? kRawDwords : kStackDwords;
            if (look(h, stack_base, buf, cap, &words, &eip, &esp)) {
                ++n;
                s.eip = eip;
                s.nret = 0;
                for (int w = 0; w < words && s.nret < kMaxRets; ++w) {
                    const uint32_t v = buf[w];
                    if (is_game_return(v) || is_return_into(v, g_own_lo, g_own_hi)) s.ret[s.nret++] = v;
                }
                if (raw_n < kRawSamples) {
                    g_raw_words[raw_n] = words;
                    g_raw_eip[raw_n] = eip;
                    g_raw_esp[raw_n] = esp;
                    g_raw_at[raw_n] = ms_of(qpc() - start);
                    ++raw_n;
                }
            }
        }
        if (others && qpc() >= others_next) {
            others_look();
            others_next = qpc() + g_freq.QuadPart * 5 / 1000;
        }
    }
}

DWORD WINAPI profile_thread(void*) {
    calibrate_cycles();
    bool was_down = false;
    int64_t next_report = qpc() + g_freq.QuadPart * kReportSeconds;
    for (;;) {
        Sleep(50);
        const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down && !was_down) {
            report();  // close the window before the capture so the sampling does not count in it
            capture();
            g_report_prev = 0;
            report();
            next_report = qpc() + g_freq.QuadPart * kReportSeconds;
        }
        was_down = down;
        if (qpc() >= next_report) {
            report();
            next_report = qpc() + g_freq.QuadPart * kReportSeconds;
        }
    }
    return 0;
}

}  // namespace

bool profile_enabled() { return g_enabled; }

void profile_game_frame_begin() {
    if (!g_game_thread_id) g_game_thread_id = GetCurrentThreadId();
    g_update_start = qpc();
}

void profile_game_frame(double ms) {
    if (!g_game_thread_id) g_game_thread_id = GetCurrentThreadId();
    g_game_ms.add(ms);
    g_last_ms = ms;
    g_last_start = g_update_start;
    g_update_start = 0;
    g_update_avg = g_update_avg * 0.98 + (ms < 50.0 ? ms : 50.0) * 0.02;
}

void profile_frame_boundary() {
    const int64_t now = qpc();
    if (g_last_boundary) g_frame_ms.add(ms_of(now - g_last_boundary));
    g_last_boundary = now;
    InterlockedIncrement(&g_frames);
}

void profile_render_span(double ms) { g_render_ms.add(ms); }

void profile_hook_device(IDirect3DDevice9* device) {
    if (!g_enabled || !device || g_device_hooked) return;
    void** vt = *reinterpret_cast<void***>(device);
    g_thunks = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_thunks) return;
    DWORD old = 0;
    if (!VirtualProtect(vt, kDeviceSlots * sizeof(void*), PAGE_READWRITE, &old)) {
        LOG_ERROR("[perf] could not unprotect the device vtable (Win32=%lu)", GetLastError());
        return;
    }
    for (int i = 0; i < kDeviceSlots; ++i) g_orig[i] = vt[i];
    for (int i = 3; i < kDeviceSlots; ++i) {
        void* own = nullptr;
        switch (i) {
            case kPresent: own = reinterpret_cast<void*>(&hook_present); break;
            case kGetRtData: own = reinterpret_cast<void*>(&hook_get_rt_data); break;
            case kSetVsConstF: own = reinterpret_cast<void*>(&hook_vsc); break;
            case kSetPsConstF: own = reinterpret_cast<void*>(&hook_psc); break;
            case kDrawIndexed: own = reinterpret_cast<void*>(&hook_dip); break;
            case kDrawPrimitive: own = reinterpret_cast<void*>(&hook_dp); break;
            case kDrawUp: own = reinterpret_cast<void*>(&hook_dpup); break;
            case kDrawIndexedUp: own = reinterpret_cast<void*>(&hook_dipup); break;
            case kSetVertexShader: own = reinterpret_cast<void*>(&hook_set_vs); break;
            case kSetPixelShader: own = reinterpret_cast<void*>(&hook_set_ps); break;
            case kSetVertexDecl: own = reinterpret_cast<void*>(&hook_set_decl); break;
            case kSetTexture: own = reinterpret_cast<void*>(&hook_set_tex_slow); break;
            case kCreateTexture: own = reinterpret_cast<void*>(&hook_create_tex); break;
            default: break;
        }
        if (!own) {
            uint8_t* t = g_thunks + i * 12;
            const uint32_t counter = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_calls[i]));
            const uint32_t target = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_orig[i]));
            t[0] = 0xFF;  // inc dword ptr [counter]
            t[1] = 0x05;
            std::memcpy(t + 2, &counter, 4);
            t[6] = 0xFF;  // jmp dword ptr [original]
            t[7] = 0x25;
            std::memcpy(t + 8, &target, 4);
            own = t;
        }
        vt[i] = own;
    }
    DWORD ignored = 0;
    VirtualProtect(g_thunks, 4096, PAGE_EXECUTE_READ, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_thunks, 4096);
    VirtualProtect(vt, kDeviceSlots * sizeof(void*), old, &ignored);
    g_device_hooked = true;
    hook_resources(device);
    for (int i = 0; i < kDeviceSlots; ++i) g_calls_prev[i] = g_calls[i];
    LOG_INFO("[perf] Direct3D device %p: %d methods counted (vtable %p), %d lock/query classes timed", device,
             kDeviceSlots - 3, static_cast<void*>(vt), g_res_count);
}

bool profile_start() {
    QueryPerformanceFrequency(&g_freq);
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            g_text_lo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base)) + sec[i].VirtualAddress;
            g_text_hi = g_text_lo + sec[i].Misc.VirtualSize;
        }
    }
    g_enabled = true;
    streamwatch_install();
    HANDLE thread = CreateThread(nullptr, 0, profile_thread, nullptr, 0, nullptr);
    if (!thread) {
        g_enabled = false;
        return false;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
    CloseHandle(thread);
    HMODULE own = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&profile_start), &own) && own) {
        const auto* odos = reinterpret_cast<const IMAGE_DOS_HEADER*>(own);
        const auto* ont = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const uint8_t*>(own) + odos->e_lfanew);
        g_own_lo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(own)) + 0x1000;
        g_own_hi = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(own)) + ont->OptionalHeader.SizeOfImage;
    }
    HANDLE spikes = CreateThread(nullptr, 0, spike_thread, nullptr, 0, nullptr);
    if (spikes) {
        SetThreadPriority(spikes, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(spikes);
    }
    LOG_INFO("Profiler ready: a [perf] line every %d s; press F10 in a slow scene to sample every busy thread for %d s",
             kReportSeconds, kCaptureSeconds);
    return true;
}

#else  // A release build leaves this diagnostic out (see fpsu_buildcfg.h).

volatile DWORD g_render_thread_id = 0;
volatile DWORD g_game_thread_id = 0;
bool profile_start() { return false; }
bool profile_enabled() { return false; }
void profile_hook_device(IDirect3DDevice9*) {}
void profile_game_frame_begin() {}
void profile_game_frame(double) {}
void profile_frame_boundary() {}
void profile_render_span(double) {}

#endif  // FPSU_DIAGNOSTICS
