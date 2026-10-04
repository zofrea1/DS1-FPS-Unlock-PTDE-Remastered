#include "gamma.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kPresentSlot = 17;       // IDirect3DDevice9::Present
constexpr int kSetGammaRampSlot = 21;  // IDirect3DDevice9::SetGammaRamp
constexpr int kEndSceneSlot = 42;      // IDirect3DDevice9::EndScene

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetGammaRampFn = void(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, const D3DGAMMARAMP*);
using EndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);

// ps_2_0, compiled with fxc /T ps_2_0 /O3 from:
//   sampler2D frame : register(s0);
//   sampler2D ramp : register(s1);
//   float4 main(float2 uv : TEXCOORD0) : COLOR {
//       float4 c = tex2D(frame, uv);
//       const float scale = 255.0 / 256.0;
//       const float offset = 0.5 / 256.0;
//       float r = tex2D(ramp, float2(c.r * scale + offset, 0.5)).r;
//       float g = tex2D(ramp, float2(c.g * scale + offset, 0.5)).g;
//       float b = tex2D(ramp, float2(c.b * scale + offset, 0.5)).b;
//       return float4(r, g, b, c.a);
//   }
const DWORD kRampShader[] = {
    0xFFFF0200, 0x002AFFFE, 0x42415443, 0x0000001C, 0x0000007B, 0xFFFF0200,
    0x00000002, 0x0000001C, 0x00008100, 0x00000074, 0x00000044, 0x00000003,
    0x00020001, 0x0000004C, 0x00000000, 0x0000005C, 0x00010003, 0x00060001,
    0x00000064, 0x00000000, 0x6D617266, 0xABAB0065, 0x000C0004, 0x00010001,
    0x00000001, 0x00000000, 0x706D6172, 0xABABAB00, 0x000C0004, 0x00010001,
    0x00000001, 0x00000000, 0x325F7370, 0x4D00305F, 0x6F726369, 0x74666F73,
    0x29522820, 0x534C4820, 0x6853204C, 0x72656461, 0x6D6F4320, 0x656C6970,
    0x30312072, 0xAB00312E, 0x05000051, 0xA00F0000, 0x3F7F0000, 0x3B000000,
    0x3F000000, 0x00000000, 0x0200001F, 0x80000000, 0xB0030000, 0x0200001F,
    0x90000000, 0xA00F0800, 0x0200001F, 0x90000000, 0xA00F0801, 0x03000042,
    0x800F0000, 0xB0E40000, 0xA0E40800, 0x02000001, 0x80020001, 0xA0AA0000,
    0x04000004, 0x80010001, 0x80000000, 0xA0000000, 0xA0550000, 0x02000001,
    0x80020002, 0xA0AA0000, 0x04000004, 0x80010002, 0x80550000, 0xA0000000,
    0xA0550000, 0x04000004, 0x80010000, 0x80AA0000, 0xA0000000, 0xA0550000,
    0x02000001, 0x80020000, 0xA0AA0000, 0x03000042, 0x800F0001, 0x80E40001,
    0xA0E40801, 0x03000042, 0x800F0002, 0x80E40002, 0xA0E40801, 0x03000042,
    0x800F0003, 0x80E40000, 0xA0E40801, 0x02000001, 0x80010003, 0x80000001,
    0x02000001, 0x80020003, 0x80550002, 0x02000001, 0x80080003, 0x80FF0000,
    0x02000001, 0x800F0800, 0x80E40003, 0x0000FFFF,
};

bool g_enabled = false;
IDirect3DDevice9* g_device = nullptr;  // the device that presents (not owned)
PresentFn g_present = nullptr;
SetGammaRampFn g_set_gamma_ramp = nullptr;
EndSceneFn g_end_scene = nullptr;
// Something was drawn since the last present. The game's catch-up loop presents the same frame again without
// drawing, and that frame already carries the ramp.
bool g_new_content = true;

D3DGAMMARAMP g_ramp{};
bool g_ramp_set = false;
bool g_identity = true;
bool g_lut_dirty = true;
bool g_windowed = false;
bool g_windowed_known = false;
int g_frames_since_mode_check = 0;
int g_failures = 0;
bool g_logged_active = false;

// Device resources: the copy of the frame (default pool, released before a reset), the ramp texture
// (managed pool), the shader and the saved state.
IDirect3DTexture9* g_frame = nullptr;
UINT g_frame_width = 0, g_frame_height = 0;
D3DFORMAT g_frame_format = D3DFMT_UNKNOWN;
IDirect3DTexture9* g_lut = nullptr;
IDirect3DPixelShader9* g_shader = nullptr;
IDirect3DStateBlock9* g_state = nullptr;

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

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

bool ramp_is_identity(const D3DGAMMARAMP& r) {
    for (int i = 0; i < 256; ++i) {
        const int identity = i * 257;
        if (std::abs(static_cast<int>(r.red[i]) - identity) > 256 || std::abs(static_cast<int>(r.green[i]) - identity) > 256 ||
            std::abs(static_cast<int>(r.blue[i]) - identity) > 256) {
            return false;
        }
    }
    return true;
}

void read_display_mode(IDirect3DDevice9* device) {
    IDirect3DSwapChain9* swapchain = nullptr;
    if (FAILED(device->GetSwapChain(0, &swapchain)) || !swapchain) {
        return;
    }
    D3DPRESENT_PARAMETERS pp{};
    if (SUCCEEDED(swapchain->GetPresentParameters(&pp))) {
        const bool windowed = pp.Windowed != FALSE;
        if (!g_windowed_known || windowed != g_windowed) {
            LOG_INFO("Gamma: the swapchain is %s %ux%u; the game's brightness ramp is applied by %s", windowed ? "windowed" : "fullscreen",
                     pp.BackBufferWidth, pp.BackBufferHeight, windowed ? "this mod (WindowedGamma)" : "Direct3D itself");
        }
        g_windowed = windowed;
        g_windowed_known = true;
    }
    swapchain->Release();
}

bool fill_lut() {
    D3DLOCKED_RECT locked{};
    if (FAILED(g_lut->LockRect(0, &locked, nullptr, 0))) {
        return false;
    }
    auto* texels = static_cast<uint32_t*>(locked.pBits);
    for (int i = 0; i < 256; ++i) {
        texels[i] = 0xFF000000u | (static_cast<uint32_t>(g_ramp.red[i] >> 8) << 16) | (static_cast<uint32_t>(g_ramp.green[i] >> 8) << 8) |
                    static_cast<uint32_t>(g_ramp.blue[i] >> 8);
    }
    g_lut->UnlockRect(0);
    g_lut_dirty = false;
    return true;
}

bool ensure_resources(IDirect3DDevice9* device, const D3DSURFACE_DESC& back) {
    if (!g_shader && FAILED(device->CreatePixelShader(kRampShader, &g_shader))) {
        return false;
    }
    if (!g_lut) {
        if (FAILED(device->CreateTexture(256, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_lut, nullptr))) {
            return false;
        }
        g_lut_dirty = true;
    }
    if (g_lut_dirty && !fill_lut()) {
        return false;
    }
    if (g_frame && (g_frame_width != back.Width || g_frame_height != back.Height || g_frame_format != back.Format)) {
        release(g_frame);
    }
    if (!g_frame) {
        if (FAILED(device->CreateTexture(back.Width, back.Height, 1, D3DUSAGE_RENDERTARGET, back.Format, D3DPOOL_DEFAULT, &g_frame,
                                         nullptr))) {
            return false;
        }
        g_frame_width = back.Width;
        g_frame_height = back.Height;
        g_frame_format = back.Format;
    }
    if (!g_state && FAILED(device->CreateStateBlock(D3DSBT_ALL, &g_state))) {
        return false;
    }
    return true;
}

struct Vertex {
    float x, y, z, rhw, u, v;
};

// The finished frame, looked up through the ramp, back into the back buffer.
bool apply_ramp(IDirect3DDevice9* device) {
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) {
        return false;
    }
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);
    if (!ensure_resources(device, desc)) {
        back->Release();
        return false;
    }
    IDirect3DSurface9* copy = nullptr;
    g_frame->GetSurfaceLevel(0, &copy);
    if (!copy || FAILED(device->StretchRect(back, nullptr, copy, nullptr, D3DTEXF_NONE))) {
        release(copy);
        back->Release();
        return false;
    }
    copy->Release();

    IDirect3DSurface9* targets[4] = {};
    for (DWORD i = 0; i < 4; ++i) {
        if (FAILED(device->GetRenderTarget(i, &targets[i]))) {
            targets[i] = nullptr;
        }
    }
    IDirect3DSurface9* depth = nullptr;
    if (FAILED(device->GetDepthStencilSurface(&depth))) {
        depth = nullptr;
    }
    g_state->Capture();

    device->SetRenderTarget(0, back);
    for (DWORD i = 1; i < 4; ++i) {
        if (targets[i]) {
            device->SetRenderTarget(i, nullptr);
        }
    }
    device->SetDepthStencilSurface(nullptr);
    const D3DVIEWPORT9 viewport{0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(g_shader);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetTexture(0, g_frame);
    device->SetTexture(1, g_lut);
    for (DWORD s = 0; s < 2; ++s) {
        const DWORD filter = s == 0 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
        device->SetSamplerState(s, D3DSAMP_MINFILTER, filter);
        device->SetSamplerState(s, D3DSAMP_MAGFILTER, filter);
        device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    const float w = static_cast<float>(desc.Width) - 0.5f;
    const float h = static_cast<float>(desc.Height) - 0.5f;
    const Vertex quad[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {w, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, h, 0.0f, 1.0f, 0.0f, 1.0f},
        {w, h, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    bool ok = SUCCEEDED(device->BeginScene());
    if (ok) {
        ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex)));
        device->EndScene();
    }

    for (DWORD i = 0; i < 4; ++i) {
        if (i == 0 || targets[i]) {
            device->SetRenderTarget(i, targets[i]);
        }
        release(targets[i]);
    }
    device->SetDepthStencilSurface(depth);
    release(depth);
    g_state->Apply();
    back->Release();
    return ok;
}

void WINAPI hook_set_gamma_ramp(IDirect3DDevice9* device, UINT swapchain, DWORD flags, const D3DGAMMARAMP* ramp) {
    if (ramp && swapchain == 0) {
        const bool identity = ramp_is_identity(*ramp);
        if (!g_ramp_set || std::memcmp(&g_ramp, ramp, sizeof(g_ramp)) != 0) {
            if (!g_ramp_set || identity != g_identity) {
                LOG_INFO("Gamma: the game set a%s ramp (level 64 -> %u, 128 -> %u, 192 -> %u of 65535)", identity ? "n identity" : "",
                         ramp->green[64], ramp->green[128], ramp->green[192]);
            }
            g_ramp = *ramp;
            g_lut_dirty = true;
        }
        g_ramp_set = true;
        g_identity = identity;
    }
    g_set_gamma_ramp(device, swapchain, flags, ramp);
}

HRESULT WINAPI hook_end_scene(IDirect3DDevice9* device) {
    if (device == g_device) {
        g_new_content = true;
    }
    return g_end_scene(device);
}

HRESULT WINAPI hook_present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    const bool fresh = g_new_content || !g_end_scene;
    if (device == g_device) {
        g_new_content = false;
    }
    if (g_enabled && fresh && device == g_device && g_ramp_set && !g_identity) {
        if (!g_windowed_known || ++g_frames_since_mode_check >= 120) {
            g_frames_since_mode_check = 0;
            read_display_mode(device);
        }
        if (g_windowed && g_failures < 20) {
            if (apply_ramp(device)) {
                if (!g_logged_active) {
                    g_logged_active = true;
                    LOG_INFO("Gamma: the game's brightness ramp is applied to the windowed picture");
                }
            } else if (++g_failures == 20) {
                LOG_ERROR("Gamma: applying the brightness ramp failed 20 times; giving up for this device");
            }
            g_new_content = false;  // our own draw is not new content
        }
    }
    return g_present(device, src, dst, window, dirty);
}

}  // namespace

void gamma_attach(IDirect3DDevice9* game_device, bool enabled) {
    g_enabled = enabled;
    if (!game_device) {
        return;
    }
    // The device that presents: beneath DSfix's wrapper, the owner of the back buffer.
    IDirect3DDevice9* device = nullptr;
    IDirect3DSurface9* back = nullptr;
    if (SUCCEEDED(game_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        back->GetDevice(&device);
        back->Release();
    }
    if (!device) {
        device = game_device;
        device->AddRef();
    }
    if (g_device && g_device != device) {
        // A new device: the old one's resources are gone with it.
        g_frame = nullptr;
        g_lut = nullptr;
        g_shader = nullptr;
        g_state = nullptr;
        g_failures = 0;
    }
    g_device = device;
    void* present = patch_slot(device, kPresentSlot, reinterpret_cast<void*>(&hook_present));
    if (present && !g_present) {
        g_present = reinterpret_cast<PresentFn>(present);
    }
    void* gamma = patch_slot(device, kSetGammaRampSlot, reinterpret_cast<void*>(&hook_set_gamma_ramp));
    if (gamma && !g_set_gamma_ramp) {
        g_set_gamma_ramp = reinterpret_cast<SetGammaRampFn>(gamma);
    }
    void* end_scene = patch_slot(device, kEndSceneSlot, reinterpret_cast<void*>(&hook_end_scene));
    if (end_scene && !g_end_scene) {
        g_end_scene = reinterpret_cast<EndSceneFn>(end_scene);
    }
    LOG_INFO("Gamma: %s (presenting device %p%s)", enabled ? "brightness ramp in windowed modes on" : "WindowedGamma off", device,
             device == game_device ? "" : ", beneath a wrapper");
    g_windowed_known = false;
    read_display_mode(device);
    device->Release();  // the game owns it; only the pointer is kept
}

void gamma_before_reset() {
    release(g_frame);
    release(g_state);
}

void gamma_after_reset() {
    g_windowed_known = false;
    g_failures = 0;
}
