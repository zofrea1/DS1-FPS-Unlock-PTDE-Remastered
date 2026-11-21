#include "passcull.h"

#include "occl.h"

#include "log.h"
#include "resolution.h"

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
//
// A draw that reads the picture is often hidden (2026-11-21): on the Undead Burg bridge an object at the far end of
// the deck (o1351) and in Blighttown a water piece behind the walls read it every frame, so the pass (142 draws on the
// bridge with DrawMapPieces all, 0.93 ms of the render thread; 0.31 ms with game) was drawn for nothing: the picture
// painted magenta showed nowhere on screen (at New Londo's water it shows at once). So from a reader's second frame in
// a row on, each draw that reads it is counted by an occlusion query of its own (its pixels that pass the depth test,
// none of which is changed), and a frame whose reading draws all came back with nothing counts as one that read
// nothing. The first frame of reads after a frame without any counts as read at once (the pass is drawn from the next
// frame, as before), as does a read inside one of OcclusionCulling's own queries (queries do not nest). Results come
// back a frame or a few later: a reader coming out from behind something reads the kept copy until then, which is
// drawn again every 8th frame while readers are hidden so that it stays close to the current picture.
namespace {

using SetRtFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using ClearFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetTexFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIdxFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIdxUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                     const void*, UINT);
using SetPsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);

SetRtFn o_set_rt = nullptr;
ClearFn o_clear = nullptr;
SetTexFn o_set_tex = nullptr;
DrawFn o_draw = nullptr;
DrawIdxFn o_draw_idx = nullptr;
DrawUpFn o_draw_up = nullptr;
DrawIdxUpFn o_draw_idx_up = nullptr;
SetPsFn o_set_ps = nullptr;

constexpr UINT kPassW = 256, kPassH = 180;
constexpr D3DCOLOR kPassClear = 0x00404040;
constexpr int kStages = 16;
constexpr int kUnusedFramesToSkip = 3;

// The render thread's.
volatile LONG g_mode = 0;                        // 0 off, 1 skip when unused, 2 always skip (development)
volatile LONG g_dbg_color = 0;                   // development: the mirror cleared to this colour too
volatile LONG g_dbg_copy = 0;                    // development: the kept copy painted this colour
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

// The samplers each pixel shader declares (dcl s#): a draw reads the picture only through a stage its shader samples.
// The game leaves the texture bound after the draw that used it, so every later draw of the frame had it bound.
constexpr int kPsSlots = 2048;
struct PsSlot {
    IDirect3DPixelShader9* ps;
    uint32_t mask;
};
PsSlot g_ps_table[kPsSlots] = {};
uint32_t g_ps_mask = 0xFFFFu;  // the current shader's (all until known)

uint32_t sampler_mask(IDirect3DPixelShader9* ps) {
    if (!ps) return 0xFFFFu;  // fixed function: any stage may be read
    uint32_t h = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ps)) >> 4) * 2654435761u >> 21;
    for (int probe = 0; probe < 16; ++probe, h = (h + 1) & (kPsSlots - 1)) {
        if (g_ps_table[h].ps == ps) return g_ps_table[h].mask;
        if (!g_ps_table[h].ps) break;
    }
    uint32_t mask = 0xFFFFu;  // unknown: any
    UINT size = 0;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < 65536 && (size & 3) == 0) {
        static DWORD buf[16384];  // render thread
        if (SUCCEEDED(ps->GetFunction(buf, &size))) {
            const UINT n = size / 4;
            const DWORD ver = buf[0];
            if ((ver & 0xFFFF0000u) == 0xFFFF0000u && ((ver >> 8) & 0xFF) >= 2) {  // ps_2_0 and later
                uint32_t m = 0;
                bool ok = false;
                for (UINT i = 1; i < n;) {
                    const DWORD tok = buf[i];
                    if (tok == 0x0000FFFFu) {
                        ok = true;
                        break;
                    }
                    if ((tok & 0xFFFF) == 0xFFFE) {  // comment
                        i += 1 + ((tok >> 16) & 0x7FFF);
                        continue;
                    }
                    const UINT len = (tok >> 24) & 0x0F;
                    if ((tok & 0xFFFF) == 31 && len >= 2 && i + 2 < n) {  // dcl: usage token, register token
                        const DWORD reg = buf[i + 2];
                        const DWORD type = ((reg >> 28) & 7) | ((reg >> 8) & 0x18);
                        if (type == 10) m |= 1u << (reg & 0x7FF & 15);  // D3DSPR_SAMPLER
                    }
                    i += 1 + len;
                }
                if (ok) mask = m;
            }
        }
    }
    for (int probe = 0; probe < 16; ++probe, h = (h + 1) & (kPsSlots - 1)) {
        if (!g_ps_table[h].ps || g_ps_table[h].ps == ps) {
            g_ps_table[h].ps = ps;
            g_ps_table[h].mask = mask;
            break;
        }
    }
    return mask;
}

// The occlusion queries of the draws that read the picture (see the top). Frames are resolved in order.
constexpr int kQueries = 256;
constexpr int kRecs = 32;
constexpr int kMaxWait = 12;   // frames a result may take before the frame counts as read
constexpr int kRefresh = 8;    // while readers are hidden, every 8th pass is drawn (the kept copy stays current)
IDirect3DQuery9* g_q[kQueries] = {};
bool g_q_failed = false;
int g_q_issued = 0, g_q_done = 0;  // running counts (slot = count % kQueries)
struct Rec {
    int q0, n;
    bool read;  // read at once (not counted by queries)
    int wait;
};
Rec g_recs[kRecs];
int g_rec_head = 0, g_rec_tail = 0;  // running counts
int g_cur_q0 = 0, g_cur_n = 0;
bool g_cur_read = false;
bool g_prev_reads = false;    // the last frame had reads
bool g_hidden_reads = false;  // the latest resolved frame had reads, all hidden
int g_skip_run = 0;
volatile LONG g_vis = 1;      // the queries on (RemoteControl `passcull 1 0 0` off)
volatile LONG g_nomask = 0;   // development: every bound stage counts as read (before 2026-11-21)
volatile LONG g_stat_hidden = 0;
// development counters (RemoteControl `passcull` prints them)
long g_c_first = 0, g_c_occl = 0, g_c_noq = 0, g_c_vis = 0, g_c_hid = 0, g_c_wait = 0, g_c_q = 0;
IDirect3DSurface9* g_q_rt[kQueries] = {};
int g_vis_logged = 0;

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

void resolve();

void end_window() {
    if (!g_window) return;
    g_window = false;
    if (g_rec_tail - g_rec_head >= kRecs) {  // full (results not coming back): the oldest counts as read
        Rec& r = g_recs[g_rec_head % kRecs];
        if (r.n) g_q_done = r.q0 + r.n;
        ++g_rec_head;
        g_unused_frames = 0;
    }
    Rec& r = g_recs[g_rec_tail % kRecs];
    r.q0 = g_cur_q0;
    r.n = g_cur_n;
    r.read = g_cur_read;
    r.wait = 0;
    ++g_rec_tail;
    g_prev_reads = g_used;
    g_cur_n = 0;
    g_cur_read = false;
    g_used = false;
    resolve();
}

IDirect3DQuery9* take_query(IDirect3DDevice9* dev) {
    if (g_q_failed || g_q_issued - g_q_done >= kQueries) return nullptr;
    IDirect3DQuery9*& q = g_q[g_q_issued % kQueries];
    if (!q && FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) {
        q = nullptr;
        g_q_failed = true;
        LOG_INFO("SkipUnusedReflection: no occlusion queries; any draw that reads the mirrored scene keeps it drawn");
        return nullptr;
    }
    if (g_cur_n == 0) g_cur_q0 = g_q_issued;
    ++g_q_issued;
    ++g_cur_n;
    return q;
}

// A draw while the reflection's texture may be read: the query to count it with, if any.
inline IDirect3DQuery9* before_draw(IDirect3DDevice9* dev) {
    if (!g_window || !(g_bound_mask & g_ps_mask)) return nullptr;
    if (!g_used) occl_note_reflection_read(g_bound_mask);
    g_used = true;
    IDirect3DQuery9* q = nullptr;
    if (!g_cur_read) {
        if (g_mode == 1 && g_vis && g_prev_reads && !occl_query_active()) q = take_query(dev);
        if (!q) {
            g_cur_read = true;
            if (!g_prev_reads) {
                ++g_c_first;
            } else if (occl_query_active()) {
                ++g_c_occl;
                occl_note_reflection_read(g_bound_mask | 0x100u);
            } else {
                ++g_c_noq;
            }
        } else {
            ++g_c_q;
            g_q_rt[(g_q_issued - 1) % kQueries] = g_rt0;
        }
    }
    if (!g_window_skipped) return q;
    // The pass was not drawn this frame: the target holds a blur. Our copy of the last drawn picture instead.
    const uint32_t need = g_bound_mask & ~g_sub_mask;
    if (!need) return q;
    if (!g_copy_valid) {
        InterlockedIncrement(&g_stat_stale);
        return q;
    }
    for (int s = 0; s < kStages; ++s) {
        if (need & (1u << s)) o_set_tex(dev, s, g_copy);
    }
    g_sub_mask |= need;
    InterlockedIncrement(&g_stat_stale);
    return q;
}

// The frames whose results are in, in order.
void resolve() {
    while (g_rec_head != g_rec_tail) {
        Rec& r = g_recs[g_rec_head % kRecs];
        bool read = r.read, pending = false;
        for (int i = 0; !read && i < r.n; ++i) {
            DWORD samples = 0;
            const HRESULT hr = g_q[(r.q0 + i) % kQueries]->GetData(&samples, sizeof(samples), 0);
            if (hr == S_FALSE) {
                pending = true;
                break;
            }
            if (FAILED(hr) || samples > 0) {
                read = true;
                if (g_vis_logged < 12) {
                    ++g_vis_logged;
                    D3DSURFACE_DESC d{};
                    IDirect3DSurface9* rt = g_q_rt[(r.q0 + i) % kQueries];
                    if (rt) rt->GetDesc(&d);
                    LOG_INFO("SkipUnusedReflection: a reading draw showed %lu samples (query %d of %d; target %ux%u)",
                             static_cast<unsigned long>(samples), i, r.n, d.Width, d.Height);
                }
            }
        }
        if (pending) {
            if (++r.wait <= kMaxWait) break;
            read = true;
            ++g_c_wait;
        }
        if (r.n && !r.read) {
            if (read) {
                ++g_c_vis;
            } else {
                ++g_c_hid;
            }
        }
        if (r.n) g_q_done = r.q0 + r.n;
        ++g_rec_head;
        if (read) {
            g_unused_frames = 0;
        } else if (g_unused_frames < 1000) {
            ++g_unused_frames;
        }
        g_hidden_reads = !read && r.n > 0;
        if (g_hidden_reads) InterlockedIncrement(&g_stat_hidden);
    }
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
        // With WaterResolution the pass went into a larger target of the resolution code's: copied from that one.
        IDirect3DSurface9* src = resolution_reflection_source(g_pass_surface);
        g_copy_valid = SUCCEEDED(dev->StretchRect(src, nullptr, dst, nullptr,
                                                  src == g_pass_surface ? D3DTEXF_NONE : D3DTEXF_LINEAR));
        if (g_dbg_copy) dev->ColorFill(dst, nullptr, static_cast<D3DCOLOR>(g_dbg_copy));  // development
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
        if (g_skipping && mode == 1 && g_hidden_reads && g_skip_run >= kRefresh - 1) g_skipping = false;
        g_skip_run = g_skipping ? g_skip_run + 1 : 0;
        if (!g_skipping && g_dbg_color) {  // development: the mirror painted one colour (where it shows on screen)
            const HRESULT hr = o_clear(dev, count, rects, flags, color, z, stencil);
            o_clear(dev, 0, nullptr, D3DCLEAR_TARGET, static_cast<D3DCOLOR>(g_dbg_color), z, stencil);
            return hr;
        }
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

HRESULT WINAPI hk_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    if (g_mode != 0) g_ps_mask = g_nomask ? 0xFFFFu : sampler_mask(ps);
    return o_set_ps(dev, ps);
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
    IDirect3DQuery9* q = before_draw(dev);
    if (!q) return o_draw(dev, type, start, count);
    q->Issue(D3DISSUE_BEGIN);
    const HRESULT hr = o_draw(dev, type, start, count);
    q->Issue(D3DISSUE_END);
    return hr;
}

HRESULT WINAPI hk_draw_idx(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT vertices,
                           UINT start, UINT count) {
    if (g_in_pass && g_skipping) return D3D_OK;
    IDirect3DQuery9* q = before_draw(dev);
    if (!q) return o_draw_idx(dev, type, base, min_index, vertices, start, count);
    q->Issue(D3DISSUE_BEGIN);
    const HRESULT hr = o_draw_idx(dev, type, base, min_index, vertices, start, count);
    q->Issue(D3DISSUE_END);
    return hr;
}

HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    if (g_in_pass && g_skipping) return D3D_OK;
    IDirect3DQuery9* q = before_draw(dev);
    if (!q) return o_draw_up(dev, type, count, data, stride);
    q->Issue(D3DISSUE_BEGIN);
    const HRESULT hr = o_draw_up(dev, type, count, data, stride);
    q->Issue(D3DISSUE_END);
    return hr;
}

HRESULT WINAPI hk_draw_idx_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count,
                              const void* indices, D3DFORMAT format, const void* data, UINT stride) {
    if (g_in_pass && g_skipping) return D3D_OK;
    IDirect3DQuery9* q = before_draw(dev);
    if (!q) return o_draw_idx_up(dev, type, min_index, vertices, count, indices, format, data, stride);
    q->Issue(D3DISSUE_BEGIN);
    const HRESULT hr = o_draw_idx_up(dev, type, min_index, vertices, count, indices, format, data, stride);
    q->Issue(D3DISSUE_END);
    return hr;
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
    hook(dev, 107, &hk_set_ps, o_set_ps);
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
    for (auto& q : g_q) {
        if (q) q->Release();
        q = nullptr;
    }
    for (auto& e : g_ps_table) e = PsSlot{};
    g_ps_mask = 0xFFFFu;
    g_q_issued = g_q_done = 0;
    g_rec_head = g_rec_tail = 0;
    g_cur_n = 0;
    g_cur_read = g_prev_reads = g_hidden_reads = false;
    g_skip_run = 0;
}

IDirect3DTexture9* passcull_copy() { return g_copy_valid ? g_copy : nullptr; }

int passcull_unused_frames() { return g_unused_frames; }
void passcull_counters(long out[7]) {
    out[0] = g_c_first; out[1] = g_c_occl; out[2] = g_c_noq; out[3] = g_c_q; out[4] = g_c_vis; out[5] = g_c_hid;
    out[6] = g_c_wait;
    g_vis_logged = 0;
}

bool passcull_skipping() { return g_in_pass && g_skipping; }
void passcull_debug_color(unsigned long color) { InterlockedExchange(&g_dbg_color, static_cast<LONG>(color)); }
void passcull_set_queries(int on) { InterlockedExchange(&g_vis, on ? 1 : 0); }
void passcull_set_nomask(int on) { InterlockedExchange(&g_nomask, on ? 1 : 0); }
void passcull_debug_copy(unsigned long color) { InterlockedExchange(&g_dbg_copy, static_cast<LONG>(color)); }
long passcull_take_hidden() { return InterlockedExchange(&g_stat_hidden, 0); }

void passcull_take_stats(long* skipped, long* drawn, long* stale) {
    *skipped = InterlockedExchange(&g_stat_skipped, 0);
    *drawn = InterlockedExchange(&g_stat_drawn, 0);
    *stale = InterlockedExchange(&g_stat_stale, 0);
}
