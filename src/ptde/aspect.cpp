#include "aspect.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "user32.lib")

namespace {

// The Steam build stores these in the exe (image base 0x400000, not relocated).
// Every camera copies them by value: +0x50 is the vertical field of view in radians,
// +0x54 is the aspect. 43 degrees and 16/9. Writing them here is Vert+: the horizontal
// view stays the same, and a shorter aspect (a taller screen) gets a wider vertical view.
constexpr unsigned kAspectRva = 0xDE8484;  // 1.777...
constexpr unsigned kFovYRva = 0xDE4134;    // 0.75049 rad, about 43 degrees
constexpr float kVanillaAspect = 16.f / 9.f;
constexpr float kVanillaFovY = 0.75049156f;
constexpr float kPi = 3.14159265f;

// Below this, the display is taller than 16:9 (16:10 is 1.6, 16:9 is 1.778).
constexpr float kWiderThan = 1.70f;

bool g_enabled = false;
bool g_active = false;
bool g_cover = false;
float g_aspect = kVanillaAspect;
HWND g_window = nullptr;

bool near_f(float a, float b, float tol) {
    const float d = a - b;
    return d < tol && d > -tol;
}

bool steam_deck() {
    char buf[16] = {};
    const DWORD n = GetEnvironmentVariableA("SteamDeck", buf, sizeof(buf));
    return n > 0 && buf[0] != '0';
}

struct Size {
    UINT w;
    UINT h;
};

Size display_size(IDirect3D9* d3d, UINT adapter, HWND window) {
    Size s{0, 0};
    if (window) {
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            s.w = static_cast<UINT>(info.rcMonitor.right - info.rcMonitor.left);
            s.h = static_cast<UINT>(info.rcMonitor.bottom - info.rcMonitor.top);
        }
    }
    if (s.w < 640 || s.h < 480) {
        D3DDISPLAYMODE mode{};
        if (d3d && SUCCEEDED(d3d->GetAdapterDisplayMode(adapter, &mode))) {
            s.w = mode.Width;
            s.h = mode.Height;
        }
    }
    return s;
}

// A same-width mode taller than the current one, in the 16:10 band. The game's own
// list drops these; the adapter sometimes still has them.
UINT taller_same_width(IDirect3D9* d3d, UINT adapter, UINT width, UINT height, D3DFORMAT format) {
    UINT best = height;
    if (!d3d || width == 0) {
        return best;
    }
    const UINT count = d3d->GetAdapterModeCount(adapter, format);
    for (UINT i = 0; i < count; ++i) {
        D3DDISPLAYMODE mode{};
        if (FAILED(d3d->EnumAdapterModes(adapter, format, i, &mode))) {
            continue;
        }
        if (mode.Width != width || mode.Height <= best) {
            continue;
        }
        const float aspect = static_cast<float>(mode.Width) / static_cast<float>(mode.Height);
        if (aspect < kWiderThan && aspect > 1.45f) {
            best = mode.Height;
        }
    }
    return best;
}

bool mode_listed(IDirect3D9* d3d, UINT adapter, UINT width, UINT height, D3DFORMAT format) {
    if (!d3d) {
        return false;
    }
    const UINT count = d3d->GetAdapterModeCount(adapter, format);
    for (UINT i = 0; i < count; ++i) {
        D3DDISPLAYMODE mode{};
        if (FAILED(d3d->EnumAdapterModes(adapter, format, i, &mode))) {
            continue;
        }
        if (mode.Width == width && mode.Height == height) {
            return true;
        }
    }
    return false;
}

float vert_plus_fovy(float aspect) {
    // tan(newFov/2) = tan(oldFov/2) * (16/9) / aspect. xScale stays, yScale shrinks.
    const float t = std::tan(kVanillaFovY * 0.5f) * kVanillaAspect / aspect;
    return 2.f * std::atan(t);
}

bool write_float(void* slot, float value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(float), PAGE_READWRITE, &old)) {
        return false;
    }
    *static_cast<float*>(slot) = value;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(float), old, &ignored);
    return true;
}

void restore_camera_constants() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) {
        return;
    }
    float* aspect_slot = reinterpret_cast<float*>(base + kAspectRva);
    float* fov_slot = reinterpret_cast<float*>(base + kFovYRva);
    const float cur_a = *aspect_slot;
    const float cur_f = *fov_slot;
    if (near_f(cur_a, kVanillaAspect, 0.002f) && near_f(cur_f, kVanillaFovY, 0.002f)) {
        return;
    }
    // Only undo a value this mod wrote. A different number is left for whoever set it.
    if (cur_a < 1.2f || cur_a > kWiderThan || cur_f < 0.4f || cur_f > 1.4f) {
        return;
    }
    if (!write_float(aspect_slot, kVanillaAspect) || !write_float(fov_slot, kVanillaFovY)) {
        return;
    }
    LOG_INFO("Aspect: restored the 16:9 camera (%.1f deg)", kVanillaFovY * (180.f / kPi));
}

void write_camera_constants(float aspect) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) {
        return;
    }
    float* aspect_slot = reinterpret_cast<float*>(base + kAspectRva);
    float* fov_slot = reinterpret_cast<float*>(base + kFovYRva);
    const float cur_a = *aspect_slot;
    const float cur_f = *fov_slot;
    if (cur_a < 0.5f || cur_a > 4.f || cur_f < 0.2f || cur_f > 2.f) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO("Aspect: camera constants look unexpected (%.4f, %.4f); leaving them, correcting the draw instead",
                     cur_a, cur_f);
        }
        return;
    }
    const float fov = vert_plus_fovy(aspect);
    if (near_f(cur_a, aspect, 0.002f) && near_f(cur_f, fov, 0.002f)) {
        return;
    }
    if (!write_float(aspect_slot, aspect) || !write_float(fov_slot, fov)) {
        LOG_INFO("Aspect: could not write the camera constants");
        return;
    }
    LOG_INFO("Aspect: horizontal view unchanged, vertical FOV %.1f -> %.1f deg (aspect %.4f -> %.4f)",
             kVanillaFovY * (180.f / kPi), fov * (180.f / kPi), cur_a, aspect);
}

void cover_window(HWND window) {
    if (!window || !g_cover) {
        return;
    }
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }
    RECT rect{};
    if (!GetWindowRect(window, &rect)) {
        return;
    }
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const bool framed = (style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME)) != 0;
    const bool covers = rect.left == info.rcMonitor.left && rect.top == info.rcMonitor.top &&
                        rect.right == info.rcMonitor.right && rect.bottom == info.rcMonitor.bottom;
    if (!framed && covers) {
        return;
    }
    const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const LONG_PTR frame = WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
                           WS_MAXIMIZEBOX;
    const LONG_PTR ex_frame = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
    SetWindowLongPtrW(window, GWL_STYLE, (style & ~frame) | WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style & ~ex_frame);
    SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

DWORD WINAPI cover_watcher(void*) {
    for (;;) {
        Sleep(500);
        if (g_cover && g_window && !IsIconic(g_window)) {
            cover_window(g_window);
        }
    }
}

void start_cover_thread() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const HANDLE thread = CreateThread(nullptr, 0, cover_watcher, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
}

// A 16:9 rectangle centered in a taller surface: the game's letterbox.
bool is_letterbox(LONG x, LONG y, LONG w, LONG h, LONG bw, LONG bh) {
    if (bw < 16 || bh < 16 || w < 16 || h < 16) {
        return false;
    }
    if (w * 10 < bw * 9 || x > bw / 25) {
        return false;
    }
    if (h + 8 >= bh) {
        return false;
    }
    const float vp = static_cast<float>(w) / static_cast<float>(h);
    if (!near_f(vp, kVanillaAspect, 0.04f)) {
        return false;
    }
    const float bb = static_cast<float>(bw) / static_cast<float>(bh);
    if (!(bb < kWiderThan)) {
        return false;
    }
    const LONG bars = bh - h;
    const LONG centered = bars / 2;
    const LONG dy = y - centered;
    return dy < 8 && dy > -8;
}

bool target_size(IDirect3DSurface9* surface, UINT* w, UINT* h) {
    if (!surface) {
        return false;
    }
    D3DSURFACE_DESC desc{};
    if (FAILED(surface->GetDesc(&desc))) {
        return false;
    }
    *w = desc.Width;
    *h = desc.Height;
    return true;
}

using SetViewportFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetScissorFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*);
using StretchFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                   D3DTEXTUREFILTERTYPE);
using SetTransformFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

SetViewportFn g_set_viewport = nullptr;
SetScissorFn g_set_scissor = nullptr;
StretchFn g_stretch = nullptr;
SetTransformFn g_set_transform = nullptr;
PresentFn g_present = nullptr;

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

void note_once(bool* flag, const char* message) {
    if (*flag) {
        return;
    }
    *flag = true;
    LOG_INFO("%s", message);
}

HRESULT WINAPI hook_set_viewport(IDirect3DDevice9* device, const D3DVIEWPORT9* vp) {
    D3DVIEWPORT9 expanded{};
    const D3DVIEWPORT9* use = vp;
    if (g_active && vp) {
        IDirect3DSurface9* target = nullptr;
        UINT bw = 0;
        UINT bh = 0;
        if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target_size(target, &bw, &bh) &&
            is_letterbox(static_cast<LONG>(vp->X), static_cast<LONG>(vp->Y), static_cast<LONG>(vp->Width),
                         static_cast<LONG>(vp->Height), static_cast<LONG>(bw), static_cast<LONG>(bh))) {
            expanded = *vp;
            expanded.X = 0;
            expanded.Y = 0;
            expanded.Width = bw;
            expanded.Height = bh;
            use = &expanded;
            static bool logged = false;
            note_once(&logged, "Aspect: a 16:9 viewport was opened to the full target");
        }
        if (target) {
            target->Release();
        }
    }
    return g_set_viewport(device, use);
}

HRESULT WINAPI hook_set_scissor(IDirect3DDevice9* device, const RECT* rect) {
    RECT expanded{};
    const RECT* use = rect;
    if (g_active && rect) {
        IDirect3DSurface9* target = nullptr;
        UINT bw = 0;
        UINT bh = 0;
        const LONG w = rect->right - rect->left;
        const LONG h = rect->bottom - rect->top;
        if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target_size(target, &bw, &bh) &&
            is_letterbox(rect->left, rect->top, w, h, static_cast<LONG>(bw), static_cast<LONG>(bh))) {
            expanded.left = 0;
            expanded.top = 0;
            expanded.right = static_cast<LONG>(bw);
            expanded.bottom = static_cast<LONG>(bh);
            use = &expanded;
            static bool logged = false;
            note_once(&logged, "Aspect: a 16:9 scissor was opened to the full target");
        }
        if (target) {
            target->Release();
        }
    }
    return g_set_scissor(device, use);
}

HRESULT WINAPI hook_stretch(IDirect3DDevice9* device, IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst,
                            const RECT* dst_rect, D3DTEXTUREFILTERTYPE filter) {
    RECT expanded{};
    const RECT* use = dst_rect;
    if (g_active && dst && dst_rect) {
        UINT bw = 0;
        UINT bh = 0;
        const LONG w = dst_rect->right - dst_rect->left;
        const LONG h = dst_rect->bottom - dst_rect->top;
        if (target_size(dst, &bw, &bh) &&
            is_letterbox(dst_rect->left, dst_rect->top, w, h, static_cast<LONG>(bw), static_cast<LONG>(bh))) {
            expanded.left = 0;
            expanded.top = 0;
            expanded.right = static_cast<LONG>(bw);
            expanded.bottom = static_cast<LONG>(bh);
            use = &expanded;
            static bool logged = false;
            note_once(&logged, "Aspect: a 16:9 present blit was opened to the full target");
        }
    }
    return g_stretch(device, src, src_rect, dst, use, filter);
}

// Same 16:9 perspective test as the shader-constant path. Fixed-function draws use this.
bool correct_matrix(float* m) {
    if (!g_active || m[0] < 0.05f || m[5] < 0.05f) {
        return false;
    }
    auto zero = [](float v) { return v < 0.02f && v > -0.02f; };
    if (!zero(m[1]) || !zero(m[2]) || !zero(m[4]) || !zero(m[6]) || !zero(m[8]) || !zero(m[9])) {
        return false;
    }
    const float ratio = m[5] / m[0];
    if (!near_f(ratio, kVanillaAspect, 0.035f)) {
        return false;
    }
    const bool row_major = near_f(m[11], 1.f, 0.02f) && zero(m[15]);
    const bool column_major = near_f(m[14], 1.f, 0.02f) && zero(m[15]);
    if (!row_major && !column_major) {
        return false;
    }
    const float y = m[0] * g_aspect;
    if (near_f(m[5], y, 0.0001f)) {
        return false;
    }
    m[5] = y;
    return true;
}

HRESULT WINAPI hook_set_transform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    D3DMATRIX copy{};
    const D3DMATRIX* use = matrix;
    if (g_active && matrix && state == D3DTS_PROJECTION) {
        copy = *matrix;
        if (correct_matrix(reinterpret_cast<float*>(&copy))) {
            use = &copy;
            static bool logged = false;
            note_once(&logged, "Aspect: a 16:9 projection was opened vertically");
        }
    }
    return g_set_transform(device, state, use);
}

HRESULT WINAPI hook_present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    const RECT* use = dst;
    if (g_active && dst) {
        D3DDEVICE_CREATION_PARAMETERS created{};
        HWND target = window;
        if (!target && SUCCEEDED(device->GetCreationParameters(&created))) {
            target = created.hFocusWindow;
        }
        if (target && g_cover) {
            g_window = target;
        }
        if (target) {
            RECT client{};
            if (GetClientRect(target, &client)) {
                const LONG bw = client.right - client.left;
                const LONG bh = client.bottom - client.top;
                const LONG w = dst->right - dst->left;
                const LONG h = dst->bottom - dst->top;
                if (is_letterbox(dst->left, dst->top, w, h, bw, bh)) {
                    use = nullptr;  // the whole client
                    static bool logged = false;
                    note_once(&logged, "Aspect: Present was letterboxed; using the full window");
                }
            }
        }
    }
    return g_present(device, src, use, window, dirty);
}

constexpr int kStretchSlot = 34;
constexpr int kSetTransformSlot = 44;
constexpr int kSetViewportSlot = 47;
constexpr int kSetScissorSlot = 75;
constexpr int kPresentSlot = 17;

bool correct_upload(const float* data, UINT count, float* out) {
    if (!g_active || !data || !out || count < 4 || count > 24) {
        return false;
    }
    std::memcpy(out, data, count * 4 * sizeof(float));
    bool changed = false;
    const UINT matrices = count / 4;
    const UINT limit = matrices > 4 ? 4 : matrices;
    for (UINT i = 0; i < limit; ++i) {
        if (correct_matrix(out + i * 16)) {
            changed = true;
        }
    }
    if (changed) {
        static bool logged = false;
        note_once(&logged, "Aspect: a 16:9 shader projection was opened vertically");
    }
    return changed;
}

}  // namespace

void aspect_set_enabled(bool on) {
    g_enabled = on;
}

bool aspect_is_enabled() {
    return g_enabled;
}

bool aspect_is_active() {
    return g_active;
}

void aspect_adjust_present(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp, const char* where) {
    if (!g_enabled || !d3d || !pp || pp->BackBufferWidth == 0 || pp->BackBufferHeight == 0) {
        if (!g_enabled) {
            g_active = false;
            g_cover = false;
            restore_camera_constants();
        }
        return;
    }
    Size display = display_size(d3d, adapter, pp->hDeviceWindow);
    if (display.w < 640 || display.h < 480) {
        return;
    }
    const D3DFORMAT format = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
    const UINT listed = taller_same_width(d3d, adapter, display.w, display.h, format);
    bool deck_guess = false;
    if (listed > display.h) {
        display.h = listed;
    } else if (steam_deck() && display.w == 1280 && display.h == 720) {
        // Gamescope often reports the 720p mode the game picked, so 1280x800 never appears.
        display.h = 800;
        deck_guess = true;
    }
    const float display_aspect = static_cast<float>(display.w) / static_cast<float>(display.h);
    const float requested = static_cast<float>(pp->BackBufferWidth) / static_cast<float>(pp->BackBufferHeight);
    const bool request_is_16_9 = near_f(requested, kVanillaAspect, 0.04f);
    const bool request_matches = pp->BackBufferWidth == display.w && pp->BackBufferHeight == display.h;
    // Take the display size when the game asked for 16:9 on a taller screen, or already asked for
    // that display. A smaller 16:10 window is left at its own size and only the bars inside it go.
    UINT target_w = pp->BackBufferWidth;
    UINT target_h = pp->BackBufferHeight;
    if (display_aspect < kWiderThan && (request_is_16_9 || request_matches)) {
        target_w = display.w;
        target_h = display.h;
    }
    const float aspect = static_cast<float>(target_w) / static_cast<float>(target_h);
    if (!(aspect < kWiderThan)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO("Aspect: %ux%u is 16:9 or wider; left unchanged", target_w, target_h);
        }
        g_active = false;
        g_cover = false;
        restore_camera_constants();
        return;
    }

    g_active = true;
    g_aspect = aspect;
    g_window = pp->hDeviceWindow;
    write_camera_constants(aspect);

    static UINT log_w = 0;
    static UINT log_h = 0;
    static UINT log_rw = 0;
    static UINT log_rh = 0;
    if (log_w != target_w || log_h != target_h || log_rw != pp->BackBufferWidth || log_rh != pp->BackBufferHeight) {
        LOG_INFO("Aspect: %s game asked for %ux%u; presenting %ux%u (%.3f)", where, pp->BackBufferWidth,
                 pp->BackBufferHeight, target_w, target_h, aspect);
        log_w = target_w;
        log_h = target_h;
        log_rw = pp->BackBufferWidth;
        log_rh = pp->BackBufferHeight;
    }
    const bool was_windowed = pp->Windowed != 0;
    const bool resize = pp->BackBufferWidth != target_w || pp->BackBufferHeight != target_h;
    if (resize) {
        pp->BackBufferWidth = target_w;
        pp->BackBufferHeight = target_h;
    }
    g_cover = false;
    if (!was_windowed && !mode_listed(d3d, adapter, target_w, target_h, format)) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        g_cover = true;
        LOG_INFO("Aspect: %ux%u is not an exclusive fullscreen mode; using a borderless window", target_w, target_h);
    } else if (was_windowed && (resize || (target_w == display.w && target_h == display.h))) {
        // The client has to match the backbuffer, or Present squashes a taller image into the old window.
        g_cover = true;
    }
    if (deck_guess) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO("Aspect: Steam Deck reported 1280x720, so the panel's 1280x800 was requested. If the picture is "
                     "squashed, set this game's resolution in Steam to 1280x800.");
        }
    }
    if (g_cover) {
        start_cover_thread();
        cover_window(g_window);
    }
}

void aspect_hook_device(IDirect3DDevice9* device) {
    if (!device || !g_active) {
        return;
    }
    void* viewport = patch_slot(device, kSetViewportSlot, reinterpret_cast<void*>(&hook_set_viewport));
    if (viewport && !g_set_viewport) {
        g_set_viewport = reinterpret_cast<SetViewportFn>(viewport);
    }
    void* scissor = patch_slot(device, kSetScissorSlot, reinterpret_cast<void*>(&hook_set_scissor));
    if (scissor && !g_set_scissor) {
        g_set_scissor = reinterpret_cast<SetScissorFn>(scissor);
    }
    void* stretch = patch_slot(device, kStretchSlot, reinterpret_cast<void*>(&hook_stretch));
    if (stretch && !g_stretch) {
        g_stretch = reinterpret_cast<StretchFn>(stretch);
    }
    void* transform = patch_slot(device, kSetTransformSlot, reinterpret_cast<void*>(&hook_set_transform));
    if (transform && !g_set_transform) {
        g_set_transform = reinterpret_cast<SetTransformFn>(transform);
    }
    void* present = patch_slot(device, kPresentSlot, reinterpret_cast<void*>(&hook_present));
    if (present && !g_present) {
        g_present = reinterpret_cast<PresentFn>(present);
    }
    static bool logged = false;
    if (!logged && g_set_viewport && g_present) {
        logged = true;
        LOG_INFO("Aspect: letterbox hooks installed");
    }
}

bool aspect_correct_constants(const float* data, UINT count, float* out) {
    return correct_upload(data, count, out);
}
