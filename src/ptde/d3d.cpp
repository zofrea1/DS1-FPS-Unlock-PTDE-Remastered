#include "depthrev.h"
#include "d3d.h"

#include "aspect.h"
#include "atoc.h"
#include "layout.h"
#include "menuscale.h"
#include "passcull.h"
#include "d3dperf.h"
#include "surfpool.h"
#include "dxgipresent.h"
#include "dxvk.h"
#include "frame.h"
#include "fpsu_buildcfg.h"
#include "scanwatch.h"
#include "limiter.h"
#include "gamma.h"
#include "extension.h"
#include "log.h"
#include "profile.h"
#include "remote.h"
#include "remixdisplay.h"
#include "dsfix.h"
#include "dofwatch.h"
#include "occl.h"
#include "foliage.h"
#include "resolution.h"
#include "vkprobe.h"
#include "shadowmap.h"
#include "settingsmenu.h"
#include "settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

HMODULE g_self = nullptr;
Settings g_settings;
volatile LONG g_settings_state = 0;  // 0 = not loaded, 1 = loading, 2 = loaded

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                        IDirect3DDevice9**);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

CreateFn g_create = nullptr;
bool g_dxvk_active = false;  // Direct3DCreate9 leads to DXVK
volatile bool g_d3d_created = false;  // the game's first Direct3DCreate9 has run (g_dxvk_active is decided)
CreateDeviceFn g_create_device = nullptr;
ResetFn g_reset = nullptr;
using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PresentFn g_present = nullptr;

// DXVK through DXGI: each picture the game presents, with the frame boundary its flip is timed from.
bool g_present_dxgi = false;
bool g_present_watch = false;  // the system Direct3D 9: ScanWatch is told when the game presents

HRESULT WINAPI hook_present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window,
                            const RGNDATA* dirty) {
    atoc_aa_tick();
    settingsmenu_present(device);
    if (g_present_dxgi) dxgipresent_frame_presented(frame_boundary_qpc());
    const HRESULT hr = g_present(device, src, dst, window, dirty);
#if FPSU_DIAGNOSTICS
    if (g_present_watch) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        scanwatch_note_flip(t.QuadPart);
    }
#endif
    return hr;
}

constexpr int kCreateDeviceSlot = 16;  // IDirect3D9::CreateDevice
constexpr int kResetSlot = 16;         // IDirect3DDevice9::Reset
constexpr int kPresentSlot = 17;       // IDirect3DDevice9::Present
constexpr int kSetVsConstSlot = 94;    // IDirect3DDevice9::SetVertexShaderConstantF

using SetVsConstFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
SetVsConstFn g_set_vs_const = nullptr;
bool g_probe = false;
unsigned long long g_frame_hash = 1469598103934665603ull;
unsigned long long g_small_hash = 1469598103934665603ull;  // uploads of up to 8 registers (camera, world matrices)
unsigned long long g_big_hash = 1469598103934665603ull;    // larger uploads (skinning palettes)

HRESULT WINAPI hook_set_vs_const(IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    float corrected[24 * 4];
    const float* use = data;
    if (data && aspect_correct_constants(data, count, corrected)) {
        use = corrected;
    }
    if (g_probe && data && count >= 3 && count < 512) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        unsigned long long h = g_frame_hash ^ start;
        h *= 1099511628211ull;
        for (UINT i = 0; i < count * 16; ++i) {
            h = (h ^ bytes[i]) * 1099511628211ull;
        }
        g_frame_hash = h;
        unsigned long long& part = count <= 8 ? g_small_hash : g_big_hash;
        unsigned long long p = part ^ start;
        p *= 1099511628211ull;
        for (UINT i = 0; i < count * 16; ++i) {
            p = (p ^ bytes[i]) * 1099511628211ull;
        }
        part = p;
    }
    return g_set_vs_const(device, start, use, count);
}

const Settings& settings() {
    if (g_settings_state != 2) {
        if (InterlockedCompareExchange(&g_settings_state, 1, 0) == 0) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(g_self, path, MAX_PATH);
            g_settings = settings_load(path);
            g_settings_state = 2;
        } else {
            while (g_settings_state != 2) {
                Sleep(1);
            }
        }
    }
    return g_settings;
}

// FixAspect, unless DSfix decides the picture (YieldToDSFix): then the game keeps its 16:9 picture on any screen and
// draws its HUD and menus where it does without this mod, which is where DSfix 3.0's HUD mod and weaponWearBars look
// for them (FixAspectUI's 16:9 layout in the middle, or the layout stretched without it, would move them).
bool aspect_wanted() {
    return settings().fps_unlock && settings().fix_aspect && !dsfix_yields();
}

// The game's own settings: [[0x1378700]+0x34], the refresh rate (setting 9, DarkSouls.ini [DisplaySettingFullScreen]
// RefreshRate) at +0xE0 (the PC settings screen's apply writes it through 0xBFE010, its getter reads +0xE0).
constexpr uintptr_t kGameSettingsHolder = 0x01378700;

void set_game_refresh_rate(UINT rate) {
    __try {
        const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(kGameSettingsHolder);
        const uintptr_t obj = holder ? *reinterpret_cast<const uintptr_t*>(holder + 0x34) : 0;
        if (obj) *reinterpret_cast<UINT*>(obj + 0xE0) = rate;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Fullscreen modes the display offers at the requested size, and the one to use.
void adjust_present_parameters(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp, const char* where) {
    const Settings& cfg = settings();
    if (!pp || pp->Windowed || cfg.fullscreen_refresh < 0) {
        return;
    }
    // YieldToDSFix: the rate is DSfix's (fullscreenHz, in 2.x and 3.0). This hook sits beneath DSfix's, so the rate DSfix
    // chose would be replaced (FullscreenRefreshRate 0: the display's highest).
    if (dsfix_yields()) {
        LOG_INFO("%s: fullscreen %ux%u at %u Hz as DSfix passes it on (YieldToDSFix: DSfix's fullscreenHz decides, "
                 "FullscreenRefreshRate is off)", where, pp->BackBufferWidth, pp->BackBufferHeight,
                 pp->FullScreen_RefreshRateInHz);
        return;
    }
    // A 10-bit back buffer (HighPrecisionOutput) is shown in the same display modes as an 8-bit one.
    const D3DFORMAT format = pp->BackBufferFormat == D3DFMT_UNKNOWN || pp->BackBufferFormat == D3DFMT_A2R10G10B10
                                 ? D3DFMT_X8R8G8B8
                                 : pp->BackBufferFormat;
    const UINT count = d3d->GetAdapterModeCount(adapter, format);
    UINT rates[64] = {};
    int rate_count = 0;
    for (UINT i = 0; i < count; ++i) {
        D3DDISPLAYMODE mode{};
        if (FAILED(d3d->EnumAdapterModes(adapter, format, i, &mode))) {
            continue;
        }
        if (mode.Width != pp->BackBufferWidth || mode.Height != pp->BackBufferHeight) {
            continue;
        }
        bool seen = false;
        for (int r = 0; r < rate_count; ++r) {
            seen |= rates[r] == mode.RefreshRate;
        }
        if (!seen && rate_count < 64) {
            rates[rate_count++] = mode.RefreshRate;
        }
    }
    char list[512] = {};
    int n = 0;
    for (int r = 0; r < rate_count; ++r) {
        n += std::snprintf(list + n, sizeof(list) - n, "%s%u", r ? ", " : "", rates[r]);
    }
    const UINT requested = pp->FullScreen_RefreshRateInHz;
    if (rate_count == 0) {
        LOG_INFO("%s: fullscreen %ux%u, no display modes listed at that size; leaving the refresh rate at %u Hz",
                 where, pp->BackBufferWidth, pp->BackBufferHeight, requested);
        return;
    }
    UINT chosen = 0;
    // 0 (auto): the rate chosen in the game's PC settings (Frequency, which lists every rate the display offers with
    // refresh_list_patch), or the highest while that is still the game's own 59/60 Hz; the game's setting then says
    // the rate in use, so its screen shows it (and the game keeps it in DarkSouls.ini).
    const bool game_default = requested == 0 || requested == 59 || requested == 60;
    if (cfg.fullscreen_refresh == 0 && !game_default) {
        for (int r = 0; r < rate_count; ++r) {
            if (rates[r] == requested) chosen = requested;
        }
    }
    if (cfg.fullscreen_refresh > 0) {
        // Exact match if offered, otherwise the highest rate not above the request.
        for (int r = 0; r < rate_count; ++r) {
            if (rates[r] == static_cast<UINT>(cfg.fullscreen_refresh)) {
                chosen = rates[r];
            }
        }
        if (chosen == 0) {
            for (int r = 0; r < rate_count; ++r) {
                if (rates[r] <= static_cast<UINT>(cfg.fullscreen_refresh) && rates[r] > chosen) {
                    chosen = rates[r];
                }
            }
        }
    }
    if (chosen == 0) {
        for (int r = 0; r < rate_count; ++r) {
            if (rates[r] > chosen) {
                chosen = rates[r];
            }
        }
    }
    LOG_INFO("%s: fullscreen %ux%u offers [%s] Hz; game asked for %u Hz, using %u Hz", where, pp->BackBufferWidth,
             pp->BackBufferHeight, list, requested, chosen);
    if (cfg.fullscreen_refresh == 0 && game_default && chosen != requested) set_game_refresh_rate(chosen);
    pp->FullScreen_RefreshRateInHz = chosen;
}

// HighPrecisionOutput: the back buffer A2R10G10B10 while the frame buffer is 10-bit and the device is DXVK's (the game
// asks for X8R8G8B8), else the game's own format (an 8-bit one if the game's descriptor gave it the frame's 10-bit
// format). Returns the format the game asked for.
// Under Proton: the game window's Vulkan surface offers a 10-bit format in the SDR colour space (vkprobe.h), so
// Proton's DXVK presents the 10-bit back buffer as it is (gamescope with HDR on, the Steam Deck OLED); -1 not probed.
int g_proton_10bit = -1;

D3DFORMAT fit_back_buffer_format(D3DPRESENT_PARAMETERS* pp, const char* where) {
    if (!pp) return D3DFMT_UNKNOWN;
    const D3DFORMAT asked = pp->BackBufferFormat;
    const bool ten = resolution_output_10bit() && (g_dxvk_active || g_proton_10bit == 1);
    if (ten) {
        pp->BackBufferFormat = D3DFMT_A2R10G10B10;
    } else if (asked == D3DFMT_A2R10G10B10) {
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    }
    static int logged = 0;
    if (logged < 4 && (resolution_output_10bit() || asked != pp->BackBufferFormat)) {
        ++logged;
        LOG_INFO("HighPrecisionOutput: %s with a %s back buffer (the game asked for format %d)", where,
                 pp->BackBufferFormat == D3DFMT_A2R10G10B10 ? "10-bit" : "8-bit", static_cast<int>(asked));
    }
    if (resolution_output_10bit() && !ten) resolution_output_8bit_back();
    return asked;
}

HRESULT WINAPI hook_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    IDirect3D9* d3d = nullptr;
    if (pp && SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
        aspect_set_enabled(aspect_wanted());
        if (aspect_is_enabled()) {
            aspect_adjust_present(d3d, D3DADAPTER_DEFAULT, pp, "Reset");
        }
        if (!pp->Windowed) {
            adjust_present_parameters(d3d, D3DADAPTER_DEFAULT, pp, "Reset");
        }
        d3d->Release();
    }
    depthrev_adjust_present(pp, settings().reversed_depth);
    const D3DFORMAT asked = fit_back_buffer_format(pp, "Reset");
    d3dperf_reset();
    surfpool_reset();
    resolution_reset();
    occl_reset();
    passcull_reset();
    atoc_reset();
    settingsmenu_reset();
    HRESULT hr = g_reset(device, pp);
    if (FAILED(hr) && pp && pp->BackBufferFormat == D3DFMT_A2R10G10B10) {
        pp->BackBufferFormat = asked == D3DFMT_A2R10G10B10 || asked == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : asked;
        resolution_output_8bit_back();
        hr = g_reset(device, pp);
    }
    if (SUCCEEDED(hr) && aspect_is_active()) {
        aspect_hook_device(device);
    }
    if (SUCCEEDED(hr)) {
        gamma_after_reset(device);
    }
    return hr;
}

// Patch one slot of a COM object's vtable, returning the original pointer.
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

HRESULT WINAPI hook_create_device(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                  D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    // RTX Remix's bridge sets the fullscreen display mode itself (at 60 Hz): its call takes FullscreenRefreshRate.
    if (settings().rtx_remix_applied) remix_display_install(settings().fps_unlock ? settings().fullscreen_refresh : -1);
    aspect_set_enabled(aspect_wanted());
    if (aspect_is_enabled()) {
        aspect_adjust_present(d3d, adapter, pp, "CreateDevice");
    }
    if (settings().fps_unlock) adjust_present_parameters(d3d, adapter, pp, "CreateDevice");
    depthrev_adjust_present(pp, settings().reversed_depth);
    if (resolution_output_10bit() && !g_dxvk_active && g_proton_10bit < 0 && d3d_under_proton()) {
        char text[512];
        const HWND w = pp && pp->hDeviceWindow ? pp->hDeviceWindow : window;
        g_proton_10bit = vkprobe_10bit_surface(w, text, sizeof(text)) == 1 ? 1 : 0;
        LOG_INFO("HighPrecisionOutput: under Proton the game window's Vulkan surface offers %s: %s", text,
                 g_proton_10bit ? "a 10-bit format, so Proton's DXVK presents 10 bits a channel"
                                : "no 10-bit format in the SDR colour space, so the picture stays 8-bit (dithered)");
    }
    const D3DFORMAT asked = fit_back_buffer_format(pp, "CreateDevice");
    HRESULT hr = g_create_device(d3d, adapter, type, window, flags, pp, out);
    if (FAILED(hr) && pp && pp->BackBufferFormat == D3DFMT_A2R10G10B10) {
        pp->BackBufferFormat = asked == D3DFMT_A2R10G10B10 || asked == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : asked;
        resolution_output_8bit_back();
        hr = g_create_device(d3d, adapter, type, window, flags, pp, out);
    }
    LOG_INFO("CreateDevice: adapter %u, type %d, behavior flags 0x%08lX%s%s%s%s%s%s, hr 0x%08lX", adapter,
             static_cast<int>(type), flags, (flags & D3DCREATE_HARDWARE_VERTEXPROCESSING) ? " hardware-vp" : "",
             (flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? " SOFTWARE-vp" : "",
             (flags & D3DCREATE_MIXED_VERTEXPROCESSING) ? " MIXED-vp" : "",
             (flags & D3DCREATE_MULTITHREADED) ? " multithreaded" : "",
             (flags & D3DCREATE_FPU_PRESERVE) ? " fpu-preserve" : "", (flags & D3DCREATE_PUREDEVICE) ? " pure" : "",
             static_cast<unsigned long>(hr));
    if (FAILED(hr) && pp) {
        RECT client{};
        GetClientRect(pp->hDeviceWindow ? pp->hDeviceWindow : window, &client);
        LOG_ERROR("CreateDevice failed with %ux%u format %d, %s, %u back buffers, swap effect %d, window client %ldx%ld",
                  pp->BackBufferWidth, pp->BackBufferHeight, static_cast<int>(pp->BackBufferFormat),
                  pp->Windowed ? "windowed" : "fullscreen", pp->BackBufferCount, static_cast<int>(pp->SwapEffect),
                  client.right - client.left, client.bottom - client.top);
    }
    if (SUCCEEDED(hr) && out && *out) {
        depthrev_hook_device(*out, settings().reversed_depth && g_dxvk_active);  // beneath every other hook (ReversedDepth, experimental)
        occl_capture_device(*out);  // before any hook
        const PtdeExtension* ext = ptde_extension();
        if (ext && ext->device_created) ext->device_created(*out);
    }
    if (SUCCEEDED(hr) && out && *out && settings().fps_unlock) {
        void* original = patch_slot(*out, kResetSlot, reinterpret_cast<void*>(&hook_reset));
        if (original && !g_reset) {
            g_reset = reinterpret_cast<ResetFn>(original);
            LOG_INFO("Device Reset hooked");
        }
        g_present_dxgi = g_dxvk_active && dxgipresent_in_use();
#if FPSU_DIAGNOSTICS
        if (settings().scan_watch) {
            scanwatch_start(true);
            g_present_watch = !g_present_dxgi;
        }
#endif
        // Present is hooked in every mode: the FPS Unlock Settings page (its hint, its controls and its drawing) and the
        // anti-aliasing that follows the game's own option (atoc_aa_tick) run there each frame. Until 3.0.1 it was
        // hooked only for DXVK through DXGI or ScanWatch, so with DXVK = false (the Steam Deck under Proton, whose
        // own DXVK draws) the page never showed and its hint never came (2026-11-25 Deck log: the PC settings screen
        // seen, no controller buttons ever read by the page).
        {
            void* present = patch_slot(*out, kPresentSlot, reinterpret_cast<void*>(&hook_present));
            if (present && !g_present) g_present = reinterpret_cast<PresentFn>(present);
        }
        // Aspect rewrites the projection on the way through this hook. The content probe only hashes.
        g_probe = settings().content_probe;
        void* vs = (g_probe || aspect_is_active())
                       ? patch_slot(*out, kSetVsConstSlot, reinterpret_cast<void*>(&hook_set_vs_const))
                       : nullptr;
        if (vs && !g_set_vs_const) {
            g_set_vs_const = reinterpret_cast<SetVsConstFn>(vs);
            LOG_INFO("Device SetVertexShaderConstantF hooked%s", g_probe ? " (content-cadence probe)" : "");
        }
        // First of the scissor hooks, so it sees the rectangle the others pass on (beside DSfix 3.0 with a larger map).
        shadowmap_hook_device(*out);
        if (aspect_is_active()) {
            aspect_hook_device(*out);
        }
        // Ours takes precedence over DSfix's filteringOverride: the hook sits under DSfix's device wrapper.
        // YieldToDSFix: DSfix's filteringOverride decides (0 here = the game's own filtering reaches it).
        d3dperf_set_anisotropy(dsfix_yields() ? 0 : settings().anisotropic_filtering);
        d3dperf_hook_device(*out, settings().async_readback, settings().filter_sampler_states,
                            settings().filter_shader_constants);
        if (settings().reuse_surfaces) {
            surfpool_hook_device(*out);
        }
        gamma_hook_device(*out, settings().gamma_ramp, g_dxvk_active);
        resolution_hook_device(*out);
        resolution_check_scene_format(*out, settings().antialiasing_samples);
        resolution_sun_hook_device(*out);
        // FixWater: DSfix 2.4 as released leaves the water's ripple step at 512x360. DSfix 3.0 sets it itself, and the
        // hook would only look at every pixel shader constant upload for nothing.
        if (settings().fix_water_with_dsfix && dsfix_dof_override() > 0 && !resolution_is_active()) {
            if (dsfix_is_v3()) {
                static bool told = false;
                if (!told) {
                    told = true;
                    LOG_INFO("FixWater: off beside DSfix 3.0, which corrects the water itself");
                }
            } else {
                resolution_water_hook_device(*out, dsfix_dof_override());
            }
        }
        atoc_fix_depth_check(*out);
        if (settings().alpha_to_coverage) {
            atoc_hook_device(*out);
        }
        // Last, so the draws it leaves out reach no other hook.
        // Not beside DSfix: it puts textures back through state blocks, which the watch for reads does not see.
        if (dsfix_loaded()) {
            passcull_set_mode(0);
            if (settings().skip_unused_reflection) {
                LOG_INFO("SkipUnusedReflection: off while DSfix is loaded");
            }
        } else {
            passcull_set_mode(settings().skip_unused_reflection ? 1 : 0);
            if (settings().skip_unused_reflection || settings().remote_control) {
                passcull_hook_device(*out);
            }
        }
        // YieldToDSFix: the HUD and the menus are DSfix's (its HUD mod: hudScale*, menuScale, listWindowScale,
        // promptScale, subtitleScale). None of ours is placed, so the game draws them as it does without this mod:
        // DSfix 3.0 finds the HUD's parts, windows, prompts and weapon slots by where and how large the game draws them
        // (its HUD mod, weaponWearBars), which UIScale, UIScaleWorld, UIScaleMenus and FixBossBarOverlap would move.
        const bool yield_hud = dsfix_yields();
        const int ui_scale = yield_hud ? 100 : settings().ui_scale;
        const int world_scale = yield_hud ? 100 : settings().ui_scale_world;
        // UIScale and UIScaleWorld at the menus' source (menuscale.cpp: each dialog of the HUD scaled by the game's
        // own transform); the layout's draws are then refit for FixAspectUI only. Its position rules are the fallback
        // where the menus' code is not as this build's.
        // UIScaleText: the letters of the HUD's texts with their part (auto, 0) or at a size of their own.
        const int text_scale = yield_hud ? 0 : settings().ui_scale_text;
        const bool text_own = text_scale >= 25 && text_scale <= 200 && text_scale != 100;
        // UIScaleMenus: the in-game menu's screens about the middle of the screen (auto: as UIScale). FixBossBarOverlap:
        // the prompts moved up over a boss's health bar.
        const int menus_scale = yield_hud ? 100 : settings().ui_scale_menus;
        const int menus_eff = menus_scale == 0 ? ui_scale : menus_scale;
        const bool menus_own = menus_eff >= 25 && menus_eff <= 200 && menus_eff != 100;
        const bool fix_boss = !yield_hud && settings().fix_boss_bar_overlap;
        const bool hooks = ui_scale != 100 || world_scale != 100 || text_own || menus_own || fix_boss;
        const bool at_source = hooks && menuscale_install();
        if (at_source) {
            menuscale_set(ui_scale, world_scale, text_scale, menus_scale);
            menuscale_fix_boss_bar(fix_boss);
        }
        layout_configure(settings().fix_aspect_ui, at_source ? 100 : ui_scale);
        layout_set_world_scale(at_source ? 100 : world_scale);
        if ((settings().fix_aspect_ui && aspect_is_active()) || (!at_source && (ui_scale < 100 || world_scale < 100))) {
            layout_hook_device(*out, !resolution_is_active());
        }
        if (resolution_is_active() || (settings().fix_aspect_ui && aspect_is_active())) layout_patch_scissor_sites();
        // FoliageSway: the static models' vertex shaders get their swaying copies as the game makes them.
        foliage_hook_device(*out);
        if (settings().shadow_bias_listed) {
            static bool told = false;
            if (!told) {
                told = true;
                LOG_INFO("ShadowBias is no longer a setting (2.6.0): it erased real shadows on characters and objects "
                         "close to the camera, which lit up as they crossed the shadow map's slices. The game's own "
                         "shadow comparison is used; ShadowMapResolution keeps the texel weave fine");
            }
        }
        dofwatch_hook_device(*out);
        occl_set_device(*out);
    }
    if (SUCCEEDED(hr) && out && *out && remote_enabled()) {
        remote_hook_device(*out);
    }
    // Last, so the census counts what the game calls and chains into every hook above.
    if (SUCCEEDED(hr) && out && *out && profile_enabled() && settings().profile_d3d) {
        profile_hook_device(*out);
    }
    return hr;
}

bool dxvk_expected() {
    if (!settings().fps_unlock || !settings().dxvk) return false;
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return false;
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) {
        slash[1] = 0;
    } else {
        dir[0] = 0;
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s%s", dir, kDxvkFileName);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// DXVK goes in before the game's first call reaches the system Direct3DCreate9, so whatever
// wraps that function (DSfix) wraps DXVK.
void route_to_dxvk() {
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) {
        slash[1] = 0;
    } else {
        dir[0] = 0;
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s%s", dir, kDxvkFileName);
    dxgipresent_set_pacing(settings().dxvk_paced_present ? 1 : 0, limiter_requested_fps);
    dxgipresent_set_hdr_chain(settings().dxvk_hdr_swap_chain);
    // ReversedDepth: DXVK keeps the game's 24-bit depth buffers in 32-bit float ones (as it always does on GPUs
    // without 24-bit depth, AMD's among them), with stencil and multisampling as before.
    if (settings().reversed_depth) dxvk_add_option("d3d9.useD32forD24", "True");  // only read when DXVK loads
    g_dxvk_active = dxvk_route(path, settings().dxvk_exclusive_fullscreen, settings().dxvk_dxgi_present);
}

IDirect3D9* WINAPI hook_direct3d_create9(UINT sdk_version) {
    // The import slot is patched from DllMain, before the INI can be read; with FPSUnlock false
    // Direct3D and the device are left alone.
    if (settings().fps_unlock && settings().dxvk) {
        route_to_dxvk();
    }
    IDirect3D9* d3d = g_create(sdk_version);
    g_d3d_created = true;
    const PtdeExtension* ext = ptde_extension();
    if (d3d && (settings().fps_unlock || (ext && ext->wants_device && ext->wants_device()))) {
        void* original = patch_slot(d3d, kCreateDeviceSlot, reinterpret_cast<void*>(&hook_create_device));
        if (original && !g_create_device) {
            g_create_device = reinterpret_cast<CreateDeviceFn>(original);
            LOG_INFO("IDirect3D9::CreateDevice hooked");
        }
    }
    return d3d;
}

// Finds the game's import-table slot for d3d9.dll!Direct3DCreate9.
void** find_create_slot() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return nullptr;
    }
    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), "d3d9.dll") != 0) {
            continue;
        }
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* slots = reinterpret_cast<void**>(base + desc->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) {
                continue;
            }
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "Direct3DCreate9") == 0) {
                return &slots[i];
            }
        }
    }
    return nullptr;
}

}  // namespace

bool d3d_dxvk_expected() { return dxvk_expected(); }

// The PC settings screen's Frequency: the game lists a fullscreen resolution's modes only at 59 and 60 Hz (its list
// builder 0xC154D0 keeps a mode whose rate is in {59, 60}: `je 0xC15964` at 0xC15862 skips the others). With the
// jump gone every rate the display offers at that resolution is listed (the format check stays).
bool d3d_refresh_list_patch() {
    static const uint8_t kExpect[6] = {0x0F, 0x84, 0xFC, 0x00, 0x00, 0x00};
    auto* site = reinterpret_cast<uint8_t*>(0x00C15862);
    if (std::memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        LOG_ERROR("FullscreenRefreshRate: the PC settings screen's rate filter does not match this build; it keeps "
                  "59 and 60 Hz");
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(kExpect), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memset(site, 0x90, sizeof(kExpect));
    VirtualProtect(site, sizeof(kExpect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kExpect));
    LOG_INFO("FullscreenRefreshRate: the PC settings screen's Frequency lists every rate the display offers");
    return true;
}
bool d3d_created() { return g_d3d_created; }
bool d3d_dxvk_active() { return g_dxvk_active; }
bool d3d_under_proton() {
    return settings().fps_unlock && GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}

void d3d_install_early(HMODULE self) {
    g_self = self;
    void** slot = find_create_slot();
    if (!slot) {
        return;
    }
    g_create = reinterpret_cast<CreateFn>(*slot);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return;
    }
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hook_direct3d_create9));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
}

unsigned long long d3d_take_frame_hash(unsigned long long* small_hash, unsigned long long* big_hash) {
    const unsigned long long h = g_frame_hash;
    *small_hash = g_small_hash;
    *big_hash = g_big_hash;
    g_frame_hash = g_small_hash = g_big_hash = 1469598103934665603ull;
    return h;
}
