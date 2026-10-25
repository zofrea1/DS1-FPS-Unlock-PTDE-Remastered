#include "slidesteer.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

constexpr uint32_t kLoad = 0x007BF429;       // mov ecx, [esp+18]; fld dword [ecx+20]
constexpr uint32_t kLoadNext = 0x007BF430;   // fcomp st(1)
constexpr uint32_t kReload = 0x007BF58A;     // fstp st(0); fld dword [ecx+20]
constexpr uint32_t kReloadNext = 0x007BF58F;
constexpr uint32_t kCaller = 0x00D98623;     // the one call of 0x7BF200
constexpr uint32_t kHavok = 0x007BF200;
constexpr uint32_t kThreshold = 0x0110B1D8;  // 0.1f, copied onto the skeleton

// Read by the two caves. 1 at 30 FPS and below. Address-taken, so the frame update's store is visible
// to the game thread's fmul.
alignas(4) float g_scale = 1.0f;
bool g_installed = false;

bool write_bytes(void* address, const void* bytes, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old, &ignored);
    return true;
}

void emit(uint8_t*& p, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) {
        *p++ = b;
    }
}

void emit32(uint8_t*& p, uint32_t v) {
    std::memcpy(p, &v, 4);
    p += 4;
}

void emit_rel(uint8_t*& p, uint32_t target) {
    emit32(p, target - (reinterpret_cast<uint32_t>(p) + 4));
}

}  // namespace

bool slidesteer_install() {
    static const uint8_t kLoadBytes[7] = {0x8B, 0x4C, 0x24, 0x18, 0xD9, 0x41, 0x20};
    static const uint8_t kReloadBytes[5] = {0xDD, 0xD8, 0xD9, 0x41, 0x20};
    const float threshold = *reinterpret_cast<const float*>(kThreshold);
    auto* call = reinterpret_cast<const uint8_t*>(kCaller);
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    if (std::memcmp(reinterpret_cast<const void*>(kLoad), kLoadBytes, sizeof(kLoadBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kReload), kReloadBytes, sizeof(kReloadBytes)) != 0 ||
        call[0] != 0xE8 || static_cast<uint32_t>(kCaller + 5 + rel) != kHavok ||
        !(threshold > 0.05f && threshold < 0.2f)) {
        LOG_ERROR("Slide steering sites do not match this build (threshold %.4f). Not patching.", threshold);
        return false;
    }
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) {
        LOG_ERROR("Could not allocate the slide steering cave (Win32=%lu)", GetLastError());
        return false;
    }
    const uint32_t scale = reinterpret_cast<uint32_t>(&g_scale);
    uint8_t* p = cave;

    // Same stack as the game: ecx = [esp+18], st(0) = threshold * scale, then back to fcomp.
    uint8_t* load_stub = p;
    emit(p, {0x8B, 0x4C, 0x24, 0x18});  // mov ecx, [esp+18]
    emit(p, {0xD9, 0x41, 0x20});        // fld dword [ecx+20]
    emit(p, {0xD8, 0x0D});              // fmul dword [g_scale]
    emit32(p, scale);
    emit(p, {0xE9});
    emit_rel(p, kLoadNext);

    // The divide path reloads the threshold after popping the weight. ecx is still the skeleton.
    uint8_t* reload_stub = p;
    emit(p, {0xDD, 0xD8});      // fstp st(0)
    emit(p, {0xD9, 0x41, 0x20});  // fld dword [ecx+20]
    emit(p, {0xD8, 0x0D});
    emit32(p, scale);
    emit(p, {0xE9});
    emit_rel(p, kReloadNext);
    FlushInstructionCache(GetCurrentProcess(), cave, static_cast<size_t>(p - cave));

    uint8_t load_jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    const int32_t load_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(load_stub) - (kLoad + 5));
    std::memcpy(load_jump + 1, &load_rel, 4);
    uint8_t reload_jump[5] = {0xE9, 0, 0, 0, 0};
    const int32_t reload_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(reload_stub) - (kReload + 5));
    std::memcpy(reload_jump + 1, &reload_rel, 4);
    if (!write_bytes(reinterpret_cast<void*>(kLoad), load_jump, sizeof(load_jump)) ||
        !write_bytes(reinterpret_cast<void*>(kReload), reload_jump, sizeof(reload_jump))) {
        LOG_ERROR("Could not patch slide steering (Win32=%lu)", GetLastError());
        return false;
    }
    g_installed = true;
    LOG_INFO("Slide steering threshold follows the frame time (0x%08X and 0x%08X, cave %p)", kLoad, kReload, cave);
    return true;
}

void slidesteer_update(double dt) {
    if (!g_installed) {
        return;
    }
    double n = dt * 30.0;
    if (!(n > 0.0)) {
        n = 1.0;
    }
    if (n > 1.0) {
        n = 1.0;
    }
    g_scale = static_cast<float>(n * n);
}
