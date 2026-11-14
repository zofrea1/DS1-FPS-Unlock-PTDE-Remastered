// AlphaToCoverage. Grass, leaves, hair and fences are alpha-tested: each pixel is either drawn or not by its
// texture's alpha, so their edges are stair-stepped and thin blades far away break up into dots. Multisampling (the
// game's own Antialiasing option: a 2x multisampled scene, AntialiasingSamples for more) smooths the edges of
// geometry but not these: the test is made once per pixel. Alpha to coverage turns the alpha into the share of the
// pixel's samples covered instead, so those edges come out as smooth as any other.
//
// Direct3D 9 has no switch for it; the graphics drivers added one each: NVIDIA's render state ADAPTIVETESS_Y =
// 'ATOC' (alpha-tested draws use coverage while it is set), AMD's POINTSIZE = 'A2M1' (on) / 'A2M0' (off). DXVK
// understands both. Before each draw, coverage is on when the draw is alpha-tested, not blended, and goes into a
// multisampled target, and off otherwise (the shadow maps, the effects and the HUD are drawn as the game drew them).
//
// Coverage straight from the texture's alpha makes the blades see-through: textures made for an alpha test have
// alpha well below 1 inside the shape (anything above the test's reference was drawn solid). So for such a draw the
// pixel shader is swapped for a copy whose alpha is sharpened around the reference first (Ben Golus, "Anti-aliased
// Alpha Test: The Esoteric Alpha To Coverage", 2017): alpha' = saturate((alpha - ref) / fwidth(alpha) + 0.5). Solid
// inside, the test's own outline, and a ramp one pixel wide across it that the samples turn into a smooth edge. The
// copy is the game's shader with its colour output (oC0) written to a spare temporary register and, at the end, those
// instructions and the move to oC0 added (c210 = ref, a floor for fwidth, 0.5). A shader that cannot be copied this
// way (subroutines, no spare register) gets plain coverage.
#include "atoc.h"

#include "d3dperf.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr DWORD kNvOn = MAKEFOURCC('A', 'T', 'O', 'C');
constexpr DWORD kAmdOn = MAKEFOURCC('A', '2', 'M', '1');
constexpr DWORD kAmdOff = MAKEFOURCC('A', '2', 'M', '0');
constexpr UINT kSharpenConst = 210;  // no game pixel shader uses a register above c183; ours use c200-c203

using SetRTFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetPSFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using DrawFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUPFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedUPFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                         const void*, UINT);
SetRTFn o_set_rt = nullptr;
SetPSFn o_set_ps = nullptr;
DrawFn o_draw = nullptr;
DrawIndexedFn o_draw_indexed = nullptr;
DrawUPFn o_draw_up = nullptr;
DrawIndexedUPFn o_draw_indexed_up = nullptr;

enum class Method { None, Nvidia, Amd };
Method g_method = Method::None;
volatile LONG g_mode = 2;     // 0 off, 1 plain coverage, 2 sharpened (RemoteControl `atoc`)
bool g_multisampled = false;  // render target 0 is multisampled
bool g_amd_on = false;        // AMD's switch as we last set it (POINTSIZE does not read back as the switch)
IDirect3DPixelShader9* g_game_ps = nullptr;  // the pixel shader the game bound
bool g_logged_target = false;
bool g_logged_on = false;

// The sharpened copies, by the game's shader (held, so its address is not reused for another). nullptr: not copyable.
std::mutex g_lock;
std::unordered_map<IDirect3DPixelShader9*, IDirect3DPixelShader9*> g_variants;
LONG g_made = 0, g_refused = 0;

// --- ps_3_0 bytecode -----------------------------------------------------------------------------------------------
constexpr DWORD kEnd = 0x0000FFFF;
constexpr DWORD kRegTemp = 0, kRegConst = 2, kRegColorOut = 8;

DWORD reg_type(DWORD p) { return ((p >> 28) & 7) | ((p >> 8) & 0x18); }
DWORD dst(DWORD type, DWORD num, DWORD mask, bool sat = false) {
    return 0x80000000 | ((type & 7) << 28) | ((type & 0x18) << 8) | (mask << 16) | (sat ? (1u << 20) : 0) | num;
}
DWORD src(DWORD type, DWORD num, DWORD swizzle, DWORD mod = 0) {
    return 0x80000000 | ((type & 7) << 28) | ((type & 0x18) << 8) | (swizzle << 16) | (mod << 24) | num;
}
constexpr DWORD kX = 0x00, kY = 0x55, kZ = 0xAA, kW = 0xFF, kXYZW = 0xE4;
constexpr DWORD kNeg = 1, kAbs = 11;

// The game's ps_3_0 with oC0 written to a spare temporary and the sharpened alpha added at the end. False when the
// shader is not one this can copy.
bool sharpen(const DWORD* code, size_t count, std::vector<DWORD>& out) {
    if (count < 2 || code[0] != 0xFFFF0300) return false;
    int max_temp = -1;
    bool writes = false;
    size_t end = 0;
    for (size_t i = 1; i < count;) {
        const DWORD tok = code[i];
        if (tok == kEnd) {
            end = i;
            break;
        }
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {  // comment
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if (op == 0x19 || op == 0x1A || op == 0x1C || op == 0x1E) return false;  // call, callnz, ret, label
        for (DWORD k = 1; k <= len && i + k < count; ++k) {
            const DWORD p = code[i + k];
            if ((op == 0x51 || op == 0x30 || op == 0x2F) && k > 1) continue;  // def/defi/defb literals
            if (op == 0x1F && k == 1) continue;                                  // dcl's usage token
            const DWORD type = reg_type(p);
            if (type == kRegTemp) max_temp = max_temp > static_cast<int>(p & 0x7FF) ? max_temp : static_cast<int>(p & 0x7FF);
            if (type == kRegColorOut && (p & 0x7FF) == 0) writes = true;
        }
        i += 1 + len;
    }
    if (!end || !writes || max_temp + 2 > 31) return false;
    const DWORD n = static_cast<DWORD>(max_temp + 1), u = static_cast<DWORD>(max_temp + 2);
    out.assign(code, code + end);
    for (size_t i = 1; i < out.size();) {
        const DWORD tok = out[i];
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        for (DWORD k = 1; k <= len && i + k < out.size(); ++k) {
            DWORD& p = out[i + k];
            if ((op == 0x51 || op == 0x30 || op == 0x2F) && k > 1) continue;
            if (op == 0x1F && k == 1) continue;
            if (reg_type(p) == kRegColorOut && (p & 0x7FF) == 0) {
                // oC0 -> rN, keeping the write mask and modifiers.
                p = (p & ~(0x70000000u | 0x00001800u | 0x7FFu)) | n;
            }
        }
        i += 1 + len;
    }
    const DWORD tail[] = {
        0x0200005B, dst(kRegTemp, u, 1), src(kRegTemp, n, kW),                                  // dsx u.x, n.w
        0x0200005C, dst(kRegTemp, u, 2), src(kRegTemp, n, kW),                                  // dsy u.y, n.w
        0x03000002, dst(kRegTemp, u, 1), src(kRegTemp, u, kX, kAbs), src(kRegTemp, u, kY, kAbs),  // u.x = fwidth
        0x0300000B, dst(kRegTemp, u, 1), src(kRegTemp, u, kX), src(kRegConst, kSharpenConst, kY), // max with floor
        0x02000006, dst(kRegTemp, u, 1), src(kRegTemp, u, kX),                                  // rcp
        0x03000002, dst(kRegTemp, u, 2), src(kRegTemp, n, kW), src(kRegConst, kSharpenConst, kX, kNeg),  // a - ref
        0x04000004, dst(kRegTemp, n, 8, true), src(kRegTemp, u, kY), src(kRegTemp, u, kX),
        src(kRegConst, kSharpenConst, kZ),                                                       // sat(.. + 0.5)
        0x02000001, dst(kRegColorOut, 0, 0xF), src(kRegTemp, n, kXYZW),                        // mov oC0, rN
    };
    out.insert(out.end(), std::begin(tail), std::end(tail));
    out.push_back(kEnd);
    return true;
}

IDirect3DPixelShader9* variant_of(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_variants.find(ps);
    if (it != g_variants.end()) return it->second;
    IDirect3DPixelShader9* made = nullptr;
    UINT size = 0;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size % 4 == 0) {
        std::vector<DWORD> code(size / 4), out;
        if (SUCCEEDED(ps->GetFunction(code.data(), &size)) && sharpen(code.data(), code.size(), out) &&
            SUCCEEDED(dev->CreatePixelShader(out.data(), &made)) && made) {
            ++g_made;
        } else {
            made = nullptr;
            ++g_refused;
        }
    }
    ps->AddRef();
    g_variants[ps] = made;
    if (g_made + g_refused <= 3 || (g_made + g_refused) % 50 == 0) {
        LOG_INFO("AlphaToCoverage: %ld alpha-tested pixel shaders sharpened, %ld given plain coverage", g_made,
                 g_refused);
    }
    return made;
}

// A render state as the device holds it: from the render-state filter when it knows (three of these were read back
// from Direct3D for every draw into the multisampled scene, some 7,000 calls a frame in a busy view), else asked.
DWORD render_state(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state) {
    unsigned long v = 0;
    if (d3dperf_render_state(static_cast<unsigned long>(state), &v)) return v;
    DWORD d = 0;
    dev->GetRenderState(state, &d);
    return d;
}

// Before a draw: coverage on or off, and the sharpened shader in place (true: the game's goes back after the draw).
bool before_draw(IDirect3DDevice9* dev) {
    if (g_method == Method::None) return false;
    const LONG mode = g_mode;
    bool want = false;
    if (g_multisampled && mode) {
        want = render_state(dev, D3DRS_ALPHATESTENABLE) && !render_state(dev, D3DRS_ALPHABLENDENABLE);
    }
    if (g_method == Method::Nvidia) {
        // As the device holds it each time: a state block the game applies may have changed it.
        const DWORD now = render_state(dev, D3DRS_ADAPTIVETESS_Y);
        if ((now == kNvOn) != want) dev->SetRenderState(D3DRS_ADAPTIVETESS_Y, want ? kNvOn : D3DFMT_UNKNOWN);
    } else if (want != g_amd_on) {
        dev->SetRenderState(D3DRS_POINTSIZE, want ? kAmdOn : kAmdOff);
        g_amd_on = want;
    }
    if (want && !g_logged_on) {
        g_logged_on = true;
        LOG_INFO("AlphaToCoverage: alpha-tested surfaces now write coverage (first draw)");
    }
    if (!want || mode != 2 || !g_game_ps || !o_set_ps) return false;
    const DWORD func = render_state(dev, D3DRS_ALPHAFUNC);
    if (func != D3DCMP_GREATEREQUAL && func != D3DCMP_GREATER) return false;
    IDirect3DPixelShader9* sharp = variant_of(dev, g_game_ps);
    if (!sharp) return false;
    const DWORD ref = render_state(dev, D3DRS_ALPHAREF);
    const float c[4] = {static_cast<float>(ref & 0xFF) / 255.0f, 1.0f / 1024.0f, 0.5f, 0.0f};
    dev->SetPixelShaderConstantF(kSharpenConst, c, 1);
    o_set_ps(dev, sharp);
    return true;
}

void after_draw(IDirect3DDevice9* dev, bool swapped) {
    if (swapped) o_set_ps(dev, g_game_ps);
}

HRESULT WINAPI hk_set_rt(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* rt) {
    if (index == 0) {
        D3DSURFACE_DESC d{};
        g_multisampled = rt && SUCCEEDED(rt->GetDesc(&d)) && d.MultiSampleType != D3DMULTISAMPLE_NONE;
        if (g_multisampled && !g_logged_target) {
            g_logged_target = true;
            const int samples = d.MultiSampleType == D3DMULTISAMPLE_NONMASKABLE ? 0 : static_cast<int>(d.MultiSampleType);
            LOG_INFO("AlphaToCoverage: the game's anti-aliasing draws the scene %ux%u with %dx multisampling (quality "
                     "%lu); its alpha-tested surfaces get coverage", d.Width, d.Height, samples,
                     static_cast<unsigned long>(d.MultiSampleQuality));
        }
    }
    return o_set_rt(dev, index, rt);
}

HRESULT WINAPI hk_set_ps(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    g_game_ps = ps;
    return o_set_ps(dev, ps);
}

HRESULT WINAPI hk_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    const bool s = before_draw(dev);
    const HRESULT hr = o_draw(dev, type, start, count);
    after_draw(dev, s);
    return hr;
}

HRESULT WINAPI hk_draw_indexed(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT vertices,
                               UINT start, UINT count) {
    const bool s = before_draw(dev);
    const HRESULT hr = o_draw_indexed(dev, type, base, min_index, vertices, start, count);
    after_draw(dev, s);
    return hr;
}

HRESULT WINAPI hk_draw_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    const bool s = before_draw(dev);
    const HRESULT hr = o_draw_up(dev, type, count, data, stride);
    after_draw(dev, s);
    return hr;
}

HRESULT WINAPI hk_draw_indexed_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT min_index, UINT vertices,
                                  UINT count, const void* indices, D3DFORMAT format, const void* data, UINT stride) {
    const bool s = before_draw(dev);
    const HRESULT hr = o_draw_indexed_up(dev, type, min_index, vertices, count, indices, format, data, stride);
    after_draw(dev, s);
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

// The game's multisample types by its setting (0x10D38CC: none, 2x, 4x, ...); its Antialiasing option uses entry 1.
constexpr uintptr_t kMsaaTable = 0x010D38CC;

}  // namespace

void atoc_hook_device(IDirect3DDevice9* dev, bool force) {
    if (!dev) return;
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return;
    dev->GetCreationParameters(&cp);
    const D3DFORMAT adapter = SUCCEEDED(dev->GetDisplayMode(0, &mode)) && mode.Format != D3DFMT_UNKNOWN
                                  ? mode.Format
                                  : D3DFMT_X8R8G8B8;
    const auto supported = [&](DWORD fourcc) {
        return d3d->CheckDeviceFormat(cp.AdapterOrdinal, D3DDEVTYPE_HAL, adapter, 0, D3DRTYPE_SURFACE,
                                      static_cast<D3DFORMAT>(fourcc)) == D3D_OK;
    };
    g_method = supported(kNvOn) ? Method::Nvidia : (supported(kAmdOn) ? Method::Amd : Method::None);
    d3d->Release();
    if (g_method == Method::None && force) {
        g_method = Method::Nvidia;
        LOG_INFO("AlphaToCoverage = force: the graphics driver reports neither NVIDIA's nor AMD's switch; NVIDIA's is "
                 "used anyway (if the edges of grass and leaves do not get smoother, the driver ignores it)");
    }
    if (g_method == Method::None) {
        LOG_INFO("AlphaToCoverage: the graphics driver offers neither NVIDIA's nor AMD's switch for it; not used");
        return;
    }
    hook(dev, 37, reinterpret_cast<void*>(&hk_set_rt), o_set_rt);
    hook(dev, 107, reinterpret_cast<void*>(&hk_set_ps), o_set_ps);
    hook(dev, 81, reinterpret_cast<void*>(&hk_draw), o_draw);
    hook(dev, 82, reinterpret_cast<void*>(&hk_draw_indexed), o_draw_indexed);
    hook(dev, 83, reinterpret_cast<void*>(&hk_draw_up), o_draw_up);
    hook(dev, 84, reinterpret_cast<void*>(&hk_draw_indexed_up), o_draw_indexed_up);
    LOG_INFO("AlphaToCoverage: ready (%s switch, edges sharpened around the alpha test); takes effect when the game's "
             "anti-aliasing is on (Antialiasing = 1 in DarkSouls.ini, or the in-game option)",
             g_method == Method::Nvidia ? "NVIDIA's" : "AMD's");
}

void atoc_set_mode(int mode) { g_mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode); }

bool atoc_set_msaa_samples(int samples) {
    if (samples != 2 && samples != 4 && samples != 8) return false;
    auto* entry = reinterpret_cast<uint32_t*>(kMsaaTable + 4);
    if (*entry != 2) {
        LOG_ERROR("AntialiasingSamples: the game's multisample table at %08X holds %u, not 2 in this build. Not "
                  "changing it.", static_cast<unsigned>(kMsaaTable + 4), *entry);
        return false;
    }
    if (samples == 2) return true;
    DWORD old = 0;
    if (!VirtualProtect(entry, 4, PAGE_READWRITE, &old)) return false;
    *entry = static_cast<uint32_t>(samples);
    DWORD ignored = 0;
    VirtualProtect(entry, 4, old, &ignored);
    LOG_INFO("AntialiasingSamples: the game's anti-aliasing (when on) multisamples the scene %dx (the game's own: 2x)",
             samples);
    return true;
}
