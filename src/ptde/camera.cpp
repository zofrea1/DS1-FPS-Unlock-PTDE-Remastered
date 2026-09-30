#include "camera.h"

#include "fixes.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kFollowUpdate = 0x00F02720;  // ChrFollowCam::Update(this, dt, chr, extra), callee pops 16
constexpr uint32_t kCallSites[] = {0x00F148DD, 0x00F148FE};

// Per-frame blend weights in the follow camera object.
constexpr uint32_t kFields[] = {0x238, 0x1BC};
constexpr int kFieldCount = 2;

using FollowFn = void(__stdcall*)(void*, float, void*, void*);
FollowFn g_orig = reinterpret_cast<FollowFn>(kFollowUpdate);
volatile long g_enabled = 1;

struct Field {
    float original = 0.0f;
    float written = -1.0f;
};
struct Camera {
    void* object = nullptr;
    Field field[kFieldCount];
    bool logged = false;
};
Camera g_cameras[4];

Camera* camera_slot(void* object) {
    Camera* free_slot = nullptr;
    for (auto& c : g_cameras) {
        if (c.object == object) return &c;
        if (!c.object && !free_slot) free_slot = &c;
    }
    if (!free_slot) free_slot = &g_cameras[0];
    *free_slot = Camera();
    free_slot->object = object;
    return free_slot;
}

// The value the game means for a weight after n frames' worth of smoothing in one step. Weights
// up to 0.5 are blend amounts (new = old + (target - old) * k); larger values are kept per-frame
// multipliers of the remaining error (error *= m), which scale by a plain power.
float scaled(float original, double n) {
    if (!(original > 0.0f) || original >= 1.0f) return original;
    if (original <= 0.5f) return static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(original), n));
    return static_cast<float>(std::pow(static_cast<double>(original), n));
}

bool read_field(void* object, uint32_t offset, float* out) {
    __try {
        *out = *reinterpret_cast<volatile float*>(static_cast<uint8_t*>(object) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_field(void* object, uint32_t offset, float value) {
    __try {
        *reinterpret_cast<volatile float*>(static_cast<uint8_t*>(object) + offset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void scale_camera(void* object, float dt) {
    if (!object) return;
    double n = static_cast<double>(dt) * 30.0;
    if (!(n > 0.02)) n = 0.02;
    if (n > 10.0) n = 10.0;
    Camera* cam = camera_slot(object);
    for (int i = 0; i < kFieldCount; ++i) {
        float value = 0.0f;
        if (!read_field(object, kFields[i], &value)) return;
        Field& f = cam->field[i];
        if (value != f.written) {
            f.original = value;  // the game (re)wrote it: that is the 30 FPS value
        }
        const float target = g_enabled ? scaled(f.original, n) : f.original;
        f.written = target;
        write_field(object, kFields[i], target);
    }
    if (!cam->logged) {
        cam->logged = true;
        LOG_INFO("Follow camera %p: blend weights +0x238=%.4f +0x1BC=%.4f (30 FPS values)", object,
                 cam->field[0].original, cam->field[1].original);
    }
}

void __stdcall follow_stub(void* camera, float dt, void* chr, void* extra) {
    scale_camera(camera, dt);
    g_orig(camera, dt, chr, extra);
}

}  // namespace

bool camera_install() {
    for (uint32_t site : kCallSites) {
        auto* p = reinterpret_cast<uint8_t*>(site);
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, 4);
        if (p[0] != 0xE8 || static_cast<uint32_t>(site + 5 + rel) != kFollowUpdate) {
            LOG_ERROR("Camera call site %08X does not call %08X. Not patching the camera.", site, kFollowUpdate);
            return false;
        }
    }
    for (uint32_t site : kCallSites) {
        auto* p = reinterpret_cast<uint8_t*>(site);
        DWORD old = 0;
        if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            LOG_ERROR("Could not unprotect the camera call at %08X (Win32=%lu)", site, GetLastError());
            return false;
        }
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&follow_stub) - (site + 5));
        std::memcpy(p + 1, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        DWORD ignored = 0;
        VirtualProtect(p + 1, 4, old, &ignored);
    }
    LOG_INFO("Follow camera smoothing follows the frame time (2 call sites)");
    return true;
}

void camera_set_enabled(bool on) {
    g_enabled = on ? 1 : 0;
}

bool camera_enabled() {
    return g_enabled != 0;
}
