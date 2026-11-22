#include "depthrev.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "log.h"


// See depthrev.h.
namespace {

constexpr DWORD kEnd = 0x0000FFFF;
constexpr int kCreateTextureSlot = 23, kCreateDepthSlot = 29, kClearSlot = 43, kSetTransformSlot = 44,
              kGetTransformSlot = 45, kSetViewportSlot = 47, kGetViewportSlot = 48, kSetRenderStateSlot = 57,
              kGetRenderStateSlot = 58, kSetTextureSlot = 65, kDrawPrimitiveSlot = 81, kDrawIndexedSlot = 82,
              kDrawUpSlot = 83, kDrawIndexedUpSlot = 84, kCreateVsSlot = 91, kCreatePsSlot = 106, kSetPsSlot = 107;

bool g_on = false;
bool g_hooked = false;


using CreateTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DTexture9**, HANDLE*);
using CreateDepthFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                       IDirect3DSurface9**, HANDLE*);
using ClearFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetTransformFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using GetTransformFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
using SetViewportFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using GetViewportFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DVIEWPORT9*);
using SetRenderStateFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using GetRenderStateFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD*);
using SetTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawPrimitiveFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedUpFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                         const void*, UINT);
using CreateVsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using CreatePsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);

CreateTextureFn o_create_texture = nullptr;
CreateDepthFn o_create_depth = nullptr;
ClearFn o_clear = nullptr;
SetTransformFn o_set_transform = nullptr;
GetTransformFn o_get_transform = nullptr;
SetViewportFn o_set_viewport = nullptr;
GetViewportFn o_get_viewport = nullptr;
SetRenderStateFn o_set_rs = nullptr;
GetRenderStateFn o_get_rs = nullptr;
SetTextureFn o_set_texture = nullptr;
DrawPrimitiveFn o_draw = nullptr;
DrawIndexedFn o_draw_indexed = nullptr;
DrawUpFn o_draw_up = nullptr;
DrawIndexedUpFn o_draw_indexed_up = nullptr;
CreateVsFn o_create_vs = nullptr;
CreatePsFn o_create_ps = nullptr;
SetPsFn o_set_ps = nullptr;

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

bool is_depth_format(D3DFORMAT f) { return f == D3DFMT_D24S8 || f == D3DFMT_D24X8 || f == D3DFMT_D16; }

// ---- shader tokens ----
inline DWORD reg_type(DWORD p) { return ((p >> 28) & 7) | ((p >> 8) & 0x18); }
inline DWORD reg_num(DWORD p) { return p & 0x7FF; }
inline DWORD type_bits(DWORD type) { return ((type & 7) << 28) | ((type & 0x18) << 8); }
inline DWORD dst_tok(DWORD type, DWORD num, DWORD mask) { return 0x80000000u | type_bits(type) | (mask << 16) | num; }
inline DWORD src_tok(DWORD type, DWORD num, DWORD swz, DWORD mod) {
    return 0x80000000u | type_bits(type) | (swz << 16) | (mod << 24) | num;
}
constexpr DWORD kTemp = 0, kConst = 2, kRastout = 4, kOutput = 6, kDepthOut = 9, kSampler = 10;

// Opcodes whose first parameter is not a register written.
bool no_dst(DWORD op) {
    switch (op) {
        case 0: case 25: case 26: case 27: case 28: case 29: case 30: case 38: case 39: case 40: case 41: case 42:
        case 43: case 44: case 45: case 65: case 96:
            return true;
        default:
            return false;
    }
}

struct Inst {
    size_t at;   // index of the instruction token
    DWORD op;
    DWORD len;   // parameter tokens
};

// The instructions of a shader model 2/3 shader (comments skipped), or false.
bool parse(const DWORD* code, size_t n, std::vector<Inst>& out, size_t& end_at) {
    out.clear();
    end_at = 0;
    for (size_t i = 1; i < n;) {
        const DWORD tok = code[i];
        if (tok == kEnd) {
            end_at = i;
            return true;
        }
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if (i + len >= n) return false;
        out.push_back({i, op, len});
        i += 1 + len;
    }
    return false;
}

// Where code that has to run last in the main program goes: before its first ret or label at nesting depth 0 (the
// subroutines follow it), or before the end.
size_t main_tail(const std::vector<Inst>& ins, size_t end_at) {
    int depth = 0;
    for (const Inst& in : ins) {
        if (in.op == 40 || in.op == 41 || in.op == 27 || in.op == 38) ++depth;
        if (in.op == 43 || in.op == 29 || in.op == 39) --depth;
        if (depth == 0 && (in.op == 28 || in.op == 30)) return in.at;
    }
    return end_at;
}

int max_temp(const DWORD* code, const std::vector<Inst>& ins) {
    int m = -1;
    for (const Inst& in : ins) {
        if (in.op == 31 || in.op == 81 || in.op == 47 || in.op == 48) continue;  // dcl, def*
        for (DWORD k = 1; k <= in.len; ++k) {
            const DWORD p = code[in.at + k];
            if (!(p & 0x80000000u)) continue;
            if (reg_type(p) == kTemp && static_cast<int>(reg_num(p)) > m) m = static_cast<int>(reg_num(p));
        }
    }
    return m;
}

// A float constant register the shader neither reads nor defines (from the top of its range down), or -1.
int free_const(const DWORD* code, const std::vector<Inst>& ins, int limit) {
    std::vector<bool> used(static_cast<size_t>(limit), false);
    for (const Inst& in : ins) {
        for (DWORD k = 1; k <= in.len; ++k) {
            const DWORD p = code[in.at + k];
            if (!(p & 0x80000000u)) continue;
            if (reg_type(p) == kConst && static_cast<int>(reg_num(p)) < limit) used[reg_num(p)] = true;
            if (p & 0x2000) return -1;  // relative addressing: any constant may be read
        }
    }
    for (int c = limit - 1; c >= 0; --c) {
        if (!used[static_cast<size_t>(c)]) return c;
    }
    return -1;
}

void put_def_one(std::vector<DWORD>& out, int c) {
    const float one = 1.0f;
    DWORD f = 0;
    std::memcpy(&f, &one, 4);
    out.push_back(0x05000051u);  // def
    out.push_back(dst_tok(kConst, static_cast<DWORD>(c), 0xF));
    for (int k = 0; k < 4; ++k) out.push_back(f);
}

// Vertex shader: every write of the position output goes to a spare temporary, and at the end of the main program the
// position is written from it with z = w - z.
bool patch_vs(const DWORD* code, size_t n, std::vector<DWORD>& out) {
    const DWORD ver = code[0];
    const DWORD major = (ver >> 8) & 0xFF;
    if ((ver & 0xFFFF0000u) != 0xFFFE0000u || major < 2) return false;
    std::vector<Inst> ins;
    size_t end_at = 0;
    if (!parse(code, n, ins, end_at)) return false;
    DWORD pos_type = kRastout, pos_num = 0;
    if (major >= 3) {
        bool found = false;
        for (const Inst& in : ins) {
            if (in.op != 31 || in.len < 2) continue;
            const DWORD usage = code[in.at + 1];
            const DWORD reg = code[in.at + 2];
            if ((usage & 0x1F) == 0 /* POSITION */ && ((usage >> 16) & 0xF) == 0 && reg_type(reg) == kOutput) {
                pos_type = kOutput;
                pos_num = reg_num(reg);
                found = true;
            }
        }
        if (!found) return false;
    }
    const int t = max_temp(code, ins) + 1;
    if (t >= (major >= 3 ? 32 : 12)) return false;
    const size_t tail = main_tail(ins, end_at);
    out.clear();
    out.reserve(n + 16);
    int writes = 0;
    size_t next = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i == tail) {
            out.push_back(0x02000001u);  // mov pos.xyw, rT
            out.push_back(dst_tok(pos_type, pos_num, 0xB));
            out.push_back(src_tok(kTemp, static_cast<DWORD>(t), 0xE4, 0));
            out.push_back(0x03000002u);  // add pos.z, rT.w, -rT.z
            out.push_back(dst_tok(pos_type, pos_num, 0x4));
            out.push_back(src_tok(kTemp, static_cast<DWORD>(t), 0xFF, 0));
            out.push_back(src_tok(kTemp, static_cast<DWORD>(t), 0xAA, 1));
        }
        DWORD tok = code[i];
        while (next < ins.size() && ins[next].at + 1 < i) ++next;
        if (next < ins.size() && i == ins[next].at + 1 && !no_dst(ins[next].op) && ins[next].op != 31 &&
            ins[next].op != 81 && ins[next].op != 47 && ins[next].op != 48 && ins[next].len >= 1 &&
            reg_type(tok) == pos_type && reg_num(tok) == pos_num && !(tok & 0x2000)) {
            tok = 0x80000000u | (tok & 0x00FF0000u) | type_bits(kTemp) | static_cast<DWORD>(t);
            ++writes;
        }
        out.push_back(tok);
    }
    return writes > 0;
}

// Pixel shader writing depth: the writes go to a spare temporary, and the end writes oDepth = 1 - it.
bool patch_ps_depth(const DWORD* code, size_t n, std::vector<DWORD>& out) {
    const DWORD ver = code[0];
    const DWORD major = (ver >> 8) & 0xFF;
    if ((ver & 0xFFFF0000u) != 0xFFFF0000u || major < 2) return false;
    std::vector<Inst> ins;
    size_t end_at = 0;
    if (!parse(code, n, ins, end_at)) return false;
    DWORD depth_dst = 0;
    for (const Inst& in : ins) {
        if (no_dst(in.op) || in.op == 31 || in.op == 81 || in.len < 1) continue;
        const DWORD p = code[in.at + 1];
        if (reg_type(p) == kDepthOut) depth_dst = p;
    }
    if (!depth_dst) return false;
    const int t = max_temp(code, ins) + 1;
    const int c = free_const(code, ins, major >= 3 ? 224 : 32);
    if (t >= 32 || c < 0) return false;
    const size_t tail = main_tail(ins, end_at);
    out.clear();
    out.reserve(n + 16);
    out.push_back(code[0]);
    put_def_one(out, c);
    size_t next = 0;
    for (size_t i = 1; i < n; ++i) {
        if (i == tail) {
            out.push_back(0x03000002u);  // add oDepth, c.x, -rT.x
            out.push_back(depth_dst);
            out.push_back(src_tok(kConst, static_cast<DWORD>(c), 0x00, 0));
            out.push_back(src_tok(kTemp, static_cast<DWORD>(t), 0x00, 1));
        }
        DWORD tok = code[i];
        while (next < ins.size() && ins[next].at + 1 < i) ++next;
        if (next < ins.size() && i == ins[next].at + 1 && !no_dst(ins[next].op) && ins[next].op != 31 &&
            ins[next].op != 81 && reg_type(tok) == kDepthOut) {
            tok = 0x80000000u | (tok & 0x00FF0000u) | type_bits(kTemp) | static_cast<DWORD>(t);
        }
        out.push_back(tok);
    }
    return true;
}

// Pixel shader copy whose samples from the samplers in `mask` read 1 - the value (a depth texture holds reversed
// depth now; the shader expects the game's).
bool patch_ps_invert(const DWORD* code, size_t n, uint32_t mask, std::vector<DWORD>& out) {
    const DWORD ver = code[0];
    const DWORD major = (ver >> 8) & 0xFF;
    if ((ver & 0xFFFF0000u) != 0xFFFF0000u || major < 2) return false;
    std::vector<Inst> ins;
    size_t end_at = 0;
    if (!parse(code, n, ins, end_at)) return false;
    const int c = free_const(code, ins, major >= 3 ? 224 : 32);
    if (c < 0) return false;
    out.clear();
    out.reserve(n + 32);
    out.push_back(code[0]);
    put_def_one(out, c);
    int done = 0;
    size_t i = 1;
    for (const Inst& in : ins) {
        while (i <= in.at + in.len) out.push_back(code[i++]);
        // texld (66), texldd (93), texldl (95): dst, coordinate, sampler
        if ((in.op == 66 || in.op == 93 || in.op == 95) && in.len >= 3) {
            const DWORD dst = code[in.at + 1];
            const DWORD smp = code[in.at + 3];
            if (reg_type(smp) == kSampler && reg_num(smp) < 32 && (mask >> reg_num(smp)) & 1 && reg_type(dst) == kTemp) {
                out.push_back(0x03000002u);  // add rD.mask, c, -rD
                out.push_back(dst);
                out.push_back(src_tok(kConst, static_cast<DWORD>(c), 0xE4, 0));
                out.push_back(src_tok(kTemp, reg_num(dst), 0xE4, 1));
                ++done;
            }
        }
    }
    while (i < n) out.push_back(code[i++]);
    return done > 0;
}

size_t shader_size(const DWORD* code) {
    size_t i = 1;
    while (i < 65536) {
        const DWORD tok = code[i];
        if (tok == kEnd) return i + 1;
        const DWORD o = tok & 0xFFFF;
        i += 1 + (o == 0xFFFE ? ((tok >> 16) & 0x7FFF) : ((tok >> 24) & 0xF));
    }
    return 0;
}

// The samplers a pixel shader declares (dcl s#), bit per sampler.
uint32_t samplers_of(const DWORD* code, size_t n) {
    std::vector<Inst> ins;
    size_t end_at = 0;
    if (!parse(code, n, ins, end_at)) return 0;
    uint32_t m = 0;
    for (const Inst& in : ins) {
        if (in.op == 31 && in.len >= 2) {
            const DWORD reg = code[in.at + 2];
            if (reg_type(reg) == kSampler && reg_num(reg) < 32) m |= 1u << reg_num(reg);
        }
    }
    return m;
}

// ---- state ----
SRWLOCK g_lock = SRWLOCK_INIT;
struct PsInfo {
    std::vector<DWORD> code;
    uint32_t samplers = 0;
    std::unordered_map<uint32_t, IDirect3DPixelShader9*> variants;
};
std::unordered_map<IDirect3DPixelShader9*, PsInfo> g_ps;
// The depth textures made (a few: the shadow map's, the scene's copies, the low-resolution effects'), read without a
// lock on every SetTexture: a pointer found here is checked for a depth format before it counts.
constexpr int kMaxDepthTextures = 64;
IDirect3DBaseTexture9* volatile g_depth_textures[kMaxDepthTextures] = {};
volatile LONG g_depth_texture_n = 0;
IDirect3DPixelShader9* g_cur_ps = nullptr;
uint32_t g_depth_bound = 0;  // stages with a depth texture bound
D3DMATRIX g_proj{};          // the projection last set (the game's)
bool g_proj_set = false;
std::atomic<long> g_vs_patched{0}, g_vs_failed{0}, g_ps_depth{0}, g_variants{0}, g_formats{0};

DWORD mirror_cmp(DWORD f) {
    switch (f) {
        case D3DCMP_LESS: return D3DCMP_GREATER;
        case D3DCMP_LESSEQUAL: return D3DCMP_GREATEREQUAL;
        case D3DCMP_GREATER: return D3DCMP_LESS;
        case D3DCMP_GREATEREQUAL: return D3DCMP_LESSEQUAL;
        default: return f;
    }
}
DWORD negate_float(DWORD v) {
    float f = 0.0f;
    std::memcpy(&f, &v, 4);
    f = -f;
    std::memcpy(&v, &f, 4);
    return v;
}

HRESULT WINAPI hk_create_texture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                 D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    const bool depth = (usage & D3DUSAGE_DEPTHSTENCIL) && is_depth_format(fmt);
    if (depth) ++g_formats;
    const HRESULT hr = o_create_texture(dev, w, h, levels, usage, fmt, pool, out, shared);
    if (depth && SUCCEEDED(hr) && out && *out) {
        const LONG k = InterlockedIncrement(&g_depth_texture_n) - 1;
        if (k < kMaxDepthTextures) {
            g_depth_textures[k] = *out;
        } else {
            g_depth_textures[k % kMaxDepthTextures] = *out;  // many made over time: the oldest slots reused
        }
    }
    return hr;
}

HRESULT WINAPI hk_create_depth(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q,
                               BOOL discard, IDirect3DSurface9** out, HANDLE* shared) {
    if (is_depth_format(fmt)) ++g_formats;
    return o_create_depth(dev, w, h, fmt, ms, q, discard, out, shared);
}

void log_counts();

HRESULT WINAPI hk_clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                        DWORD stencil) {
    if (flags & D3DCLEAR_ZBUFFER) z = 1.0f - z;
    log_counts();
    return o_clear(dev, count, rects, flags, color, z, stencil);
}

HRESULT WINAPI hk_set_transform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
    if (state == D3DTS_PROJECTION && m) {
        g_proj = *m;
        g_proj_set = true;
        D3DMATRIX r = *m;
        for (int row = 0; row < 4; ++row) r.m[row][2] = m->m[row][3] - m->m[row][2];
        return o_set_transform(dev, state, &r);
    }
    return o_set_transform(dev, state, m);
}

HRESULT WINAPI hk_get_transform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, D3DMATRIX* m) {
    if (state == D3DTS_PROJECTION && m && g_proj_set) {
        *m = g_proj;
        return D3D_OK;
    }
    return o_get_transform(dev, state, m);
}

HRESULT WINAPI hk_set_viewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp) {
    if (!vp) return o_set_viewport(dev, vp);
    D3DVIEWPORT9 r = *vp;
    r.MinZ = 1.0f - vp->MaxZ;
    r.MaxZ = 1.0f - vp->MinZ;
    return o_set_viewport(dev, &r);
}

HRESULT WINAPI hk_get_viewport(IDirect3DDevice9* dev, D3DVIEWPORT9* vp) {
    const HRESULT hr = o_get_viewport(dev, vp);
    if (SUCCEEDED(hr) && vp) {
        const float mn = vp->MinZ, mx = vp->MaxZ;
        vp->MinZ = 1.0f - mx;
        vp->MaxZ = 1.0f - mn;
    }
    return hr;
}

HRESULT WINAPI hk_set_rs(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD v) {
    if (s == D3DRS_ZFUNC) v = mirror_cmp(v);
    else if (s == D3DRS_DEPTHBIAS || s == D3DRS_SLOPESCALEDEPTHBIAS) v = negate_float(v);
    return o_set_rs(dev, s, v);
}

HRESULT WINAPI hk_get_rs(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD* v) {
    const HRESULT hr = o_get_rs(dev, s, v);
    if (SUCCEEDED(hr) && v) {
        if (s == D3DRS_ZFUNC) *v = mirror_cmp(*v);
        else if (s == D3DRS_DEPTHBIAS || s == D3DRS_SLOPESCALEDEPTHBIAS) *v = negate_float(*v);
    }
    return hr;
}

HRESULT WINAPI hk_set_texture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
    if (stage < 16) {
        bool depth = false;
        if (tex) {
            LONG n = g_depth_texture_n;
            if (n > kMaxDepthTextures) n = kMaxDepthTextures;
            for (LONG k = 0; k < n && !depth; ++k) depth = g_depth_textures[k] == tex;
            if (depth && tex->GetType() == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC d{};
                depth = SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d)) && is_depth_format(d.Format);
            }
        }
        if (depth) {
            g_depth_bound |= 1u << stage;
        } else {
            g_depth_bound &= ~(1u << stage);
        }
    }
    return o_set_texture(dev, stage, tex);
}

bool safe_patch_vs(const DWORD* code, size_t n, std::vector<DWORD>& out) {
    __try {
        return n && patch_vs(code, n, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safe_patch_invert(const DWORD* code, size_t n, uint32_t mask, std::vector<DWORD>& out) {
    __try {
        return patch_ps_invert(code, n, mask, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT WINAPI hk_create_vs(IDirect3DDevice9* dev, const DWORD* code, IDirect3DVertexShader9** out) {
    if (code) {
        const size_t n = shader_size(code);
        std::vector<DWORD> patched;
        const bool ok = safe_patch_vs(code, n, patched);
        if (ok && SUCCEEDED(o_create_vs(dev, patched.data(), out))) {
            ++g_vs_patched;
            return D3D_OK;
        }
        if (++g_vs_failed <= 20) {
            LOG_ERROR("ReversedDepth: a vertex shader (version %08lX, %u tokens) could not be rewritten; drawn with "
                      "the game's depth (wrong in front of or behind others)", code[0], static_cast<unsigned>(n));
        }
    }
    return o_create_vs(dev, code, out);
}

bool make_ps_depth(const DWORD* code, size_t n, std::vector<DWORD>& out) {
    __try {
        return patch_ps_depth(code, n, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT WINAPI hk_create_ps(IDirect3DDevice9* dev, const DWORD* code, IDirect3DPixelShader9** out) {
    if (!code) return o_create_ps(dev, code, out);
    const size_t n = shader_size(code);
    std::vector<DWORD> patched;
    const bool depth = n && make_ps_depth(code, n, patched);
    const DWORD* use = depth ? patched.data() : code;
    const size_t use_n = depth ? patched.size() : n;
    const HRESULT hr = o_create_ps(dev, use, out);
    if (SUCCEEDED(hr) && out && *out && use_n) {
        if (depth) ++g_ps_depth;
        PsInfo info;
        info.code.assign(use, use + use_n);
        info.samplers = samplers_of(use, use_n);
        AcquireSRWLockExclusive(&g_lock);
        g_ps[*out] = std::move(info);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return hr;
}

HRESULT WINAPI hk_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    g_cur_ps = ps;
    return o_set_ps(dev, ps);
}

// The pixel shader to draw with while depth textures are bound: a copy reading 1 - depth from them (made once per
// shader and set of samplers), or null for the shader as it is.
IDirect3DPixelShader9* variant_for(IDirect3DDevice9* dev) {
    IDirect3DPixelShader9* ps = g_cur_ps;
    if (!ps) return nullptr;
    AcquireSRWLockExclusive(&g_lock);
    IDirect3DPixelShader9* v = nullptr;
    auto it = g_ps.find(ps);
    static int unknown_logged = 0;
    if (it == g_ps.end() && unknown_logged < 5) {
        ++unknown_logged;
        LOG_INFO("ReversedDepth: a pixel shader %p not seen created is drawn with depth textures on %08X",
                 static_cast<void*>(ps), g_depth_bound);
    }
    if (it != g_ps.end()) {
        const uint32_t m = it->second.samplers & g_depth_bound;
        if (m) {
            auto vi = it->second.variants.find(m);
            if (vi != it->second.variants.end()) {
                v = vi->second;
            } else {
                std::vector<DWORD> code;
                const bool ok = safe_patch_invert(it->second.code.data(), it->second.code.size(), m, code);
                if (ok && SUCCEEDED(o_create_ps(dev, code.data(), &v)) && v) {
                    ++g_variants;
                } else {
                    v = nullptr;
                }
                LOG_INFO("ReversedDepth: pixel shader %p (%u tokens, samplers %08X) drawn with depth textures on %08X: "
                         "copy %s", static_cast<void*>(ps), static_cast<unsigned>(it->second.code.size()),
                         it->second.samplers, m, v ? "made" : (ok ? "refused by the device" : "NOT made"));
                it->second.variants[m] = v;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return v;
}

template <typename F>
HRESULT with_variant(IDirect3DDevice9* dev, F draw) {
    IDirect3DPixelShader9* v = g_depth_bound ? variant_for(dev) : nullptr;
    if (!v) return draw();
    IDirect3DPixelShader9* keep = g_cur_ps;
    o_set_ps(dev, v);
    const HRESULT hr = draw();
    o_set_ps(dev, keep);
    return hr;
}

HRESULT WINAPI hk_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT count) {
    if (!g_depth_bound) return o_draw(dev, t, start, count);
    return with_variant(dev, [&] { return o_draw(dev, t, start, count); });
}
HRESULT WINAPI hk_draw_indexed(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT mn, UINT nv, UINT start,
                               UINT count) {
    if (!g_depth_bound) return o_draw_indexed(dev, t, base, mn, nv, start, count);
    return with_variant(dev, [&] { return o_draw_indexed(dev, t, base, mn, nv, start, count); });
}
HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) {
    if (!g_depth_bound) return o_draw_up(dev, t, count, data, stride);
    return with_variant(dev, [&] { return o_draw_up(dev, t, count, data, stride); });
}
HRESULT WINAPI hk_draw_indexed_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT mn, UINT nv, UINT count,
                                  const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
    if (!g_depth_bound) return o_draw_indexed_up(dev, t, mn, nv, count, idx, fmt, data, stride);
    return with_variant(dev, [&] { return o_draw_indexed_up(dev, t, mn, nv, count, idx, fmt, data, stride); });
}

template <typename T>
void hook(IDirect3DDevice9* dev, int slot, T replacement, T& original) {
    void* p = patch_slot(dev, slot, reinterpret_cast<void*>(replacement));
    if (p && !original) original = reinterpret_cast<T>(p);
}

ULONGLONG g_next_log = 0;

void log_counts() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_log) return;
    g_next_log = now + 30000;
    LOG_INFO("ReversedDepth: %ld vertex shaders rewritten (%ld not), %ld depth-writing pixel shaders, %ld depth-reading "
             "copies, %ld depth buffers made float", g_vs_patched.load(), g_vs_failed.load(), g_ps_depth.load(),
             g_variants.load(), g_formats.load());
}

}  // namespace

void depthrev_adjust_present(D3DPRESENT_PARAMETERS*, bool) {}  // the formats stay the game's (see d3d.cpp)

bool depthrev_active() { return g_on && g_hooked; }

void depthrev_hook_device(IDirect3DDevice9* dev, bool enabled) {
    if (!enabled || !dev || g_hooked) return;
    hook(dev, kCreateTextureSlot, &hk_create_texture, o_create_texture);
    hook(dev, kCreateDepthSlot, &hk_create_depth, o_create_depth);
    hook(dev, kClearSlot, &hk_clear, o_clear);
    hook(dev, kSetTransformSlot, &hk_set_transform, o_set_transform);
    hook(dev, kGetTransformSlot, &hk_get_transform, o_get_transform);
    hook(dev, kSetViewportSlot, &hk_set_viewport, o_set_viewport);
    hook(dev, kGetViewportSlot, &hk_get_viewport, o_get_viewport);
    hook(dev, kSetRenderStateSlot, &hk_set_rs, o_set_rs);
    hook(dev, kGetRenderStateSlot, &hk_get_rs, o_get_rs);
    hook(dev, kSetTextureSlot, &hk_set_texture, o_set_texture);
    hook(dev, kDrawPrimitiveSlot, &hk_draw, o_draw);
    hook(dev, kDrawIndexedSlot, &hk_draw_indexed, o_draw_indexed);
    hook(dev, kDrawUpSlot, &hk_draw_up, o_draw_up);
    hook(dev, kDrawIndexedUpSlot, &hk_draw_indexed_up, o_draw_indexed_up);
    hook(dev, kCreateVsSlot, &hk_create_vs, o_create_vs);
    hook(dev, kCreatePsSlot, &hk_create_ps, o_create_ps);
    hook(dev, kSetPsSlot, &hk_set_ps, o_set_ps);
    g_hooked = true;
    g_on = true;
    // The game's state as the device holds it when the hooks go in: the default compare (LESSEQUAL) mirrored.
    o_set_rs(dev, D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
    LOG_INFO("ReversedDepth (experimental): depth reversed (far 0, near 1); DXVK keeps the depth buffers in 32-bit "
             "float (d3d9.useD32forD24)");
}
