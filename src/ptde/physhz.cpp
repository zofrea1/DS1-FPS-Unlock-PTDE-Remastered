#include "physhz.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kWorldChrMan = 0x0137DC70;
constexpr uint32_t kPhysUpdate = 0x00EC3A90;  // stdcall(phys, dt)
constexpr uint32_t kGraze = 0x00E3A8A0;       // edi = movement controller; calls kPhysUpdate, then the graze check
constexpr uint32_t kSyncCall = 0x00E3A590;    // call 0xEBDA70 (esi = phys): [phys+0x20] = [phys+0x10]; [phys+0x10] = proxy
constexpr uint32_t kSyncFn = 0x00EBDA70;

volatile long g_on = 0;
int g_hz = 30;

// Render thread (frame boundary).
double g_acc = 0.0;
volatile bool g_fire = false;
volatile double g_fire_dt = 1.0 / 30.0;
volatile float g_alpha = 0.0f;
volatile uint32_t g_frame_id = 1;
volatile uint32_t g_mc = 0, g_phys = 0;  // the player's, refreshed once a frame

// Simulation thread.
uint32_t g_graze_frame = 0;   // frame the graze routine was handled for the player
bool g_step_pending = false;  // a physics step ran for the player since the last sync
bool g_in_step = false;       // inside the player's step (graze routine) with the step's dt applied
float g_disp_acc[4] = {};     // root motion of skipped frames ([phys+0x60], one frame's displacement)

// Interpolation: the last two step results, the true position and what was written for display.
struct Vec3 {
    float x, y, z;
};
bool g_have_steps = false;
Vec3 g_prev{}, g_cur{};
bool g_have_written = false;
Vec3 g_written{}, g_true{};

void* g_phys_tramp = nullptr;
void* g_graze_tramp = nullptr;
void* g_sync_target = nullptr;
bool g_installed = false;

uint32_t read_u32(uint32_t address) {
    __try {
        return *reinterpret_cast<const uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void refresh_player() {
    const uint32_t manager = read_u32(kWorldChrMan);
    const uint32_t first = manager ? read_u32(manager + 4) : 0;
    const uint32_t chr = first ? read_u32(first) : 0;
    const uint32_t mc = chr ? read_u32(chr + 0x28) : 0;
    const uint32_t phys = mc ? read_u32(mc + 0x1C) : 0;
    g_mc = mc;
    g_phys = phys;
}

Vec3* pos(uint32_t phys) {
    return reinterpret_cast<Vec3*>(phys + 0x10);
}

bool same(const Vec3& a, const Vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

float dist2(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

using PhysFn = void(__stdcall*)(void* phys, float dt);

void __stdcall hk_phys(void* phys, float dt) {
    const PhysFn orig = reinterpret_cast<PhysFn>(g_phys_tramp);
    if (g_in_step && reinterpret_cast<uint32_t>(phys) == g_phys) {
        orig(phys, static_cast<float>(g_fire_dt));
        return;
    }
    orig(phys, dt);
}

// Graze routine entry for any character: 1 = run it, 0 = skip it this frame.
extern "C" uint32_t __cdecl physhz_graze_enter(uint32_t mc) {
    const uint32_t phys = g_phys;
    if (!g_on || mc != g_mc || mc == 0 || phys == 0) return 1;
    if (g_graze_frame == g_frame_id) return 0;  // once a frame
    g_graze_frame = g_frame_id;
    float* disp = reinterpret_cast<float*>(phys + 0x60);
    if (!g_fire) {
        for (int i = 0; i < 4; ++i) g_disp_acc[i] += disp[i];
        return 0;
    }
    // Step frame. The true position goes back in, unless something else moved the body since.
    Vec3* p = pos(phys);
    if (g_have_written && same(*p, g_written)) *p = g_true;
    g_have_written = false;
    // The graze check measures the last sync's movement: make that the last step's.
    if (g_have_steps) *reinterpret_cast<Vec3*>(phys + 0x20) = g_prev;
    for (int i = 0; i < 4; ++i) {
        disp[i] += g_disp_acc[i];
        g_disp_acc[i] = 0.0f;
    }
    g_step_pending = true;
    g_in_step = true;
    fixes_override_dt(g_fire_dt);
    return 1;
}

extern "C" void __cdecl physhz_graze_leave() {
    if (!g_in_step) return;
    g_in_step = false;
    fixes_restore_dt();
}

__declspec(naked) void hk_graze() {
    __asm {
        pushad
        push edi
        call physhz_graze_enter
        add esp, 4
        test eax, eax
        popad
        jz skip
        call dword ptr [g_graze_tramp]
        pushad
        call physhz_graze_leave
        popad
    skip:
        ret
    }
}

// After the per-frame sync of [phys+0x10] from the proxy.
extern "C" void __cdecl physhz_after_sync(uint32_t phys) {
    if (!g_on || phys != g_phys || phys == 0) {
        if (phys == g_phys) {
            g_have_steps = false;
            g_have_written = false;
        }
        return;
    }
    Vec3* p = pos(phys);
    const Vec3 t = *p;
    const bool stepped = g_step_pending;
    g_step_pending = false;
    if (stepped && g_have_steps) {
        g_prev = g_cur;
        g_cur = t;
    } else if (!g_have_steps || !same(t, g_cur)) {
        g_prev = g_cur = t;  // first frame, or moved outside the physics step (warp, grab, ladder)
        g_have_steps = true;
    }
    if (dist2(g_prev, g_cur) > 4.0f) g_prev = g_cur;  // more than 2 units in one step: do not smear it
    const float a = g_alpha;
    Vec3 v{g_prev.x + (g_cur.x - g_prev.x) * a, g_prev.y + (g_cur.y - g_prev.y) * a,
           g_prev.z + (g_cur.z - g_prev.z) * a};
    g_true = t;
    g_written = v;
    g_have_written = true;
    *p = v;
}

__declspec(naked) void hk_sync() {
    __asm {
        call dword ptr [g_sync_target]
        pushad
        pushfd
        push esi
        call physhz_after_sync
        add esp, 4
        popfd
        popad
        ret
    }
}

void* make_trampoline(uint32_t site, const uint8_t* expect, size_t stolen) {
    auto* p = reinterpret_cast<uint8_t*>(site);
    if (std::memcmp(p, expect, stolen) != 0) return nullptr;
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return nullptr;
    std::memcpy(t, p, stolen);
    t[stolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((site + stolen) - reinterpret_cast<uintptr_t>(t + stolen + 5));
    std::memcpy(t + stolen + 1, &back, 4);
    return t;
}

bool write_branch(uint32_t site, uint8_t opcode, size_t size, void* target) {
    auto* p = reinterpret_cast<uint8_t*>(site);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = opcode;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < size; ++i) p[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

void set_on(bool on) {
    g_acc = 0.0;
    g_fire = false;
    g_on = on ? 1 : 0;
}

}  // namespace

bool physhz_install(const Settings& settings) {
    const bool wanted = settings.fixed_rate_physics;
    if (!wanted && !settings.trace) return true;  // Ctrl+8 needs it installed
    // push ebp; mov ebp,esp; and esp,-16 ; sub esp,44h
    static const uint8_t kPhysExpect[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44};
    // push ebp; mov ebp,esp; and esp,-16 ; mov eax,[edi+10h]
    static const uint8_t kGrazeExpect[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x47, 0x10};
    auto* sync = reinterpret_cast<uint8_t*>(kSyncCall);
    int32_t sync_rel = 0;
    std::memcpy(&sync_rel, sync + 1, 4);
    if (sync[0] != 0xE8 || kSyncCall + 5 + static_cast<uint32_t>(sync_rel) != kSyncFn ||
        std::memcmp(reinterpret_cast<void*>(kPhysUpdate), kPhysExpect, sizeof(kPhysExpect)) != 0 ||
        std::memcmp(reinterpret_cast<void*>(kGraze), kGrazeExpect, sizeof(kGrazeExpect)) != 0) {
        LOG_ERROR("Fixed-rate physics: the physics step, graze check or position sync does not match this build");
        return false;
    }
    g_phys_tramp = make_trampoline(kPhysUpdate, kPhysExpect, 6);
    g_graze_tramp = make_trampoline(kGraze, kGrazeExpect, 6);
    g_sync_target = reinterpret_cast<void*>(kSyncFn);
    if (!g_phys_tramp || !g_graze_tramp) return false;
    if (!write_branch(kPhysUpdate, 0xE9, 6, reinterpret_cast<void*>(&hk_phys)) ||
        !write_branch(kGraze, 0xE9, 6, reinterpret_cast<void*>(&hk_graze)) ||
        !write_branch(kSyncCall, 0xE8, 5, reinterpret_cast<void*>(&hk_sync))) {
        LOG_ERROR("Fixed-rate physics: could not patch");
        return false;
    }
    g_hz = settings.physics_hz >= 10 && settings.physics_hz <= 240 ? settings.physics_hz : 30;
    set_on(wanted);
    g_installed = true;
    LOG_INFO("Fixed-rate player physics installed: %s, %d steps a second, interpolated (Ctrl+8 toggles with Trace = true)",
             wanted ? "on" : "off", g_hz);
    return true;
}

void physhz_frame(double dt) {
    if (!g_installed) return;
    refresh_player();
    if (!g_on) {
        g_fire = false;
        ++g_frame_id;
        return;
    }
    const double step = 1.0 / static_cast<double>(g_hz);
    g_acc += dt;
    if (g_acc >= step * 0.999) {
        // Exactly one step's time while the frame rate is above the step rate, so the step is the
        // original game's; a slow frame takes all of it (one step can run per frame).
        const double take = g_acc < 2.0 * step ? (g_acc < step ? g_acc : step) : g_acc;
        g_fire_dt = take;
        g_acc -= take;
        if (g_acc < 0.0) g_acc = 0.0;
        g_fire = true;
    } else {
        g_fire = false;
    }
    double a = g_acc / step;
    g_alpha = static_cast<float>(a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a));
    ++g_frame_id;
}

void physhz_poll_hotkeys() {
    if (!g_installed) return;
    static bool down_was = false;
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState('8') & 0x8000) != 0;
    if (down && !down_was) {
        set_on(!g_on);
        LOG_INFO("Fixed-rate player physics is now %s", g_on ? "ON" : "OFF (every frame)");
    }
    down_was = down;
}
