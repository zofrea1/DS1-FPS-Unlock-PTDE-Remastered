#include "remote.h"

#include "fpsu_buildcfg.h"

#if FPSU_DIAGNOSTICS

#include "limiter.h"
#include "profile.h"
#include "atoc.h"
#include "resolution.h"
#include "watch.h"
#include "log.h"

#include <d3d9.h>

#include <intrin.h>

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Device method slots (IDirect3DDevice9) and query slots (IDirect3DQuery9).
enum Slot {
    kPresent = 17, kCreateTexture = 23, kCreateRenderTarget = 28, kCreateDepth = 29, kGetRTData = 32,
    kStretchRect = 34, kColorFill = 35, kSetRenderTarget = 37, kSetDepth = 39, kClear = 43, kSetViewport = 47,
    kSetRenderState = 57, kSetTexture = 65, kSetScissor = 75, kDrawPrim = 81, kDrawIdxPrim = 82, kDrawPrimUP = 83,
    kDrawIdxPrimUP = 84, kCreateVS = 91, kSetVS = 92, kSetVSConstF = 94, kCreatePS = 106, kSetPS = 107, kSetPSConstF = 109,
    kCreateQuery = 118,
};
constexpr int kQueryIssue = 6, kQueryGetData = 7;

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using CreateTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DTexture9**, HANDLE*);
using CreateSurfFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                      IDirect3DSurface9**, HANDLE*);
using GetRTDataFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
using StretchRectFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
                                       const RECT*, D3DTEXTUREFILTERTYPE);
using ColorFillFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
using SetRenderTargetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepthFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*);
using ClearFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetViewportFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetRenderStateFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SetTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetScissorFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*);
using DrawPrimFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIdxPrimFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimUPFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIdxPrimUPFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                         D3DFORMAT, const void*, UINT);
using CreateShaderFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, void**);
using SetShaderFn = HRESULT(WINAPI*)(IDirect3DDevice9*, void*);
using SetConstFFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using CreateQueryFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
using QueryIssueFn = HRESULT(WINAPI*)(IDirect3DQuery9*, DWORD);
using QueryGetDataFn = HRESULT(WINAPI*)(IDirect3DQuery9*, void*, DWORD, DWORD);

PresentFn o_present;
CreateTextureFn o_create_texture;
CreateSurfFn o_create_rt, o_create_depth;
GetRTDataFn o_get_rt_data;
StretchRectFn o_stretch;
ColorFillFn o_color_fill;
SetRenderTargetFn o_set_rt;
SetDepthFn o_set_depth;
ClearFn o_clear;
SetViewportFn o_set_viewport;
SetRenderStateFn o_set_rs;
SetTextureFn o_set_texture;
SetScissorFn o_set_scissor;
DrawPrimFn o_draw;
DrawIdxPrimFn o_draw_idx;
DrawPrimUPFn o_draw_up;
DrawIdxPrimUPFn o_draw_idx_up;
CreateShaderFn o_create_vs, o_create_ps;
SetShaderFn o_set_vs, o_set_ps;
SetConstFFn o_set_ps_const, o_set_vs_const;
CreateQueryFn o_create_query;
QueryIssueFn o_query_issue;
QueryGetDataFn o_query_get_data;

// D3DX from the game's own d3dx9_43.dll.
struct XBuffer {
    virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0;
    virtual ULONG __stdcall AddRef() = 0;
    virtual ULONG __stdcall Release() = 0;
    virtual void* __stdcall GetBufferPointer() = 0;
    virtual DWORD __stdcall GetBufferSize() = 0;
};
using SaveSurfaceFn = HRESULT(WINAPI*)(const wchar_t*, int, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
using DisassembleFn = HRESULT(WINAPI*)(const DWORD*, BOOL, const char*, XBuffer**);
constexpr int kPng = 3;
SaveSurfaceFn x_save_surface;
DisassembleFn x_disassemble;

// ---------------------------------------------------------------------------------------------------------------
bool g_on = false;
wchar_t g_dir[MAX_PATH];
ULONGLONG g_t0 = 0;
std::mutex g_out_lock;

void out(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_out_lock);
    wchar_t path[MAX_PATH];
    std::swprintf(path, MAX_PATH, L"%s\\fpsu-remote.txt", g_dir);
    FILE* f = _wfopen(path, L"a");
    if (f) {
        std::fprintf(f, "%8.3f %s\n", (GetTickCount64() - g_t0) / 1000.0, line);
        std::fclose(f);
    }
}

std::wstring widen(const std::string& s) {
    std::wstring w;
    for (char c : s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return w;
}

std::wstring shot_path(const std::string& name, const wchar_t* ext) {
    std::wstring dir = std::wstring(g_dir) + L"\\fpsu-shots";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + widen(name) + ext;
}

// ---------------------------------------------------------------------------------------------------------------
// Controller injection.
struct XState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
std::mutex g_pad_lock;
WORD g_pad_buttons = 0;
BYTE g_pad_lt = 0, g_pad_rt = 0;
ULONGLONG g_pad_until = 0;
SHORT g_stick[4] = {};
ULONGLONG g_stick_until = 0;
DWORD g_packet = 0;

WORD parse_buttons(const std::string& s) {
    static const struct { const char* name; WORD bit; } names[] = {
        {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010}, {"BACK", 0x0020},
        {"LS", 0x0040}, {"RS", 0x0080}, {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000},
        {"X", 0x4000}, {"Y", 0x8000},
    };
    WORD bits = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('+', i);
        if (j == std::string::npos) j = s.size();
        std::string part = s.substr(i, j - i);
        for (auto& c : part) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        for (const auto& n : names) {
            if (part == n.name) bits |= n.bit;
        }
        i = j + 1;
    }
    return bits;
}

// ---------------------------------------------------------------------------------------------------------------
// Resource and shader names.
struct Res {
    int id;
    char kind;  // 'T' texture, 'R' render target surface, 'D' depth surface
    UINT w, h;
    D3DFORMAT fmt;
    DWORD usage;
};
std::mutex g_res_lock;
std::unordered_map<void*, Res> g_res;
std::unordered_map<void*, std::string> g_shaders;
std::unordered_map<void*, int> g_queries;
int g_next_res = 1, g_next_query = 1;

const char* fmt_name(D3DFORMAT f, char* buf) {
    switch (f) {
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_R5G6B5: return "R5G6B5";
        case D3DFMT_A2R10G10B10: return "A2R10G10B10";
        case D3DFMT_A2B10G10R10: return "A2B10G10R10";
        case D3DFMT_A16B16G16R16F: return "RGBA16F";
        case D3DFMT_A32B32G32R32F: return "RGBA32F";
        case D3DFMT_R16F: return "R16F";
        case D3DFMT_G16R16F: return "RG16F";
        case D3DFMT_R32F: return "R32F";
        case D3DFMT_G32R32F: return "RG32F";
        case D3DFMT_A16B16G16R16: return "RGBA16";
        case D3DFMT_G16R16: return "RG16";
        case D3DFMT_D24S8: return "D24S8";
        case D3DFMT_D24X8: return "D24X8";
        case D3DFMT_D16: return "D16";
        case D3DFMT_D32: return "D32";
        case D3DFMT_A8: return "A8";
        case D3DFMT_L8: return "L8";
        default: break;
    }
    const DWORD v = static_cast<DWORD>(f);
    if (v > 0xFFFF) {
        std::snprintf(buf, 16, "'%c%c%c%c'", static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                      static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF));
    } else {
        std::snprintf(buf, 16, "fmt%lu", v);
    }
    return buf;
}

std::string res_text(void* p) {
    if (!p) return "null";
    char buf[96], fb[16];
    {
        std::lock_guard<std::mutex> lock(g_res_lock);
        auto it = g_res.find(p);
        if (it != g_res.end()) {
            std::snprintf(buf, sizeof(buf), "%c%d(%ux%u %s)", it->second.kind, it->second.id, it->second.w,
                          it->second.h, fmt_name(it->second.fmt, fb));
            return buf;
        }
    }
    std::snprintf(buf, sizeof(buf), "%p", p);
    return buf;
}

// A surface: the render target texture it belongs to, else the surface itself, else its size and format.
const GUID kIidTexture = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};

std::string surface_text(IDirect3DSurface9* s) {
    if (!s) return "null";
    bool known;
    {
        std::lock_guard<std::mutex> lock(g_res_lock);
        known = g_res.count(s) != 0;
    }
    if (!known) {
        IDirect3DTexture9* tex = nullptr;
        if (SUCCEEDED(s->GetContainer(kIidTexture, reinterpret_cast<void**>(&tex))) && tex) {
            std::string t = res_text(tex);
            tex->Release();
            if (t.size() && t[0] == 'T') return t;
        }
        D3DSURFACE_DESC d{};
        char buf[96], fb[16];
        if (SUCCEEDED(s->GetDesc(&d))) {
            std::snprintf(buf, sizeof(buf), "surf(%ux%u %s)", d.Width, d.Height, fmt_name(d.Format, fb));
            return buf;
        }
    }
    return res_text(s);
}

std::string texture_text(IDirect3DBaseTexture9* t) {
    if (!t) return "null";
    std::string known = res_text(t);
    if (known.size() && known[0] == 'T') return known;
    if (t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d{};
        char buf[96], fb[16];
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) {
            std::snprintf(buf, sizeof(buf), "tex(%ux%u %s %lu mips)", d.Width, d.Height, fmt_name(d.Format, fb),
                          static_cast<unsigned long>(t->GetLevelCount()));
            return buf;
        }
    } else if (t->GetType() == D3DRTYPE_CUBETEXTURE) {
        return "cube";
    }
    return "tex?";
}

std::string shader_name(void* sh) {
    if (!sh) return "none";
    std::lock_guard<std::mutex> lock(g_res_lock);
    auto it = g_shaders.find(sh);
    return it != g_shaders.end() ? it->second : "unknown";
}

std::unordered_map<std::string, int> g_made;  // shader creations by name (name_shader)
int g_made_fresh = 0, g_made_again = 0;
ULONGLONG g_made_next = 0;

void name_shader(const DWORD* code, void* shader, const char* prefix) {
    if (!code || !shader) return;
    size_t n = 1;
    while (n < 65536 && code[n] != 0x0000FFFF) ++n;
    uint64_t h = 1469598103934665603ULL;
    const auto* b = reinterpret_cast<const uint8_t*>(code);
    for (size_t i = 0; i < (n + 1) * 4; ++i) h = (h ^ b[i]) * 1099511628211ULL;
    char name[40];
    std::snprintf(name, sizeof(name), "%s_%08x", prefix, static_cast<unsigned>(h ^ (h >> 32)));
    {
        // Shader creations, new and again (the same bytecode made before), summed every 10 s: whether the game makes
        // the same shaders again as map parts stream in (each one a driver compile).
        char line[160] = "";
        {
            std::lock_guard<std::mutex> lock(g_res_lock);
            g_shaders[shader] = name;
            int& n = g_made[name];
            if (n++) {
                ++g_made_again;
            } else {
                ++g_made_fresh;
            }
            const ULONGLONG now = GetTickCount64();
            if (!g_made_next) g_made_next = now + 10000;
            if (now >= g_made_next) {
                std::snprintf(line, sizeof(line), "[shaders] last 10 s: %d new, %d made again (same bytecode as one "
                              "made before); %u distinct so far", g_made_fresh, g_made_again,
                              static_cast<unsigned>(g_made.size()));
                g_made_fresh = g_made_again = 0;
                g_made_next = now + 10000;
            }
        }
        if (line[0]) LOG_INFO("%s", line);
    }
    std::wstring dir = std::wstring(g_dir) + L"\\fpsu-shaders";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\" + widen(name) + L".txt";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    {
        // The bytecode too (fpsu-shaders\NAME.bin), for matching a shader in release code.
        // Its whole length: comment blocks skipped (a debug comment can hold the end token's value). The name keeps
        // the first count above, so names stay as they were.
        size_t full = 1;
        while (full < 65536 && code[full] != 0x0000FFFF) {
            full += (code[full] & 0xFFFF) == 0xFFFE ? 1 + ((code[full] >> 16) & 0x7FFF) : 1;
        }
        const std::wstring bin = dir + L"\\" + widen(name) + L".bin";
        FILE* fb = _wfopen(bin.c_str(), L"wb");
        if (fb) {
            std::fwrite(code, 4, full + 1, fb);
            std::fclose(fb);
        }
    }
    XBuffer* text = nullptr;
    if (x_disassemble && SUCCEEDED(x_disassemble(code, FALSE, nullptr, &text)) && text) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) {
            std::fwrite(text->GetBufferPointer(), 1, std::strlen(static_cast<const char*>(text->GetBufferPointer())),
                        f);
            std::fclose(f);
        }
        text->Release();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Frame capture.
volatile bool g_capturing = false;
volatile bool g_frame_requested = false;
std::string g_frame_name, g_shot_name, g_dump_name;
volatile bool g_shot_requested = false, g_dump_requested = false;
volatile LONG g_query_frames = 0;  // log occlusion query results for this many more frames
std::mutex g_frame_lock;
std::string g_frame;
int g_frame_line = 0;

void cap(const char* fmt, ...) {
    if (!g_capturing) return;
    char line[1600];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_frame_lock);
    char head[32];
    std::snprintf(head, sizeof(head), "%5d %5lu ", ++g_frame_line, GetCurrentThreadId());
    g_frame += head;
    g_frame.append(line, n > 0 && n < static_cast<int>(sizeof(line)) ? n : std::strlen(line));
    g_frame += '\n';
}

void save_surface_png(IDirect3DDevice9* dev, IDirect3DSurface9* src, const std::wstring& path) {
    D3DSURFACE_DESC d{};
    if (FAILED(src->GetDesc(&d))) return;
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        !sys) {
        out("  could not make a %ux%u copy", d.Width, d.Height);
        return;
    }
    HRESULT hr = o_get_rt_data ? o_get_rt_data(dev, src, sys) : dev->GetRenderTargetData(src, sys);
    const bool bmp = path.size() > 4 && path.compare(path.size() - 4, 4, L".bmp") == 0;
    if (SUCCEEDED(hr) && x_save_surface) hr = x_save_surface(path.c_str(), bmp ? 0 : kPng, sys, nullptr, nullptr);
    sys->Release();
    if (FAILED(hr)) out("  saving failed, hr 0x%08lX", static_cast<unsigned long>(hr));
}

void save_backbuffer(IDirect3DDevice9* dev, const std::string& name) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        out("shot %s: no back buffer", name.c_str());
        return;
    }
    save_surface_png(dev, bb, shot_path(name, L".bmp"));
    bb->Release();
    out("shot %s: saved", name.c_str());
}

// Small, quick pictures: the back buffer scaled down on the GPU first (bursts of these show motion).
std::string g_snap_name;
volatile LONG g_snap_left = 0;
volatile LONG g_snap_index = 0;
ULONGLONG g_snap_next = 0;
DWORD g_snap_interval = 100;
UINT g_snap_width = 640;
IDirect3DSurface9* g_snap_rt = nullptr;
UINT g_snap_w = 0, g_snap_h = 0;

void snap(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    const UINT w = g_snap_width, h = d.Width ? d.Height * w / d.Width : 0;
    if (g_snap_rt && (g_snap_w != w || g_snap_h != h)) {
        g_snap_rt->Release();
        g_snap_rt = nullptr;
    }
    if (!g_snap_rt && o_create_rt) {
        o_create_rt(dev, w, h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_snap_rt, nullptr);
        g_snap_w = w;
        g_snap_h = h;
    }
    if (g_snap_rt && o_stretch && SUCCEEDED(o_stretch(dev, bb, nullptr, g_snap_rt, nullptr, D3DTEXF_LINEAR))) {
        char tag[48];
        std::snprintf(tag, sizeof(tag), "-%03ld", static_cast<long>(g_snap_index));
        save_surface_png(dev, g_snap_rt, shot_path(g_snap_name + tag, L".bmp"));
    }
    bb->Release();
}

// One render target texture saved every so often (rtseq): its own picture, without lighting or the HUD.
std::string g_seq_name;
int g_seq_id = 0;
volatile LONG g_seq_left = 0;
LONG g_seq_index = 0;
ULONGLONG g_seq_next = 0;
DWORD g_seq_interval = 33;

void seq_tick(IDirect3DDevice9* dev) {
    IDirect3DTexture9* tex = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_res_lock);
        for (const auto& kv : g_res) {
            if (kv.second.kind == 'T' && kv.second.id == g_seq_id) {
                tex = static_cast<IDirect3DTexture9*>(kv.first);
                tex->AddRef();
                break;
            }
        }
    }
    if (!tex) {
        out("rtseq: no T%d", g_seq_id);
        g_seq_left = 0;
        return;
    }
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(tex->GetSurfaceLevel(0, &s)) && s) {
        char tag[48];
        std::snprintf(tag, sizeof(tag), "-%03ld", static_cast<long>(g_seq_index));
        save_surface_png(dev, s, shot_path(g_seq_name + tag, L".bmp"));
        s->Release();
    }
    tex->Release();
}

void dump_targets(IDirect3DDevice9* dev, const std::string& name) {
    // A reference each while the list is locked: a texture leaves the list only on its last Release.
    std::vector<std::pair<IDirect3DTexture9*, Res>> list;
    {
        std::lock_guard<std::mutex> lock(g_res_lock);
        for (const auto& kv : g_res) {
            if (kv.second.kind == 'T' && (kv.second.usage & D3DUSAGE_RENDERTARGET)) {
                auto* t = static_cast<IDirect3DTexture9*>(kv.first);
                t->AddRef();
                list.emplace_back(t, kv.second);
            }
        }
    }
    int saved = 0;
    for (const auto& kv : list) {
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(kv.first->GetSurfaceLevel(0, &s)) && s) {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "-T%d", kv.second.id);
            save_surface_png(dev, s, shot_path(name + tag, L".bmp"));
            s->Release();
            ++saved;
        }
        kv.first->Release();
    }
    out("rtdump %s: %d render target textures", name.c_str(), saved);
}

void write_frame() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_frame_lock);
        text.swap(g_frame);
        g_frame_line = 0;
    }
    FILE* f = _wfopen(shot_path(g_frame_name, L".txt").c_str(), L"wb");
    if (f) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    out("frame %s: %zu bytes", g_frame_name.c_str(), text.size());
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks.
HRESULT WINAPI hk_present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    if (g_capturing) {
        cap("Present");
        g_capturing = false;
        write_frame();
        save_backbuffer(dev, g_frame_name);
    }
    if (g_shot_requested) {
        g_shot_requested = false;
        save_backbuffer(dev, g_shot_name);
    }
    if (g_dump_requested) {
        g_dump_requested = false;
        dump_targets(dev, g_dump_name);
    }
    if (g_query_frames > 0) InterlockedDecrement(&g_query_frames);
    if (g_seq_left > 0 && GetTickCount64() >= g_seq_next) {
        g_seq_next = GetTickCount64() + g_seq_interval;
        seq_tick(dev);
        ++g_seq_index;
        if (InterlockedDecrement(&g_seq_left) == 0) out("rtseq %s: done (%ld)", g_seq_name.c_str(), g_seq_index);
    }
    if (g_snap_left > 0 && GetTickCount64() >= g_snap_next) {
        g_snap_next = GetTickCount64() + g_snap_interval;
        snap(dev);
        InterlockedIncrement(&g_snap_index);
        if (InterlockedDecrement(&g_snap_left) == 0) {
            out("snaps %s: done (%ld)", g_snap_name.c_str(), static_cast<long>(g_snap_index));
        }
    }
    const HRESULT hr = o_present(dev, a, b, w, r);
    if (g_frame_requested) {
        g_frame_requested = false;
        g_capturing = true;
    }
    return hr;
}

// Game code return addresses on the current stack (the game's .text), for logging who made a call.
bool read_word(const uintptr_t* p, uintptr_t* v) {
    __try {
        *v = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string game_stack(const void* from, int want) {
    const uintptr_t* sp = static_cast<const uintptr_t*>(from);
    std::string st;
    char b[16];
    int found = 0;
    for (int i = 0; i < 400 && found < want; ++i) {
        uintptr_t a = 0;
        if (!read_word(sp + i, &a)) break;
        if (a >= 0x00401000 && a < 0x010CC000) {
            std::snprintf(b, sizeof(b), " %X", static_cast<unsigned>(a));
            st += b;
            ++found;
        }
    }
    return st;
}

// Tracked objects leave the list on their last Release (per class: a class's method table is shared).
using ReleaseFn = ULONG(WINAPI*)(IUnknown*);
struct ReleaseHook {
    void** vtable;
    ReleaseFn original;
};
ReleaseHook g_release[8];
volatile LONG g_release_count = 0;

ULONG WINAPI hk_release(IUnknown* o) {
    void** vt = *reinterpret_cast<void***>(o);
    ReleaseFn original = nullptr;
    for (LONG i = 0; i < g_release_count; ++i) {
        if (g_release[i].vtable == vt) {
            original = g_release[i].original;
            break;
        }
    }
    if (!original) return 1;
    const ULONG n = original(o);
    if (n == 0) {
        std::lock_guard<std::mutex> lock(g_res_lock);
        g_res.erase(o);
    }
    return n;
}

void hook_release(void* object) {
    void** vt = *reinterpret_cast<void***>(object);
    for (LONG i = 0; i < g_release_count; ++i) {
        if (g_release[i].vtable == vt) return;
    }
    if (g_release_count >= 8) return;
    // Publish the entry before the slot changes: other threads release objects of this class meanwhile.
    g_release[g_release_count].vtable = vt;
    g_release[g_release_count].original = reinterpret_cast<ReleaseFn>(vt[2]);
    InterlockedIncrement(&g_release_count);
    DWORD old = 0;
    if (VirtualProtect(&vt[2], sizeof(void*), PAGE_READWRITE, &old)) {
        vt[2] = reinterpret_cast<void*>(&hk_release);
        DWORD ignored = 0;
        VirtualProtect(&vt[2], sizeof(void*), old, &ignored);
    }
}

void track(void* p, char kind, UINT w, UINT h, D3DFORMAT f, DWORD usage, const char* what, void* caller) {
    int id;
    {
        std::lock_guard<std::mutex> lock(g_res_lock);
        id = g_next_res++;
        g_res[p] = Res{id, kind, w, h, f, usage};
        hook_release(p);
    }
    char fb[16];
    const std::string st = game_stack(_AddressOfReturnAddress(), 6);
    out("create %c%d %s %ux%u %s usage 0x%lX caller %p game%s", kind, id, what, w, h, fmt_name(f, fb),
        static_cast<unsigned long>(usage), caller, st.c_str());
}

HRESULT WINAPI hk_create_texture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f,
                                 D3DPOOL pool, IDirect3DTexture9** out_tex, HANDLE* shared) {
    const HRESULT hr = o_create_texture(dev, w, h, levels, usage, f, pool, out_tex, shared);
    if (SUCCEEDED(hr) && out_tex && *out_tex && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        track(*out_tex, 'T', w, h, f, usage, "texture", _ReturnAddress());
    }
    return hr;
}
HRESULT WINAPI hk_create_rt(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q,
                            BOOL lockable, IDirect3DSurface9** s, HANDLE* shared) {
    const HRESULT hr = o_create_rt(dev, w, h, f, ms, q, lockable, s, shared);
    if (SUCCEEDED(hr) && s && *s) track(*s, 'R', w, h, f, D3DUSAGE_RENDERTARGET, "surface", _ReturnAddress());
    return hr;
}
HRESULT WINAPI hk_create_depth(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q,
                               BOOL discard, IDirect3DSurface9** s, HANDLE* shared) {
    const HRESULT hr = o_create_depth(dev, w, h, f, ms, q, discard, s, shared);
    if (SUCCEEDED(hr) && s && *s) {
        bool known;
        {
            std::lock_guard<std::mutex> lock(g_res_lock);
            known = g_res.count(*s) != 0;
        }
        if (!known) track(*s, 'D', w, h, f, D3DUSAGE_DEPTHSTENCIL, "depth", _ReturnAddress());
    }
    return hr;
}
HRESULT WINAPI hk_get_rt_data(IDirect3DDevice9* dev, IDirect3DSurface9* a, IDirect3DSurface9* b) {
    if (g_capturing) cap("GetRenderTargetData %s -> %s", surface_text(a).c_str(), surface_text(b).c_str());
    return o_get_rt_data(dev, a, b);
}
HRESULT WINAPI hk_stretch(IDirect3DDevice9* dev, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* d,
                          const RECT* dr, D3DTEXTUREFILTERTYPE f) {
    if (g_capturing) {
        char a[64] = "all", b[64] = "all";
        if (sr) std::snprintf(a, sizeof(a), "%ld,%ld-%ld,%ld", sr->left, sr->top, sr->right, sr->bottom);
        if (dr) std::snprintf(b, sizeof(b), "%ld,%ld-%ld,%ld", dr->left, dr->top, dr->right, dr->bottom);
        cap("StretchRect %s [%s] -> %s [%s] filter %d", surface_text(s).c_str(), a, surface_text(d).c_str(), b,
            static_cast<int>(f));
    }
    return o_stretch(dev, s, sr, d, dr, f);
}
HRESULT WINAPI hk_color_fill(IDirect3DDevice9* dev, IDirect3DSurface9* s, const RECT* r, D3DCOLOR c) {
    if (g_capturing) cap("ColorFill %s 0x%08lX", surface_text(s).c_str(), static_cast<unsigned long>(c));
    return o_color_fill(dev, s, r, c);
}
HRESULT WINAPI hk_set_rt(IDirect3DDevice9* dev, DWORD i, IDirect3DSurface9* s) {
    if (g_capturing) cap("SetRenderTarget %lu %s", static_cast<unsigned long>(i), surface_text(s).c_str());
    return o_set_rt(dev, i, s);
}
HRESULT WINAPI hk_set_depth(IDirect3DDevice9* dev, IDirect3DSurface9* s) {
    if (g_capturing) cap("SetDepthStencil %s", surface_text(s).c_str());
    return o_set_depth(dev, s);
}
HRESULT WINAPI hk_clear(IDirect3DDevice9* dev, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z,
                        DWORD st) {
    cap("Clear flags %lu color 0x%08lX z %g rects %lu", static_cast<unsigned long>(flags),
        static_cast<unsigned long>(c), z, static_cast<unsigned long>(n));
    return o_clear(dev, n, r, flags, c, z, st);
}
HRESULT WINAPI hk_set_viewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* v) {
    if (g_capturing && v) {
        cap("SetViewport %lu,%lu %lux%lu z %g-%g", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ);
    }
    return o_set_viewport(dev, v);
}
HRESULT WINAPI hk_set_rs(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD v) {
    if (g_capturing) {
        switch (s) {
            case D3DRS_ZENABLE: case D3DRS_ZWRITEENABLE: case D3DRS_ALPHABLENDENABLE: case D3DRS_SRCBLEND:
            case D3DRS_DESTBLEND: case D3DRS_BLENDOP: case D3DRS_ALPHATESTENABLE: case D3DRS_ZFUNC:
            case D3DRS_COLORWRITEENABLE: case D3DRS_SRGBWRITEENABLE: case D3DRS_STENCILENABLE: case D3DRS_CULLMODE:
            case D3DRS_SCISSORTESTENABLE:
                cap("rs %d = %lu", static_cast<int>(s), static_cast<unsigned long>(v));
                break;
            default:
                break;
        }
    }
    return o_set_rs(dev, s, v);
}
HRESULT WINAPI hk_set_texture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t) {
    if (g_capturing) cap("SetTexture %lu %s", static_cast<unsigned long>(stage), texture_text(t).c_str());
    return o_set_texture(dev, stage, t);
}
HRESULT WINAPI hk_set_scissor(IDirect3DDevice9* dev, const RECT* r) {
    if (g_capturing && r) cap("SetScissor %ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
    return o_set_scissor(dev, r);
}
bool safe_read_any(uintptr_t a, void* outp, size_t n) {
    __try {
        std::memcpy(outp, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// vbwatch PS: while capturing, draws made with that pixel shader also log their first vertices (stream 0).
std::string g_vb_ps, g_cur_ps;

void log_vertices(IDirect3DDevice9* dev, UINT start, UINT verts) {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(0, &vb, &offset, &stride)) || !vb || !stride) {
        cap("  vb: none");
        if (vb) vb->Release();
        return;
    }
    D3DVERTEXBUFFER_DESC d{};
    vb->GetDesc(&d);
    void* p = nullptr;
    const UINT n = verts < 8 ? verts : 8;
    if (SUCCEEDED(vb->Lock(offset + start * stride, n * stride, &p, D3DLOCK_READONLY)) && p) {
        const float* f = static_cast<const float*>(p);
        const UINT nf = stride / 4;
        for (UINT v = 0; v < n; ++v) {
            std::string line;
            char b[32];
            for (UINT k = 0; k < nf && k < 12; ++k) {
                std::snprintf(b, sizeof(b), " %g", f[v * nf + k]);
                line += b;
            }
            cap("  v%u:%s", v, line.c_str());
        }
        vb->Unlock();
    } else {
        cap("  vb: lock failed (usage 0x%lX pool %d)", static_cast<unsigned long>(d.Usage), static_cast<int>(d.Pool));
    }
    vb->Release();
}

HRESULT WINAPI hk_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT count) {
    cap("Draw type %d start %u prims %u", static_cast<int>(t), start, count);
    if (g_capturing && !g_vb_ps.empty() && g_cur_ps == g_vb_ps) log_vertices(dev, start, count * 3);
    return o_draw(dev, t, start, count);
}
HRESULT WINAPI hk_draw_idx(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT mn, UINT nv, UINT si, UINT pc) {
    cap("DrawIndexed type %d verts %u prims %u", static_cast<int>(t), nv, pc);
    return o_draw_idx(dev, t, base, mn, nv, si, pc);
}
HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT pc, const void* v, UINT stride) {
    if (g_capturing) {
        // Full-screen passes are usually a quad or triangle drawn from memory: show its first vertex.
        const float* f = static_cast<const float*>(v);
        if (f && stride >= 16) {
            cap("DrawUP type %d prims %u stride %u v0 %g %g %g %g", static_cast<int>(t), pc, stride, f[0], f[1], f[2],
                f[3]);
        } else {
            cap("DrawUP type %d prims %u stride %u", static_cast<int>(t), pc, stride);
        }
    }
    return o_draw_up(dev, t, pc, v, stride);
}
HRESULT WINAPI hk_draw_idx_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT mn, UINT nv, UINT pc, const void* idx,
                              D3DFORMAT ifmt, const void* v, UINT stride) {
    cap("DrawIndexedUP type %d verts %u prims %u stride %u", static_cast<int>(t), nv, pc, stride);
    return o_draw_idx_up(dev, t, mn, nv, pc, idx, ifmt, v, stride);
}
HRESULT WINAPI hk_create_vs(IDirect3DDevice9* dev, const DWORD* code, void** sh) {
    const HRESULT hr = o_create_vs(dev, code, sh);
    if (SUCCEEDED(hr) && sh) name_shader(code, *sh, "vs");
    return hr;
}
HRESULT WINAPI hk_create_ps(IDirect3DDevice9* dev, const DWORD* code, void** sh) {
    const HRESULT hr = o_create_ps(dev, code, sh);
    if (SUCCEEDED(hr) && sh) name_shader(code, *sh, "ps");
    return hr;
}
HRESULT WINAPI hk_set_vs(IDirect3DDevice9* dev, void* sh) {
    if (g_capturing) cap("SetVS %s", shader_name(sh).c_str());
    return o_set_vs(dev, sh);
}
HRESULT WINAPI hk_set_ps(IDirect3DDevice9* dev, void* sh) {
    if (g_capturing) {
        g_cur_ps = shader_name(sh);
        cap("SetPS %s", g_cur_ps.c_str());
    }
    return o_set_ps(dev, sh);
}
// scanval REG: at the watched shader's next upload of REG, find that float's bits in the game's writable memory.
volatile LONG g_scan_reg = -1;

void scan_value(const float* src) {
    uint32_t bits = 0;
    std::memcpy(&bits, src, 4);
    out("scanval: looking for %g (0x%08X), constant data at %p", *src, bits, src);
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t a = 0x10000;
    int hits = 0;
    while (a < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
        const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
        const bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        if (rw) {
            for (uintptr_t q = lo; q + 4 <= hi && hits < 60; q += 4) {
                uint32_t w = 0;
                if (!safe_read_any(q, &w, 4)) break;
                if (w == bits && q != reinterpret_cast<uintptr_t>(src)) {
                    float n[8] = {};
                    safe_read_any(q - 16, n, 32);
                    out("  %08X: %g %g %g %g [%g] %g %g %g", static_cast<unsigned>(q), n[0], n[1], n[2], n[3], n[4],
                        n[5], n[6], n[7]);
                    ++hits;
                }
            }
        }
        a = hi;
    }
    out("scanval: %d places", hits);
}

HRESULT WINAPI hk_set_ps_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    if (g_scan_reg >= 0 && v && !g_vb_ps.empty() && g_cur_ps == g_vb_ps &&
        static_cast<UINT>(g_scan_reg) >= start && static_cast<UINT>(g_scan_reg) < start + count) {
        const LONG reg = g_scan_reg;
        g_scan_reg = -1;
        scan_value(v + (reg - start) * 4);
    }
    if (g_capturing && v) {
        std::string s;
        char b[96];
        for (UINT i = 0; i < count && i < 6; ++i) {
            std::snprintf(b, sizeof(b), " [%u] %g %g %g %g", start + i, v[i * 4], v[i * 4 + 1], v[i * 4 + 2],
                          v[i * 4 + 3]);
            s += b;
        }
        cap("PSConst %u+%u%s", start, count, s.c_str());
        if (!g_vb_ps.empty() && g_cur_ps == g_vb_ps) {
            // Who uploads the watched shader's constants: game code return addresses on the stack.
            const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
            std::string st;
            char b[16];
            int found = 0;
            for (int i = 0; i < 160 && found < 12; ++i) {
                uintptr_t a = 0;
                if (!safe_read_any(reinterpret_cast<uintptr_t>(sp + i), &a, 4)) break;
                if (a >= 0x00401000 && a < 0x010CC000) {
                    std::snprintf(b, sizeof(b), " %X", static_cast<unsigned>(a));
                    st += b;
                    ++found;
                }
            }
            cap("  stack:%s", st.c_str());
        }
    }
    return o_set_ps_const(dev, start, v, count);
}
HRESULT WINAPI hk_set_vs_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    if (g_capturing && v) {
        std::string s;
        char b[96];
        for (UINT i = 0; i < count && i < 6; ++i) {
            std::snprintf(b, sizeof(b), " [%u] %g %g %g %g", start + i, v[i * 4], v[i * 4 + 1], v[i * 4 + 2],
                          v[i * 4 + 3]);
            s += b;
        }
        cap("VSConst %u+%u%s", start, count, s.c_str());
    }
    return o_set_vs_const(dev, start, v, count);
}
int query_id(IDirect3DQuery9* q) {
    std::lock_guard<std::mutex> lock(g_res_lock);
    auto it = g_queries.find(q);
    return it != g_queries.end() ? it->second : 0;
}
HRESULT WINAPI hk_query_issue(IDirect3DQuery9* q, DWORD flags) {
    if (g_capturing) cap("Query Q%d issue %s", query_id(q), (flags & D3DISSUE_BEGIN) ? "begin" : "end");
    return o_query_issue(q, flags);
}
// The caller's EDI at GetData (the lens flare code 0xFD3A20 keeps its LensFlareDrawEntity there).
volatile uint32_t g_query_edi = 0;

HRESULT WINAPI hk_query_get_data(IDirect3DQuery9* q, void* data, DWORD size, DWORD flags) {
    const uint32_t edi = g_query_edi;
    const HRESULT hr = o_query_get_data(q, data, size, flags);
    if (g_query_frames > 0 && reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x00FD3A52 && edi) {
        float wh[2] = {};
        uint32_t stored = 0;
        safe_read_any(edi + 0x40, &stored, 4);
        safe_read_any(edi + 0x60, wh, 8);
        out("lensflare %08X: result %ld hr 0x%08lX, stored %u, size %g x %g (expected %.0f px, kept below %.0f)",
            edi, hr == S_OK && data ? static_cast<long>(*static_cast<const DWORD*>(data)) : -1L,
            static_cast<unsigned long>(hr), stored, wh[1], wh[0], wh[0] * wh[1] * 0.8, wh[0] * wh[1] * 0.8 * 1.5);
    }
    if ((g_capturing || g_query_frames > 0) && hr == S_OK && data && size >= 4 && q->GetType() == D3DQUERYTYPE_OCCLUSION) {
        const unsigned long pixels = *static_cast<const DWORD*>(data);
        if (g_capturing) cap("Query Q%d result %lu pixels", query_id(q), pixels);
        if (g_query_frames > 0) out("query Q%d %lu pixels (caller %p)", query_id(q), pixels, _ReturnAddress());
    }
    return hr;
}
__declspec(naked) void hk_query_get_data_stub() {
    __asm {
        mov g_query_edi, edi
        jmp hk_query_get_data
    }
}

void* patch(void* object, int slot, void* replacement);
HRESULT WINAPI hk_create_query(IDirect3DDevice9* dev, D3DQUERYTYPE t, IDirect3DQuery9** q) {
    const HRESULT hr = o_create_query(dev, t, q);
    if (SUCCEEDED(hr) && q && *q) {
        int id;
        {
            std::lock_guard<std::mutex> lock(g_res_lock);
            id = g_next_query++;
            g_queries[*q] = id;
        }
        out("create Q%d query type %d caller %p", id, static_cast<int>(t), _ReturnAddress());
        if (!o_query_issue) {
            o_query_issue = reinterpret_cast<QueryIssueFn>(patch(*q, kQueryIssue, reinterpret_cast<void*>(&hk_query_issue)));
            o_query_get_data =
                reinterpret_cast<QueryGetDataFn>(patch(*q, kQueryGetData, reinterpret_cast<void*>(&hk_query_get_data_stub)));
        }
    }
    return hr;
}

void* patch(void* object, int slot, void* replacement) {
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

template <typename T>
void hook(IDirect3DDevice9* dev, int slot, T& original, void* replacement) {
    void* o = patch(dev, slot, replacement);
    if (o && !original) original = reinterpret_cast<T>(o);
}

// ---------------------------------------------------------------------------------------------------------------
// Commands.
bool safe_read(uintptr_t a, void* outp, size_t n) {
    __try {
        std::memcpy(outp, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The player's physics position block: [[[[WorldChrMan] + 4]] + 0x28] + 0x1C, xyz at +0x10.
uintptr_t player_physics() {
    uintptr_t wcm = 0, first = 0, chr = 0, ctrl = 0, phys = 0;
    if (!safe_read(0x0137DC70, &wcm, 4) || !wcm) return 0;
    if (!safe_read(wcm + 4, &first, 4) || !first) return 0;
    if (!safe_read(first, &chr, 4) || !chr) return 0;
    if (!safe_read(chr + 0x28, &ctrl, 4) || !ctrl) return 0;
    if (!safe_read(ctrl + 0x1C, &phys, 4) || !phys) return 0;
    return phys;
}

bool player_position(float* xyz) {
    const uintptr_t phys = player_physics();
    return phys && safe_read(phys + 0x10, xyz, 12);
}

// Event flags and warps, as the game's own event code does them (game thread only).
constexpr uint32_t kEventFlagMan = 0x0137D7D4;
constexpr uint32_t kGetFlag = 0x00D38C40;  // eax = EventFlagMan, cl = 0, push id -> al; ret 4
constexpr uint32_t kSetFlag = 0x00D38CB0;  // eax = EventFlagMan, cl = 0, push value, push id; ret 8
constexpr uint32_t kGameMan = 0x013784A0;
constexpr uint32_t kWarpKick = 0x00D62930;  // WarpNextStageKick: takes GameMan +0x14 map, +0x18 point

int read_flag(int id) {
    uint32_t efm = 0;
    if (!safe_read(kEventFlagMan, &efm, 4) || !efm) return -1;
    uint8_t on = 0;
    __asm {
        push id
        xor ecx, ecx
        mov eax, efm
        mov edx, kGetFlag
        call edx
        mov on, al
    }
    return on;
}

void write_flag(int id, int value) {
    uint32_t efm = 0;
    if (!safe_read(kEventFlagMan, &efm, 4) || !efm) return;
    uint32_t v = value ? 1 : 0;
    __asm {
        push v
        push id
        xor ecx, ecx
        mov eax, efm
        mov edx, kSetFlag
        call edx
    }
}

std::mutex g_game_lock;
std::vector<std::vector<std::string>> g_game_cmds;
volatile bool g_god = false;
float g_hold_pos[3] = {};
ULONGLONG g_hold_until = 0;

// Keeps the player's HP at its maximum (chr +0x2D4 HP, +0x2D8 max HP) while `god` is on.
void god_tick() {
    uintptr_t wcm = 0, first = 0, chr = 0;
    if (!safe_read(0x0137DC70, &wcm, 4) || !wcm) return;
    if (!safe_read(wcm + 4, &first, 4) || !first) return;
    if (!safe_read(first, &chr, 4) || !chr) return;
    int hp[2] = {};
    if (!safe_read(chr + 0x2D4, hp, 8) || hp[1] <= 0 || hp[1] > 10000) return;
    if (hp[0] > 0 && hp[0] < hp[1]) *reinterpret_cast<int*>(chr + 0x2D4) = hp[1];
}

// fakefocus 1: the game reads input only while GetForegroundWindow() is its window (0xF6FB54, 0xF72500 store the
// answer at +0x4C); for runs where nothing can take the foreground (display asleep, input desktop unavailable), its
// import answers with the game's own window. fakefocus 0 gives back the real answer.
constexpr uint32_t kForegroundImport = 0x010CC2BC;
volatile bool g_fake_focus = false;
using GetForegroundFn = HWND(WINAPI*)();
GetForegroundFn g_get_foreground = nullptr;
HWND WINAPI fake_foreground() {
    if (g_fake_focus) {
        static HWND game = nullptr;
        if (!game || !IsWindow(game)) game = FindWindowW(L"DARK SOULS", nullptr);
        if (game) return game;
    }
    return g_get_foreground();
}

void set_fake_focus(bool on) {
    auto* slot = reinterpret_cast<void**>(kForegroundImport);
    if (!g_get_foreground) {
        DWORD old = 0;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return;
        g_get_foreground = reinterpret_cast<GetForegroundFn>(*slot);
        *slot = reinterpret_cast<void*>(&fake_foreground);
        VirtualProtect(slot, 4, old, &old);
    }
    g_fake_focus = on;
}

// The player's character (WorldChrMan +4 -> first entry), or 0.
uintptr_t player_chr() {
    uintptr_t wcm = 0, first = 0, chr = 0;
    if (!safe_read(0x0137DC70, &wcm, 4) || !wcm) return 0;
    if (!safe_read(wcm + 4, &first, 4) || !first) return 0;
    if (!safe_read(first, &chr, 4)) return 0;
    return chr;
}

// ChrIns::ApplySpEffect (0xE7E950, vtable slot 125; thiscall id, source, 0, 1.0, 1.0, 1.0, own), as Prepare to
// Co-op's world.cpp calls it: the player as the source, the effect this game's own.
using ApplySpEffectFn = char(__fastcall*)(uintptr_t chr, void*, int id, uintptr_t src, int a, float f1, float f2,
                                          float f3, int own);
bool apply_speffect(uintptr_t chr, int id) {
    uintptr_t vt = 0, fn = 0;
    if (!chr || !safe_read(chr, &vt, 4) || !safe_read(vt + 125 * 4, &fn, 4) || fn != 0x00E7E950) return false;
    __try {
        return reinterpret_cast<ApplySpEffectFn>(fn)(chr, nullptr, id, chr, 0, 1.0f, 1.0f, 1.0f, 1) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The animation a character plays on layer 0 or 1 (0xE2C030: edi = layer, push the controller's animation
// object [chr+0x28]+0x2C; packed as the network samples carry it), -1 none.
int anim_on(uintptr_t chr, int layer) {
    uintptr_t ctrl = 0, ac = 0;
    if (!chr || !safe_read(chr + 0x28, &ctrl, 4) || !ctrl || !safe_read(ctrl + 0x2C, &ac, 4) || !ac) return -1;
    uint32_t r = 0xFFFFFFFF;
    __try {
        __asm {
            push edi
            mov edi, layer
            push ac
            mov eax, 0x00E2C030
            call eax
            pop edi
            mov r, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return static_cast<int>(r);
}

// ChrIns event animation: 0xE69590 starts one (esi = character, edi = animation, stdcall(loop)), 0xE69470 stops it
// (edi = character).
bool play_event_anim(uintptr_t chr, int anim, int loop) {
    if (!chr) return false;
    __try {
        __asm {
            push esi
            push edi
            mov esi, chr
            mov edi, anim
            push loop
            mov eax, 0x00E69590
            call eax
            pop edi
            pop esi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool stop_event_anim(uintptr_t chr) {
    if (!chr) return false;
    __try {
        __asm {
            push edi
            mov edi, chr
            mov eax, 0x00E69470
            call eax
            pop edi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void run_game(const std::vector<std::string>& t) {
    auto num = [&](size_t k, long dflt) { return k < t.size() ? std::strtol(t[k].c_str(), nullptr, 0) : dflt; };
    auto fnum = [&](size_t k) { return k < t.size() ? static_cast<float>(std::atof(t[k].c_str())) : 0.0f; };
    const std::string& c = t[0];
    if (c == "speffect") {
        // speffect ID: apply SpEffectParam row ID to the player (diagnostics: ticking effects, SpEffect timing).
        out("speffect %ld: %s", num(1, 0), apply_speffect(player_chr(), static_cast<int>(num(1, 0))) ? "applied" :
            "refused");
    } else if (c == "anim") {
        // anim: the player's animations on layers 0/1 (packed) and the animation id it plays (ChrCtrl+0x350);
        // anim ID [LOOP]: play event animation ID; anim stop.
        const uintptr_t me = player_chr();
        if (t.size() > 1 && t[1] == "stop") {
            out("anim stop: %s", stop_event_anim(me) ? "done" : "failed");
        } else if (t.size() > 1 && t[1] == "scan") {
            // anim scan LO HI: dwords in [LO, HI] in the player's character and controller, and one pointer deep.
            const long lo = num(2, 9000), hi = num(3, 9300);
            uintptr_t ctrl = 0;
            safe_read(me + 0x28, &ctrl, 4);
            const uintptr_t roots[2] = {me, ctrl};
            const char* names[2] = {"chr", "ctrl"};
            int hits = 0;
            for (int r = 0; r < 2 && hits < 60; ++r) {
                if (!roots[r]) continue;
                for (uint32_t o = 0; o < 0x800 && hits < 60; o += 4) {
                    uint32_t v = 0;
                    if (!safe_read_any(roots[r] + o, &v, 4)) break;
                    if (static_cast<long>(v) >= lo && static_cast<long>(v) <= hi) {
                        out("  %s+%X = %u", names[r], o, v);
                        ++hits;
                    }
                    if (v >= 0x01000000 && v < 0x7FFF0000 && (v & 3) == 0) {
                        for (uint32_t o2 = 0; o2 < 0x400 && hits < 60; o2 += 4) {
                            uint32_t w = 0;
                            if (!safe_read_any(v + o2, &w, 4)) break;
                            if (static_cast<long>(w) >= lo && static_cast<long>(w) <= hi) {
                                out("  [%s+%X]+%X = %u", names[r], o, o2, w);
                                ++hits;
                            }
                        }
                    }
                }
            }
            out("anim scan %ld..%ld: %d hits", lo, hi, hits);
        } else if (t.size() > 1) {
            out("anim %ld: %s", num(1, 0), play_event_anim(me, static_cast<int>(num(1, 0)),
                                                       static_cast<int>(num(2, 0))) ? "started" : "failed");
        } else {
            uintptr_t ctrl = 0;
            int playing = -1;
            int gesture = -1;
            if (me && safe_read(me + 0x28, &ctrl, 4) && ctrl) {
                safe_read(ctrl + 0x350, &playing, 4);
                safe_read(ctrl + 0x370, &gesture, 4);
            }
            out("anim %08X/%08X playing %d ctrl+370 %d", anim_on(me, 0), anim_on(me, 1), playing, gesture);
        }
    } else if (c == "flag") {
        out("flag %ld = %d", num(1, 0), read_flag(static_cast<int>(num(1, 0))));
    } else if (c == "setflag") {
        write_flag(static_cast<int>(num(1, 0)), static_cast<int>(num(2, 1)));
        out("setflag %ld -> %d", num(1, 0), read_flag(static_cast<int>(num(1, 0))));
    } else if (c == "tp") {
        const uintptr_t phys = player_physics();
        if (!phys) {
            out("tp: no player");
            return;
        }
        float p[3] = {fnum(1), fnum(2), fnum(3)};
        std::memcpy(reinterpret_cast<void*>(phys + 0x10), p, sizeof(p));
        // tp X Y Z MS: keep the player there for MS ms (the ground there may still be loading).
        std::memcpy(g_hold_pos, p, sizeof(p));
        g_hold_until = GetTickCount64() + static_cast<ULONGLONG>(num(4, 0));
        out("tp %.2f %.2f %.2f (held %ld ms)", p[0], p[1], p[2], num(4, 0));
    } else if (c == "warp") {
        uint32_t gm = 0;
        if (!safe_read(kGameMan, &gm, 4) || !gm) {
            out("warp: no GameMan");
            return;
        }
        const int map = static_cast<int>((num(1, 0) << 24) | (num(2, 0) << 16));
        *reinterpret_cast<int*>(gm + 0x14) = map;
        *reinterpret_cast<int*>(gm + 0x18) = static_cast<int>(num(3, -1));
        if (num(3, -1) < 0) *reinterpret_cast<uint8_t*>(gm + 0xB02) = 1;
        __asm {
            mov eax, kWarpKick
            call eax
        }
        out("warp m%02ld_%02ld point %ld", num(1, 0), num(2, 0), num(3, -1));
    }
}

std::vector<uintptr_t> g_chrs_found;  // the "chrs" command's characters

void run(const std::string& line) {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) ++i;
        size_t j = i;
        while (j < line.size() && !isspace(static_cast<unsigned char>(line[j]))) ++j;
        if (j > i) t.push_back(line.substr(i, j - i));
        i = j;
    }
    if (t.empty()) return;
    const std::string& c = t[0];
    auto num = [&](size_t k, long dflt) { return k < t.size() ? std::strtol(t[k].c_str(), nullptr, 0) : dflt; };
    if (c == "say") {
        out("say %s", line.size() > 4 ? line.c_str() + 4 : "");
    } else if (c == "fps") {
        limiter_set_fps(static_cast<int>(num(1, 0)));
        out("fps %ld", num(1, 0));
    } else if (c == "pad") {
        std::lock_guard<std::mutex> lock(g_pad_lock);
        g_pad_buttons = t.size() > 1 && t[1] != "none" ? parse_buttons(t[1]) : 0;
        g_pad_until = GetTickCount64() + static_cast<ULONGLONG>(num(2, 150));
        g_pad_lt = static_cast<BYTE>(num(3, 0));
        g_pad_rt = static_cast<BYTE>(num(4, 0));
        out("pad 0x%04X for %ld ms", g_pad_buttons, num(2, 150));
    } else if (c == "stick") {
        std::lock_guard<std::mutex> lock(g_pad_lock);
        for (int k = 0; k < 4; ++k) g_stick[k] = static_cast<SHORT>(num(1 + k, 0));
        g_stick_until = GetTickCount64() + static_cast<ULONGLONG>(num(5, 500));
        out("stick %d %d %d %d for %ld ms", g_stick[0], g_stick[1], g_stick[2], g_stick[3], num(5, 500));
    } else if (c == "pos") {
        float p[3];
        if (player_position(p)) {
            uintptr_t wcm = 0, first = 0, chr = 0;
            int map = -1;
            if (safe_read(0x0137DC70, &wcm, 4) && wcm && safe_read(wcm + 4, &first, 4) && first &&
                safe_read(first, &chr, 4) && chr) {
                safe_read(chr + 0x444, &map, 4);
            }
            out("pos %.3f %.3f %.3f map 0x%08X", p[0], p[1], p[2], static_cast<unsigned>(map));
        } else {
            out("pos: no player");
        }
    } else if (c == "snaps" && t.size() > 1) {
        g_snap_name = t[1];
        g_snap_index = 0;
        g_snap_interval = static_cast<DWORD>(num(3, 100));
        g_snap_width = static_cast<UINT>(num(4, 640));
        g_snap_next = 0;
        g_snap_left = static_cast<LONG>(num(2, 10));
        out("snaps %s: %ld every %ld ms", t[1].c_str(), num(2, 10), num(3, 100));
    } else if (c == "rtseq" && t.size() > 2) {
        g_seq_name = t[1];
        g_seq_id = static_cast<int>(num(2, 0));
        g_seq_index = 0;
        g_seq_interval = static_cast<DWORD>(num(4, 33));
        g_seq_next = 0;
        g_seq_left = static_cast<LONG>(num(3, 10));
        out("rtseq %s: T%ld, %ld every %ld ms", t[1].c_str(), num(2, 0), num(3, 10), num(4, 33));
    } else if (c == "qptr" && t.size() > 1) {
        // The IDirect3DQuery9 pointer of query Qn.
        const int want = static_cast<int>(num(1, 0));
        std::lock_guard<std::mutex> lock(g_res_lock);
        for (const auto& kv : g_queries) {
            if (kv.second == want) out("qptr Q%d = %08X", want, static_cast<unsigned>(reinterpret_cast<uintptr_t>(kv.first)));
        }
    } else if (c == "findptr" && t.size() > 1) {
        const uint32_t v = static_cast<uint32_t>(std::strtoul(t[1].c_str(), nullptr, 16));
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t a = 0x10000;
        int hits = 0;
        while (a < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) && hits < 40) {
            const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
            const bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
            if (rw) {
                for (uintptr_t q = lo; q + 4 <= hi && hits < 40; q += 4) {
                    uint32_t w = 0;
                    if (!safe_read_any(q, &w, 4)) break;
                    if (w == v) {
                        out("  findptr %08X at %08X", v, static_cast<unsigned>(q));
                        ++hits;
                    }
                }
            }
            a = hi;
        }
        out("findptr %08X: %d places", v, hits);
    } else if (c == "chrs") {
        // Characters by class (PlayerIns, ReplayGhostIns), found once by their vtable and kept: position, the class
        // of the manipulator the character holds (the first pointer in its first 0x200 bytes to an object with a
        // manipulator vtable) and its chr type word at +0x70. "chrs scan" searches again.
        std::vector<uintptr_t>& found = g_chrs_found;
        struct Known {
            uint32_t vt;
            const char* name;
        };
        static const Known kChr[] = {{0x0118A5D4, "PlayerIns"}, {0x0118A974, "ReplayGhostIns"}};
        static const Known kMan[] = {{0x0118DDEC, "NetworkManipulator"}, {0x0118E324, "ReplayManipulator"},
                                     {0x0118E23C, "PadManipulator"},     {0x0118DCCC, "NetAIManipulator"},
                                     {0x0118DB64, "ComManipulator"},     {0x0118D9EC, "ChrManipulator"}};
        if (found.empty() || (t.size() > 1 && t[1] == "scan")) {
            found.clear();
            MEMORY_BASIC_INFORMATION mbi{};
            uintptr_t a = 0x10000;
            while (a < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) && found.size() < 64) {
                const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
                const bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                                (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
                if (rw && lo >= 0x01400000) {
                    for (uintptr_t q = lo; q + 4 <= hi && found.size() < 64; q += 16) {
                        uint32_t w = 0;
                        if (!safe_read_any(q, &w, 4)) break;
                        for (const Known& k : kChr) {
                            if (w == k.vt) found.push_back(q);
                        }
                    }
                }
                a = hi;
            }
        }
        for (uintptr_t chr : found) {
            uint32_t vt = 0, ctrl = 0, phys = 0;
            float p[3] = {};
            if (!safe_read_any(chr, &vt, 4)) continue;
            const char* cls = "?";
            for (const Known& k : kChr) {
                if (vt == k.vt) cls = k.name;
            }
            if (*cls == '?') continue;  // freed or reused
            safe_read_any(chr + 0x28, &ctrl, 4);
            if (ctrl) safe_read_any(ctrl + 0x1C, &phys, 4);
            if (phys) safe_read_any(phys + 0x10, p, 12);
            const char* man = "-";
            uint32_t man_at = 0;
            for (uint32_t off = 4; off < 0x200 && *man == '-'; off += 4) {
                uint32_t ptr = 0, pvt = 0;
                if (!safe_read_any(chr + off, &ptr, 4) || ptr < 0x10000 || !safe_read_any(ptr, &pvt, 4)) continue;
                for (const Known& k : kMan) {
                    if (pvt == k.vt) {
                        man = k.name;
                        man_at = off;
                    }
                }
            }
            int type = 0;
            safe_read_any(chr + 0x70, &type, 4);
            out("  chr %08X %s pos %.2f %.2f %.2f type %d man %s (+0x%X)", static_cast<unsigned>(chr), cls, p[0], p[1],
                p[2], type, man, man_at);
        }
        out("chrs: %u", static_cast<unsigned>(found.size()));
    } else if (c == "peek" && t.size() > 1) {
        const uintptr_t a = std::strtoul(t[1].c_str(), nullptr, 16);
        const int n = static_cast<int>(num(2, 16));
        for (int i = 0; i < n && i < 256; i += 4) {
            uint32_t w[4] = {};
            if (!safe_read_any(a + i * 4, w, 16)) break;
            float f[4];
            std::memcpy(f, w, 16);
            out("  %08X: %08X %08X %08X %08X | %g %g %g %g", static_cast<unsigned>(a + i * 4), w[0], w[1], w[2], w[3],
                f[0], f[1], f[2], f[3]);
        }
    } else if (c == "poke" && t.size() > 2) {
        // poke ADDR VALUE [BYTES]: write 1 (default), 2 or 4 bytes (hex address, value as written: 0x.. or decimal).
        // E.g. the game's debug switches: 13784D2 PlayerNoDead, 13784EA AllNoDamage, 13784E9 AllNoDead.
        const uintptr_t a = std::strtoul(t[1].c_str(), nullptr, 16);
        const uint32_t v = static_cast<uint32_t>(std::strtoul(t[2].c_str(), nullptr, 0));
        const int n = static_cast<int>(num(3, 1));
        uint32_t before = 0;
        DWORD old = 0;
        if ((n == 1 || n == 2 || n == 4) && safe_read_any(a, &before, n) &&
            VirtualProtect(reinterpret_cast<void*>(a), n, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(reinterpret_cast<void*>(a), &v, n);
            VirtualProtect(reinterpret_cast<void*>(a), n, old, &old);
            out("poke %08X: %X -> %X (%d bytes)", static_cast<unsigned>(a), before, v, n);
        } else {
            out("poke %08X: not writable", static_cast<unsigned>(a));
        }
    } else if (c == "watch" && t.size() > 1) {
        const uint32_t addr = static_cast<uint32_t>(std::strtoul(t[1].c_str(), nullptr, 16));
        watch_arm_addresses(&addr, 1);
        out("watch %08X armed (writers go to the main log on watchreport)", addr);
    } else if (c == "watchreport") {
        watch_report_now();
        out("watchreport: see PTDE-FPS-Unlock.log");
    } else if (c == "scanval") {
        g_scan_reg = static_cast<LONG>(num(1, 8));
        out("scanval: c%ld at the next %s upload (while a frame capture runs)", num(1, 8), g_vb_ps.c_str());
    } else if (c == "vbwatch") {
        g_vb_ps = t.size() > 1 ? t[1] : "";
        out("vbwatch %s", g_vb_ps.c_str());
    } else if (c == "dof") {
        resolution_set_smooth_dof(num(1, 1) != 0);
        out("dof %s", num(1, 1) != 0 ? "smooth (ours)" : "the game's");
    } else if (c == "dofstrength") {
        // dofstrength PCT: DepthOfFieldStrength for this session (100 = the game's).
        resolution_set_dof_strength(static_cast<float>(num(1, 100)) / 100.0f);
        out("dofstrength %ld%%", num(1, 100));
    } else if (c == "dofhr") {
        // dofhr PCT: the half-size depth of field with its widths at PCT percent of the game's (0: the game's levels).
        resolution_set_dof_resolution(static_cast<int>(num(1, 100)));
        out("dofhr %ld", num(1, 100));
    } else if (c == "effects") {
        // effects 0/1/2/3: the game's low-resolution effects, ours at full resolution (8-bit), ours in float + dither,
        // ours at half resolution.
        resolution_set_effects(static_cast<int>(num(1, 3)));
        out("effects %ld", num(1, 3));
    } else if (c == "atoc") {
        // atoc 0/1/2: AlphaToCoverage off, plain coverage, sharpened coverage (the default) for this session.
        atoc_set_mode(static_cast<int>(num(1, 2)));
        out("atoc %ld", num(1, 2));
    } else if (c == "dofforce") {
        resolution_force_dof_level(static_cast<int>(num(1, 0)));
        out("dofforce %ld", num(1, 0));
    } else if (c == "god") {
        g_god = num(1, 1) != 0;
        out("god %d", g_god ? 1 : 0);
    } else if (c == "shot" && t.size() > 1) {
        g_shot_name = t[1];
        g_shot_requested = true;
    } else if (c == "rtdump" && t.size() > 1) {
        g_dump_name = t[1];
        g_dump_requested = true;
    } else if (c == "frame" && t.size() > 1) {
        g_frame_name = t[1];
        g_frame_requested = true;
    } else if (c == "capture") {
        profile_request_capture();
        out("capture requested (the [prof] lines go to the main log in about 12 s)");
    } else if (c == "swapoff") {
        resolution_set_swap_off_mask(static_cast<int>(num(1, 0)));
        out("swapoff 0x%lX", num(1, 0));
    } else if (c == "reduce") {
        resolution_set_present_reduce(num(1, 1) != 0);
        out("reduce %ld", num(1, 1));
    } else if (c == "fakefocus") {
        set_fake_focus(num(1, 1) != 0);
        out("fakefocus %d", g_fake_focus ? 1 : 0);
    } else if (c == "hp") {
        int hp[2] = {};
        const uintptr_t chr = player_chr();
        if (chr && safe_read(chr + 0x2D4, hp, 8)) {
            out("hp %d / %d", hp[0], hp[1]);
        } else {
            out("hp: no player");
        }
    } else if (c == "flag" || c == "setflag" || c == "tp" || c == "warp" || c == "speffect" ||
               c == "anim") {
        std::lock_guard<std::mutex> lock(g_game_lock);
        g_game_cmds.push_back(t);
    } else if (c == "queries") {
        g_query_frames = static_cast<LONG>(num(1, 10));
        out("queries: logging occlusion results for %ld frames", num(1, 10));
    } else {
        out("unknown command: %s", line.c_str());
    }
}

DWORD WINAPI poll_thread(void*) {
    wchar_t path[MAX_PATH];
    std::swprintf(path, MAX_PATH, L"%s\\fpsu-cmd.txt", g_dir);
    for (;;) {
        Sleep(100);
        FILE* f = _wfopen(path, L"rb");
        if (!f) continue;
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        DeleteFileW(path);
        size_t i = 0;
        while (i < text.size()) {
            size_t j = text.find('\n', i);
            if (j == std::string::npos) j = text.size();
            std::string line = text.substr(i, j - i);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) run(line);
            i = j + 1;
        }
    }
}

}  // namespace

std::string remote_shader_name(void* shader) { return shader_name(shader); }

bool remote_start() {
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;
    g_t0 = GetTickCount64();
    g_on = true;
    out("remote control on (build %s %s)", __DATE__, __TIME__);
    HANDLE h = CreateThread(nullptr, 0, &poll_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    LOG_INFO("RemoteControl: commands from fpsu-cmd.txt, results in fpsu-remote.txt (development only)");
    return true;
}

bool remote_enabled() { return g_on; }

void remote_hook_device(IDirect3DDevice9* dev) {
    if (!g_on || !dev) return;
    HMODULE x = GetModuleHandleW(L"d3dx9_43.dll");
    if (!x) x = LoadLibraryW(L"d3dx9_43.dll");
    if (x) {
        x_save_surface = reinterpret_cast<SaveSurfaceFn>(GetProcAddress(x, "D3DXSaveSurfaceToFileW"));
        x_disassemble = reinterpret_cast<DisassembleFn>(GetProcAddress(x, "D3DXDisassembleShader"));
    }
    hook(dev, kPresent, o_present, reinterpret_cast<void*>(&hk_present));
    hook(dev, kCreateTexture, o_create_texture, reinterpret_cast<void*>(&hk_create_texture));
    hook(dev, kCreateRenderTarget, o_create_rt, reinterpret_cast<void*>(&hk_create_rt));
    hook(dev, kCreateDepth, o_create_depth, reinterpret_cast<void*>(&hk_create_depth));
    hook(dev, kGetRTData, o_get_rt_data, reinterpret_cast<void*>(&hk_get_rt_data));
    hook(dev, kStretchRect, o_stretch, reinterpret_cast<void*>(&hk_stretch));
    hook(dev, kColorFill, o_color_fill, reinterpret_cast<void*>(&hk_color_fill));
    hook(dev, kSetRenderTarget, o_set_rt, reinterpret_cast<void*>(&hk_set_rt));
    hook(dev, kSetDepth, o_set_depth, reinterpret_cast<void*>(&hk_set_depth));
    hook(dev, kClear, o_clear, reinterpret_cast<void*>(&hk_clear));
    hook(dev, kSetViewport, o_set_viewport, reinterpret_cast<void*>(&hk_set_viewport));
    hook(dev, kSetRenderState, o_set_rs, reinterpret_cast<void*>(&hk_set_rs));
    hook(dev, kSetTexture, o_set_texture, reinterpret_cast<void*>(&hk_set_texture));
    hook(dev, kSetScissor, o_set_scissor, reinterpret_cast<void*>(&hk_set_scissor));
    hook(dev, kDrawPrim, o_draw, reinterpret_cast<void*>(&hk_draw));
    hook(dev, kDrawIdxPrim, o_draw_idx, reinterpret_cast<void*>(&hk_draw_idx));
    hook(dev, kDrawPrimUP, o_draw_up, reinterpret_cast<void*>(&hk_draw_up));
    hook(dev, kDrawIdxPrimUP, o_draw_idx_up, reinterpret_cast<void*>(&hk_draw_idx_up));
    hook(dev, kCreateVS, o_create_vs, reinterpret_cast<void*>(&hk_create_vs));
    hook(dev, kSetVS, o_set_vs, reinterpret_cast<void*>(&hk_set_vs));
    hook(dev, kCreatePS, o_create_ps, reinterpret_cast<void*>(&hk_create_ps));
    hook(dev, kSetPS, o_set_ps, reinterpret_cast<void*>(&hk_set_ps));
    hook(dev, kSetPSConstF, o_set_ps_const, reinterpret_cast<void*>(&hk_set_ps_const));
    hook(dev, kSetVSConstF, o_set_vs_const, reinterpret_cast<void*>(&hk_set_vs_const));
    hook(dev, kCreateQuery, o_create_query, reinterpret_cast<void*>(&hk_create_query));
    out("device hooked (d3dx %s)", x_save_surface ? "found" : "MISSING");
}

void remote_pad(DWORD index, void* state, DWORD* result) {
    if (!g_on || index != 0 || !state || !result) return;
    std::lock_guard<std::mutex> lock(g_pad_lock);
    const ULONGLONG now = GetTickCount64();
    // Only while a command holds input: otherwise slot 0 reads exactly as the real one (a player's own pad on
    // another slot must not lose to an always-connected idle slot 0).
    // and for 30 s after it, so a sequence of presses does not reconnect the pad each time (the game ignores a
    // freshly connected pad briefly).
    if (now >= g_pad_until + 30000 && now >= g_stick_until + 30000) return;
    auto* s = static_cast<XState*>(state);
    if (*result != 0) {
        std::memset(s, 0, sizeof(*s));
        *result = 0;
    }
    if (now < g_pad_until) {
        s->buttons |= g_pad_buttons;
        if (g_pad_lt) s->lt = g_pad_lt;
        if (g_pad_rt) s->rt = g_pad_rt;
    }
    if (now < g_stick_until) {
        if (g_stick[0] || g_stick[1]) { s->lx = g_stick[0]; s->ly = g_stick[1]; }
        if (g_stick[2] || g_stick[3]) { s->rx = g_stick[2]; s->ry = g_stick[3]; }
    }
    s->packet += ++g_packet;
}

void remote_game_frame() {
    if (!g_on) return;
    if (g_god) god_tick();
    if (g_hold_until && GetTickCount64() < g_hold_until) {
        const uintptr_t phys = player_physics();
        if (phys) std::memcpy(reinterpret_cast<void*>(phys + 0x10), g_hold_pos, sizeof(g_hold_pos));
    }
    std::vector<std::vector<std::string>> cmds;
    {
        std::lock_guard<std::mutex> lock(g_game_lock);
        if (g_game_cmds.empty()) return;
        cmds.swap(g_game_cmds);
    }
    for (const auto& t : cmds) run_game(t);
}

#else  // FPSU_DIAGNOSTICS
std::string remote_shader_name(void*) { return "none"; }

void remote_game_frame() {}
bool remote_start() { return false; }
bool remote_enabled() { return false; }
void remote_hook_device(IDirect3DDevice9*) {}
void remote_pad(DWORD, void*, DWORD*) {}

#endif  // FPSU_DIAGNOSTICS
