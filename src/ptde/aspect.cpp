#include "aspect.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "user32.lib")

namespace {

// Every perspective the game draws with is built by one function, 0x453720 (image base 0x400000, not
// relocated): vertical field of view in xmm0, then aspect, near and far on the stack, the matrix to eax
// (Camera::GetProjection 0x467970 passes camera +0x50, +0x54, +0x58, +0x5C). The screen cameras ask for 16:9.
// On a taller display those get the display's aspect and a vertical view opened so the horizontal one stays
// (Vert+), so the world, effects (their shaders take the combined view-projection) and the game's own screen
// positions (lock-on, health bars) all agree. The cameras' constants (43 degrees 0x11E8134, 16/9 0x11E8484)
// are not the place: the player camera eases +0x50 toward its camera parameter every frame (0xF02720).
constexpr uint32_t kPerspective = 0x00453720;
constexpr uint8_t kPerspectivePrologue[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x70};
constexpr float kVanillaAspect = 16.f / 9.f;
constexpr float kVanillaFovY = 0.75049156f;
constexpr float kPi = 3.14159265f;

// Below this, the display is taller than 16:9 (16:10 is 1.6, 16:9 is 1.778); above kWide it is wider (21:9 is 2.33 or
// 2.39, 32:9 is 3.56).
constexpr float kWiderThan = 1.70f;
constexpr float kWide = 1.80f;

bool other_than_16_9(float aspect) {
    return aspect < kWiderThan || aspect > kWide;
}

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
#if FPSU_DIAGNOSTICS
    // Testing on a PC as another screen: FPSU_TEST_DISPLAY=WIDTHxHEIGHT (with SteamDeck=1 and 1280x720, the Deck's
    // Gamescope report).
    char test[32] = {};
    if (GetEnvironmentVariableA("FPSU_TEST_DISPLAY", test, sizeof(test)) > 0) {
        unsigned w = 0, h = 0;
        if (std::sscanf(test, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 480) return Size{w, h};
    }
#endif
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

bool g_proj_hooked = false;
void* g_persp_orig = nullptr;

void __cdecl adjust_perspective(float* fov, float* aspect) {
    if (!g_active || !near_f(*aspect, kVanillaAspect, 0.01f)) {
        return;  // shadow and light cameras use other aspects
    }
    const float f = *fov;
    if (!(f > 0.05f && f < 3.0f)) {
        return;
    }
    // Taller than 16:9: the vertical view opens so the horizontal one stays (Vert+). Wider: the vertical view stays
    // and the wider aspect adds to the sides (Hor+).
    if (g_aspect < kVanillaAspect) *fov = 2.f * std::atan(std::tan(f * 0.5f) * kVanillaAspect / g_aspect);
    *aspect = g_aspect;
}

// The game's caller keeps nothing in the xmm registers across this call (0x453720 clobbers xmm0-7 itself), but
// they are kept anyway; only the field of view in xmm0 and the aspect on the stack change.
__declspec(naked) void hk_perspective() {
    __asm {
        pushfd
        pushad
        sub esp, 0x80
        movdqu [esp], xmm0
        movdqu [esp + 0x10], xmm1
        movdqu [esp + 0x20], xmm2
        movdqu [esp + 0x30], xmm3
        movdqu [esp + 0x40], xmm4
        movdqu [esp + 0x50], xmm5
        movdqu [esp + 0x60], xmm6
        movdqu [esp + 0x70], xmm7
        lea eax, [esp + 0xA8]  // the aspect: past the saved xmm (0x80), pushad, pushfd and our return address
        push eax
        lea eax, [esp + 4]     // the saved xmm0: the vertical field of view
        push eax
        call adjust_perspective
        add esp, 8
        movdqu xmm0, [esp]
        movdqu xmm1, [esp + 0x10]
        movdqu xmm2, [esp + 0x20]
        movdqu xmm3, [esp + 0x30]
        movdqu xmm4, [esp + 0x40]
        movdqu xmm5, [esp + 0x50]
        movdqu xmm6, [esp + 0x60]
        movdqu xmm7, [esp + 0x70]
        add esp, 0x80
        popad
        popfd
        jmp dword ptr [g_persp_orig]
    }
}

// Once, on the game's thread inside CreateDevice/Reset, before anything is drawn with the new size.
void install_projection_hook() {
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    auto* site = reinterpret_cast<uint8_t*>(kPerspective);
    if (std::memcmp(site, kPerspectivePrologue, sizeof(kPerspectivePrologue)) != 0) {
        LOG_INFO("Aspect: the projection code does not match this build; correcting the draw instead");
        return;
    }
    constexpr size_t kStolen = sizeof(kPerspectivePrologue);
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) {
        return;
    }
    std::memcpy(tramp, site, kStolen);  // push ebp; mov ebp,esp; and esp,-16; sub esp,70h: no relative operands
    tramp[kStolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((kPerspective + kStolen) - reinterpret_cast<uintptr_t>(tramp + kStolen + 5));
    std::memcpy(tramp + kStolen + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 32);
    g_persp_orig = tramp;
    DWORD old = 0;
    if (!VirtualProtect(site, kStolen, PAGE_EXECUTE_READWRITE, &old)) {
        return;
    }
    site[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_perspective) - (kPerspective + 5));
    std::memcpy(site + 1, &rel, 4);
    std::memset(site + 5, 0x90, kStolen - 5);
    FlushInstructionCache(GetCurrentProcess(), site, kStolen);
    DWORD ignored = 0;
    VirtualProtect(site, kStolen, old, &ignored);
    g_proj_hooked = true;
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

// A 16:9 rectangle centered in a taller surface (the game's letterbox) or in a wider one (its pillarbox).
bool is_letterbox(LONG x, LONG y, LONG w, LONG h, LONG bw, LONG bh) {
    if (bw < 16 || bh < 16 || w < 16 || h < 16) {
        return false;
    }
    if (static_cast<float>(bw) / static_cast<float>(bh) > kWide) {
        if (h * 10 < bh * 9 || y > bh / 25 || w + 8 >= bw) {
            return false;
        }
        if (!near_f(static_cast<float>(w) / static_cast<float>(h), kVanillaAspect, 0.04f)) {
            return false;
        }
        const LONG dx = x - (bw - w) / 2;
        return dx < 8 && dx > -8;
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

bool open_letterbox(IDirect3DSurface9* target, RECT* r) {
    if (!g_active || !target || !r) {
        return false;
    }
    UINT bw = 0;
    UINT bh = 0;
    if (!target_size(target, &bw, &bh) || !is_letterbox(r->left, r->top, r->right - r->left, r->bottom - r->top,
                                                        static_cast<LONG>(bw), static_cast<LONG>(bh))) {
        return false;
    }
    *r = RECT{0, 0, static_cast<LONG>(bw), static_cast<LONG>(bh)};
    static bool logged = false;
    note_once(&logged, "Aspect: a 16:9 present blit was opened to the full target");
    return true;
}

HRESULT WINAPI hook_stretch(IDirect3DDevice9* device, IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst,
                            const RECT* dst_rect, D3DTEXTUREFILTERTYPE filter) {
    RECT expanded = dst_rect ? *dst_rect : RECT{};
    const RECT* use = dst_rect && open_letterbox(dst, &expanded) ? &expanded : dst_rect;
    return g_stretch(device, src, src_rect, dst, use, filter);
}

// Same 16:9 perspective test as the shader-constant path. Fixed-function draws use this.
bool correct_matrix(float* m) {
    if (!g_active || g_proj_hooked || m[0] < 0.05f || m[5] < 0.05f) {
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
    if (g_aspect > kVanillaAspect) {
        const float x = m[5] / g_aspect;  // wider: the vertical scale stays, the horizontal one shrinks
        if (near_f(m[0], x, 0.0001f)) {
            return false;
        }
        m[0] = x;
        return true;
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
    if (!g_active || g_proj_hooked || !data || !out || count < 4 || count > 24) {
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

float aspect_value() {
    return g_active ? g_aspect : kVanillaAspect;
}

void aspect_adjust_present(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp, const char* where) {
    if (!g_enabled || !d3d || !pp || pp->BackBufferWidth == 0 || pp->BackBufferHeight == 0) {
        if (!g_enabled) {
            g_active = false;
            g_cover = false;
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
    if (other_than_16_9(display_aspect) && (request_is_16_9 || request_matches)) {
        target_w = display.w;
        target_h = display.h;
    }
    const float aspect = static_cast<float>(target_w) / static_cast<float>(target_h);
    if (!other_than_16_9(aspect)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO("Aspect: %ux%u is 16:9; left unchanged", target_w, target_h);
        }
        g_active = false;
        g_cover = false;
        return;
    }

    g_active = true;
    g_aspect = aspect;
    g_window = pp->hDeviceWindow;
    install_projection_hook();

    static UINT log_w = 0;
    static UINT log_h = 0;
    static UINT log_rw = 0;
    static UINT log_rh = 0;
    if (log_w != target_w || log_h != target_h || log_rw != pp->BackBufferWidth || log_rh != pp->BackBufferHeight) {
        LOG_INFO("Aspect: %s game asked for %ux%u; presenting %ux%u (%.3f)", where, pp->BackBufferWidth,
                 pp->BackBufferHeight, target_w, target_h, aspect);
        if (g_proj_hooked && aspect < kVanillaAspect) {
            LOG_INFO("Aspect: screen cameras use aspect %.3f; horizontal view unchanged, vertical view %.1f -> %.1f deg "
                     "(at the default 43 deg)", aspect, kVanillaFovY * (180.f / kPi), vert_plus_fovy(aspect) * (180.f / kPi));
        } else if (g_proj_hooked) {
            const float hx = 2.f * std::atan(std::tan(kVanillaFovY * 0.5f) * kVanillaAspect) * (180.f / kPi);
            const float hw = 2.f * std::atan(std::tan(kVanillaFovY * 0.5f) * aspect) * (180.f / kPi);
            LOG_INFO("Aspect: screen cameras use aspect %.3f; vertical view unchanged, horizontal view %.1f -> %.1f deg "
                     "(at the default 43 deg)", aspect, hx, hw);
        }
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

// One slot: our hook in front of the device's method, once. Called again after a Reset (hook_reset): a slot that
// still leads elsewhere than the method we saved (another hook of this mod or another program went in front of ours
// and passes calls on to it) is left as it is. Patching it again put our hook in front of those, and ours calls the
// saved method directly: every hook installed after ours was skipped from the first Reset on (on the Steam Deck, which
// resets to 1280x800 at start: EffectsResolution's viewports and the menus' clipping rectangles, the frame's
// scale-down, the Present hooks). Only a slot back on the saved method (our hook gone) is patched again.
template <typename Fn>
void hook_once(IDirect3DDevice9* device, int slot, void* hook, Fn* original) {
    void** vtable = *reinterpret_cast<void***>(device);
    void* current = vtable[slot];
    if (current == hook) return;
    if (*original && current != reinterpret_cast<void*>(*original)) return;
    void* previous = patch_slot(device, slot, hook);
    if (previous && !*original) *original = reinterpret_cast<Fn>(previous);
}

void aspect_hook_device(IDirect3DDevice9* device) {
    if (!device || !g_active) {
        return;
    }
    hook_once(device, kSetViewportSlot, reinterpret_cast<void*>(&hook_set_viewport), &g_set_viewport);
    hook_once(device, kSetScissorSlot, reinterpret_cast<void*>(&hook_set_scissor), &g_set_scissor);
    hook_once(device, kStretchSlot, reinterpret_cast<void*>(&hook_stretch), &g_stretch);
    hook_once(device, kSetTransformSlot, reinterpret_cast<void*>(&hook_set_transform), &g_set_transform);
    hook_once(device, kPresentSlot, reinterpret_cast<void*>(&hook_present), &g_present);
    static bool logged = false;
    if (!logged && g_set_viewport && g_present) {
        logged = true;
        LOG_INFO("Aspect: letterbox hooks installed");
    }
}

bool aspect_open_letterbox(IDirect3DSurface9* target, RECT* r) { return open_letterbox(target, r); }

bool aspect_correct_constants(const float* data, UINT count, float* out) {
    return correct_upload(data, count, out);
}
