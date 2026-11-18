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

HRESULT WINAPI hk_set_ps_const(IDirect3DDevice9* dev, UINT start, const float* v, UINT count) {
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
}
#else
void dofwatch_hook_device(IDirect3DDevice9*) {}
#endif
