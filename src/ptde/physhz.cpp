#include "physhz.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kWorldChrMan = 0x0137DC70;
constexpr uint32_t kPhysUpdate = 0x00EC3A90;
constexpr uint32_t kGraze = 0x00E3A8A0;

volatile int g_hz = 0;
double g_acc = 0.0;
bool g_fire = false;
double g_fire_dt = 0.0;
uint32_t g_frame_id = 1;
uint32_t g_phys_ran = 0;
uint32_t g_graze_ran = 0;
uint32_t g_mc = 0, g_phys = 0;  // the player's, refreshed once a frame
// The animation writes this frame's root-motion displacement to [phys+0x60] (vec4) every frame, and the
// velocity builder (fn 0xEC0BB0) turns it into a velocity by multiplying with 1/dt. On skipped frames
// it is added up here and handed to the step frame whole, otherwise only the last frame's motion is
// used with the accumulated dt and the player moves at Hz/FPS of the normal speed.
float g_disp_acc[4] = {};
void* g_phys_tramp = nullptr;
void* g_graze_tramp = nullptr;
bool g_installed = false;

uint32_t read_u32(uint32_t address) {
    __try {
        return *reinterpret_cast<const uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void refresh_player() {
    g_mc = g_phys = 0;
    const uint32_t manager = read_u32(kWorldChrMan);
    if (!manager) return;
    const uint32_t first = read_u32(manager + 4);
    const uint32_t chr = first ? read_u32(first) : 0;
    const uint32_t mc = chr ? read_u32(chr + 0x28) : 0;
    const uint32_t phys = mc ? read_u32(mc + 0x1C) : 0;
    g_mc = mc;
    g_phys = phys;
}

using PhysFn = void(__stdcall*)(void* controller, float dt);

void __stdcall hk_phys(void* controller, float dt) {
    const PhysFn orig = reinterpret_cast<PhysFn>(g_phys_tramp);
    if (g_hz == 0 || reinterpret_cast<uint32_t>(controller) != g_phys || g_phys == 0) {
        orig(controller, dt);
        return;
    }
    if (!g_fire || g_phys_ran == g_frame_id) return;  // not a step frame: the body stays where it is
    g_phys_ran = g_frame_id;
    auto* disp = reinterpret_cast<float*>(static_cast<uint8_t*>(controller) + 0x60);
    for (int i = 0; i < 4; ++i) {
        disp[i] += g_disp_acc[i];
        g_disp_acc[i] = 0.0f;
    }
    fixes_override_dt(g_fire_dt);
    orig(controller, static_cast<float>(g_fire_dt));
    fixes_restore_dt();
}

extern "C" uint32_t __cdecl physhz_graze_should_run(uint32_t mc) {
    if (g_hz == 0 || mc != g_mc || g_mc == 0) return 1;
    if (!g_fire) {
        if (g_graze_ran != g_frame_id && g_phys) {
            g_graze_ran = g_frame_id;  // once per frame
            const auto* disp = reinterpret_cast<const float*>(g_phys + 0x60);
            for (int i = 0; i < 4; ++i) g_disp_acc[i] += disp[i];
        }
        return 0;
    }
    if (g_graze_ran == g_frame_id) return 0;
    g_graze_ran = g_frame_id;
    return 1;
}

__declspec(naked) void hk_graze() {
    __asm {
        pushad
        push edi
        call physhz_graze_should_run
        add esp, 4
        test al, al
        popad
        jz skip
        jmp dword ptr [g_graze_tramp]
    skip:
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

bool write_jump(uint32_t site, size_t stolen, void* target) {
    auto* p = reinterpret_cast<uint8_t*>(site);
    DWORD old = 0;
    if (!VirtualProtect(p, stolen, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < stolen; ++i) p[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), p, stolen);
    DWORD ignored = 0;
    VirtualProtect(p, stolen, old, &ignored);
    return true;
}

}  // namespace

bool physhz_install(const Settings& settings) {
    // push ebp; mov ebp,esp; and esp,-16 ; sub esp,44h
    static const uint8_t kPhysExpect[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44};
    // push ebp; mov ebp,esp; and esp,-16 ; mov eax,[edi+10h]
    static const uint8_t kGrazeExpect[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x47, 0x10};
    g_phys_tramp = make_trampoline(kPhysUpdate, kPhysExpect, 6);
    g_graze_tramp = make_trampoline(kGraze, kGrazeExpect, 6);
    if (!g_phys_tramp || !g_graze_tramp) {
        LOG_ERROR("Fixed-rate physics experiment: the physics update or graze check does not match this build");
        return false;
    }
    // Both entries are checked in full before either is patched.
    if (std::memcmp(reinterpret_cast<void*>(kPhysUpdate), kPhysExpect, sizeof(kPhysExpect)) != 0 ||
        std::memcmp(reinterpret_cast<void*>(kGraze), kGrazeExpect, sizeof(kGrazeExpect)) != 0) {
        return false;
    }
    if (!write_jump(kPhysUpdate, 6, reinterpret_cast<void*>(&hk_phys)) ||
        !write_jump(kGraze, 6, reinterpret_cast<void*>(&hk_graze))) {
        LOG_ERROR("Fixed-rate physics experiment: could not patch");
        return false;
    }
    g_hz = settings.physics_hz;
    g_installed = true;
    LOG_INFO("Fixed-rate physics experiment installed (PhysicsHz = %d; Ctrl+8 cycles it with Trace = true)", g_hz);
    return true;
}

void physhz_frame(double dt) {
    if (!g_installed) return;
    ++g_frame_id;
    refresh_player();
    if (g_hz == 0) {
        g_fire = false;
        g_acc = 0.0;
        return;
    }
    g_acc += dt;
    const double step = 1.0 / static_cast<double>(g_hz);
    if (g_acc >= step * 0.999) {
        g_fire = true;
        g_fire_dt = g_acc;
        g_acc = 0.0;
    } else {
        g_fire = false;
    }
}

void physhz_poll_hotkeys() {
    if (!g_installed) return;
    static bool down_was = false;
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState('8') & 0x8000) != 0;
    if (down && !down_was) {
        g_hz = g_hz == 0 ? 30 : (g_hz == 30 ? 60 : 0);
        g_acc = 0.0;
        for (float& v : g_disp_acc) v = 0.0f;
        LOG_INFO("Fixed-rate physics is now %s", g_hz == 0 ? "OFF (every frame)" : (g_hz == 30 ? "30 Hz" : "60 Hz"));
    }
    down_was = down;
}
