#include "dofwatch.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstring>

// Diagnostics (2026-11-18): as an invader in Prepare to Co-op the depth of field blurred from about 13 m with a hard
// line where at home the same place (the same DofBank row, 10 or 11: 40 -> 180 m) is nearly sharp. The depth of field
// passes get the game's pixel constants c7..c71 in one call; c8 = the camera's near clip, far clip, near - far and
// near * far (the depth turned into metres), c9 = the far blur's start, end and strength, c10 = the near blur's, c11
// two more. Each change of c8..c11 (more than 1%) is logged, at most once a second and 300 times.
#if FPSU_DIAGNOSTICS
namespace {

using SetPsConstFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
SetPsConstFn o_set_ps_const = nullptr;
SetPsConstFn o_set_vs_const = nullptr;
// The last perspective camera's near and far clip, from a vertex shader's c0..c3 (the world-view-projection rows: the
// depth row is A * the w row + B, A = f / (f - n), B = -n * A), when it looks like the scene's (far over 100).
float g_cam_near = 0.0f, g_cam_far = 0.0f;
int g_mismatch_lines = 0;
ULONGLONG g_mismatch_at = 0;
float g_last[16] = {};
bool g_have = false;
int g_lines = 0;
ULONGLONG g_last_at = 0;

bool differs(const float* a, const float* b) {
    for (int i = 0; i < 16; ++i) {
        const float d = std::fabs(a[i] - b[i]);
        if (d > 0.01f * (std::fabs(b[i]) > 1.0f ? std::fabs(b[i]) : 1.0f)) return true;
    }
    return false;
}

HRESULT WINAPI hk_set_vs_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    if (v && start == 0 && count >= 4) {
        const float* z = v + 8;
        const float* w = v + 12;
        const int k = std::fabs(w[0]) > std::fabs(w[2]) ? 0 : 2;
        if (std::fabs(w[k]) > 1e-4f) {
            const float a = z[k] / w[k];
            const float b = z[3] - a * w[3];
            if (a > 1.0f && a < 2.0f && b < 0.0f) {
                const float n = -b / a, f = a * n / (a - 1.0f);
                if (n > 0.0005f && n < 10.0f && f > 100.0f) {
                    g_cam_near = n;
                    g_cam_far = f;
                }
            }
        }
    }
    return o_set_vs_const(dev, start, v, count);
}

HRESULT WINAPI hk_set_ps_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
    if (v && start == 7 && count == 65 && g_cam_near > 0.0f && g_mismatch_lines < 300) {
        // The depth of field turns the depth back into metres with c8's near clip: a scene drawn with another near
        // clip scales every distance by about c8 near / scene near.
        const float n8 = v[4];
        const ULONGLONG now = GetTickCount64();
        if (n8 > 0.0f && std::fabs(n8 - g_cam_near) > 0.02f * n8 && now - g_mismatch_at >= 1000) {
            g_mismatch_at = now;
            ++g_mismatch_lines;
            LOG_INFO("DofWatch: the scene's camera clips at %.4f / %.1f, the depth of field decodes the depth with %.4f / "
                     "%.1f: distances read %.2fx",
                     g_cam_near, g_cam_far, n8, v[5], n8 / g_cam_near);
        }
    }
    if (v && start == 7 && count == 65 && g_lines < 300) {  // the game's own block for the depth of field passes
        const float* c = v + (8 - start) * 4;
        const ULONGLONG now = GetTickCount64();
        if ((!g_have || differs(c, g_last)) && now - g_last_at >= 1000) {
            std::memcpy(g_last, c, sizeof(g_last));
            g_have = true;
            g_last_at = now;
            ++g_lines;
            LOG_INFO("DofWatch: the depth of field's constants: near/far clip %.4f / %.1f (c8 %.4g %.4g %.4g %.4g), far "
                     "blur %.1f -> %.1f m strength %.2f, near blur %.2f -> %.2f m strength %.2f, c11 %.3g %.3g %.3g "
                     "%.3g%s",
                     c[0], c[1], c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[8], c[9], c[10], c[12], c[13], c[14],
                     c[15], g_lines == 300 ? " (no more are logged)" : "");
        }
    }
    return o_set_ps_const(dev, start, v, count);
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

}  // namespace

void dofwatch_hook_device(IDirect3DDevice9* dev) {
    if (!dev) return;
    void* p = patch_slot(dev, 109, reinterpret_cast<void*>(&hk_set_ps_const));
    if (p && !o_set_ps_const) o_set_ps_const = reinterpret_cast<SetPsConstFn>(p);
    p = patch_slot(dev, 94, reinterpret_cast<void*>(&hk_set_vs_const));
    if (p && !o_set_vs_const) o_set_vs_const = reinterpret_cast<SetPsConstFn>(p);
}
#else
void dofwatch_hook_device(IDirect3DDevice9*) {}
#endif
