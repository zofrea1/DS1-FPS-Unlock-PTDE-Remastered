#include "gamma.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "user32.lib")

namespace {

constexpr int kSetGammaSlot = 21;  // IDirect3DDevice9::SetGammaRamp

using SetGammaFn = void(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, const D3DGAMMARAMP*);
SetGammaFn g_set_gamma = nullptr;
int g_mode = kGammaAuto;
bool g_dxvk = false;

D3DGAMMARAMP g_last{};
bool g_have_last = false;
bool g_last_passed = true;
SRWLOCK g_lock = SRWLOCK_INIT;

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

// DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO (Windows 10 1703), declared here so older SDKs build.
struct AdvancedColorInfo {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value;  // bit 0 supported, 1 enabled, 2 wide colour enforced (Auto Color Management), 3 force disabled
    UINT32 encoding;
    UINT32 bits_per_channel;
};
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE kGetAdvancedColorInfo = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(9);

struct DisplayState {
    bool known = false;
    bool advanced = false;  // Windows runs the display in advanced colour: gamma ramps have no effect
    char text[192] = {};
};

// The colour mode of the display showing the device's window.
DisplayState display_state(IDirect3DDevice9* device) {
    DisplayState out;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    HWND window = SUCCEEDED(device->GetCreationParameters(&cp)) ? cp.hFocusWindow : nullptr;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) {
        std::snprintf(out.text, sizeof(out.text), "display unknown");
        return out;
    }
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS ||
        path_count == 0 || path_count > 64 || mode_count > 128) {
        std::snprintf(out.text, sizeof(out.text), "display configuration unavailable");
        return out;
    }
    DISPLAYCONFIG_PATH_INFO paths[64];
    DISPLAYCONFIG_MODE_INFO modes[128];
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, nullptr) != ERROR_SUCCESS) {
        std::snprintf(out.text, sizeof(out.text), "display configuration unavailable");
        return out;
    }
    for (UINT32 i = 0; i < path_count; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            _wcsicmp(source.viewGdiDeviceName, mi.szDevice) != 0) {
            continue;
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        char name[64] = "?";
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0]) {
            WideCharToMultiByte(CP_UTF8, 0, target.monitorFriendlyDeviceName, -1, name, sizeof(name), nullptr, nullptr);
        }
        AdvancedColorInfo color{};
        color.header.type = kGetAdvancedColorInfo;
        color.header.size = sizeof(color);
        color.header.adapterId = paths[i].targetInfo.adapterId;
        color.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS) {
            std::snprintf(out.text, sizeof(out.text), "display %s: colour mode unknown (Windows before 10 1703?)",
                          name);
            return out;
        }
        const bool supported = color.value & 1, enabled = (color.value >> 1) & 1, wide = (color.value >> 2) & 1,
                   disabled = (color.value >> 3) & 1;
        out.known = true;
        out.advanced = (enabled || wide) && !disabled;
        std::snprintf(out.text, sizeof(out.text), "display %s: %s (HDR %s%s%s, %u bits per channel)", name,
                      out.advanced ? "advanced colour on" : "standard colour", supported ? "supported" : "unsupported",
                      enabled ? ", on" : ", off", wide ? ", Auto Color Management" : "", color.bits_per_channel);
        return out;
    }
    std::snprintf(out.text, sizeof(out.text), "display not found in the display configuration");
    return out;
}

// The exponent a ramp channel follows, from its middle entry (ramp = (i / 255) ^ e).
double exponent(const WORD* channel) {
    const double v = channel[128] / 65535.0;
    return v > 0.0 && v < 1.0 ? std::log(v) / std::log(128.0 / 255.0) : 0.0;
}

void describe(const D3DGAMMARAMP& r, char* out, size_t size) {
    static const int kAt[] = {0, 8, 16, 32, 64, 128, 192, 255};
    int n = std::snprintf(out, size, "exponent about R %.2f G %.2f B %.2f; R at", exponent(r.red), exponent(r.green),
                          exponent(r.blue));
    for (int i : kAt) n += std::snprintf(out + n, size - n, " %d:%u", i, r.red[i]);
    const bool grey = std::memcmp(r.red, r.green, sizeof(r.red)) == 0 && std::memcmp(r.red, r.blue, sizeof(r.red)) == 0;
    if (grey) std::snprintf(out + n, size - n, " (same in G and B)");
}

// DXVK's own identity (MapGammaControlPoint), so DXVK sees it as identity and leaves its present shader alone.
void identity(D3DGAMMARAMP* r) {
    for (int i = 0; i < 256; ++i) {
        const float x = static_cast<float>(i) / 255.0f;
        const WORD v = static_cast<WORD>(65535.0f * x);
        r->red[i] = r->green[i] = r->blue[i] = v;
    }
}

void WINAPI hook_set_gamma(IDirect3DDevice9* device, UINT chain, DWORD flags, const D3DGAMMARAMP* ramp) {
    if (!ramp) {
        g_set_gamma(device, chain, flags, ramp);
        return;
    }
    bool pass = true;
    const char* why = "";
    DisplayState state;
    if (g_mode == kGammaOff) {
        pass = false;
        why = " (GammaRamp = false)";
    } else if (g_mode == kGammaAuto && g_dxvk) {
        state = display_state(device);
        if (state.advanced) {
            pass = false;
            why = " (DXVK in advanced colour: the system Direct3D 9 would have no ramp here either)";
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    const bool changed =
        !g_have_last || std::memcmp(&g_last, ramp, sizeof(*ramp)) != 0 || g_last_passed != pass;
    g_last = *ramp;
    g_have_last = true;
    g_last_passed = pass;
    ReleaseSRWLockExclusive(&g_lock);
    if (changed) {
        if (!state.text[0]) state = display_state(device);
        char text[256];
        describe(*ramp, text, sizeof(text));
        LOG_INFO("Gamma ramp from the game (swap chain %u, flags %lu): %s; %s; %s%s", chain, flags, text, state.text,
                 pass ? "passed on" : "left out", why);
    }
    if (pass) {
        g_set_gamma(device, chain, flags, ramp);
    } else {
        D3DGAMMARAMP flat;
        identity(&flat);
        g_set_gamma(device, chain, flags, &flat);
    }
}

}  // namespace

void gamma_hook_device(IDirect3DDevice9* game_device, int mode, bool dxvk) {
    if (!game_device || g_set_gamma) return;
    g_mode = mode;
    g_dxvk = dxvk;
    void* original = patch_slot(game_device, kSetGammaSlot, reinterpret_cast<void*>(&hook_set_gamma));
    if (original) g_set_gamma = reinterpret_cast<SetGammaFn>(original);
    const DisplayState state = display_state(game_device);
    static const char* const kModes[] = {"auto", "true", "false"};
    LOG_INFO("GammaRamp = %s on %s; %s", kModes[mode >= 0 && mode <= 2 ? mode : 0],
             dxvk ? "DXVK" : "the system Direct3D 9", state.text);
}

void gamma_frame() {
    static ULONGLONG next = 0;
    static bool seen = false;
    if (seen) return;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    if (!GetModuleHandleW(L"nvAIHDR.dll")) return;
    seen = true;
    if (g_dxvk) {
        LOG_INFO("NVIDIA RTX HDR (nvAIHDR.dll) is filtering the game's picture: it applies to Dark Souls only when it "
                 "runs on DXVK, so shadows, highlights and colors look different from Direct3D 9. For the same picture "
                 "as Direct3D 9, turn RTX HDR off for this game in the NVIDIA app (Graphics > Dark Souls: Prepare to "
                 "Die Edition > RTX HDR), or tune it there (Middle grey, Contrast, Saturation).");
    } else {
        LOG_INFO("NVIDIA RTX HDR (nvAIHDR.dll) has loaded into the game on the system Direct3D 9.");
    }
}

void gamma_after_reset(IDirect3DDevice9* game_device) {
    if (!game_device) return;
    D3DPRESENT_PARAMETERS pp{};
    IDirect3DSwapChain9* chain = nullptr;
    bool windowed = true;
    if (SUCCEEDED(game_device->GetSwapChain(0, &chain)) && chain) {
        if (SUCCEEDED(chain->GetPresentParameters(&pp))) windowed = pp.Windowed != FALSE;
        chain->Release();
    }
    const DisplayState state = display_state(game_device);
    LOG_INFO("Reset to %s; %s", windowed ? "windowed" : "fullscreen", state.text);
}
