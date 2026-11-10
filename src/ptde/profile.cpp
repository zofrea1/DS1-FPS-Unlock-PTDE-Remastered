#include "profile.h"

#include "d3dperf.h"
#include "fpsu_buildcfg.h"

#if FPSU_DIAGNOSTICS

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
uint32_t g_text_lo = 0, g_text_hi = 0;  // the game's code (.text), fixed base 0x400000

// ---------------------------------------------------------------------------------------------------------------
// Direct3D 9 census. Each device method the game calls goes through a 12-byte thunk that counts it, except a few
// with their own hook that also counts primitives, registers or time.
// ---------------------------------------------------------------------------------------------------------------
constexpr int kDeviceSlots = 119;  // IDirect3DDevice9: QueryInterface (0) .. CreateQuery (118)
enum Slot : int {
    kPresent = 17, kGetRtData = 32, kStretchRect = 34, kSetRenderTarget = 37, kSetDepthStencil = 39, kClear = 43,
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
HRESULT WINAPI hook_dip(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT base, UINT min, UINT num, UINT start,
                        UINT prims) {
    ++g_calls[kDrawIndexed];
    g_dip.extra += prims;
    return reinterpret_cast<DipFn>(g_orig[kDrawIndexed])(d, type, base, min, num, start, prims);
}
HRESULT WINAPI hook_dp(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
    ++g_calls[kDrawPrimitive];
    g_dp.extra += prims;
    return reinterpret_cast<DpFn>(g_orig[kDrawPrimitive])(d, type, start, prims);
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
    const double window = static_cast<double>(now - g_report_prev) / g_freq.QuadPart;
    if (g_report_prev == 0 || window <= 0.0) {
        g_report_prev = now;
        return;
    }
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

    char line[1400];
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
        n += std::snprintf(line + n, sizeof(line) - n, " | waits:");
        lock_part("vb-lock", vb, g_prev_vb, true);
        lock_part("ib-lock", ib, g_prev_ib, true);
        lock_part("tex-lock", tex, g_prev_tex, false);
        lock_part("surf-lock", surf, g_prev_surf, false);
        lock_part("rt-readback", rtdata, g_prev_rtdata, false);
        {
            static unsigned long prev_async = 0, prev_waits = 0, prev_filtered = 0;
            const unsigned long a = d3dperf_readback_async(), w = d3dperf_readback_waits(),
                                fs = d3dperf_filtered_samplers();
            if (a != prev_async || w != prev_waits)
                n += std::snprintf(line + n, sizeof(line) - n, " (async %.1f/frame, %.1f not ready)",
                                   (a - prev_async) / f, (w - prev_waits) / f);
            if (fs != prev_filtered)
                n += std::snprintf(line + n, sizeof(line) - n, " | ss filtered %.0f/frame", (fs - prev_filtered) / f);
            prev_async = a;
            prev_waits = w;
            prev_filtered = fs;
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
    LOG_INFO("%s", line);
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
        char line[1000];
        int len = std::snprintf(line, sizeof(line), "[prof]   modules:");
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
    }
    LOG_INFO("[prof] done");
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

void profile_game_frame(double ms) {
    if (!g_game_thread_id) g_game_thread_id = GetCurrentThreadId();
    g_game_ms.add(ms);
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
    HANDLE thread = CreateThread(nullptr, 0, profile_thread, nullptr, 0, nullptr);
    if (!thread) {
        g_enabled = false;
        return false;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
    CloseHandle(thread);
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
void profile_game_frame(double) {}
void profile_frame_boundary() {}
void profile_render_span(double) {}

#endif  // FPSU_DIAGNOSTICS
