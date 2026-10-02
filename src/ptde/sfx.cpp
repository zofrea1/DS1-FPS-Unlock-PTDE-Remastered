#include "sfx.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kRegisterEffect = 0x00D19CF0;
constexpr float kMinInterval = 1.0f / 30.0f;

using RegisterFn = void*(__stdcall*)(const void* name, uint8_t* data, uint32_t size);
RegisterFn g_orig = nullptr;
std::atomic<int> g_effects{0};
std::atomic<int> g_raised{0};

uint32_t rd32(const uint8_t* d, uint32_t o) {
    uint32_t v;
    std::memcpy(&v, d + o, 4);
    return v;
}

// PTDE effect file: 'FXR\0', version, data start, pointer table offset (+0x0C), pointer count (+0x10).
// The table lists the file offset of every pointer field. A pond3 pointer is the last field of an AST
// (24 bytes: pond1 ptr, data3 count, the count again, three 0/1 flag bytes and a 0, pond2 ptr, pond3 ptr),
// so a pointer field whose preceding 20 bytes have that shape leads to a pond3 struct, whose first int is
// its type (0-7). Checked against a full parse of all 3,849 PTDE effects: it finds exactly their type 2
// and type 3 structs.
int raise_intervals(uint8_t* d, uint32_t size) {
    if (!d || size < 0x20 || std::memcmp(d, "FXR\0", 4) != 0) return 0;
    const uint32_t table = rd32(d, 0x0C);
    const uint32_t count = rd32(d, 0x10);
    if (table > size || count > (size - table) / 4) return 0;
    std::vector<uint32_t> fields(count);
    for (uint32_t i = 0; i < count; ++i) fields[i] = rd32(d, table + i * 4);
    std::sort(fields.begin(), fields.end());
    auto listed = [&](uint32_t o) { return std::binary_search(fields.begin(), fields.end(), o); };

    int raised = 0;
    for (uint32_t o : fields) {
        if (o < 0x14 || o > size - 4) continue;
        const uint32_t ast = o - 0x14;
        const uint32_t pond1 = rd32(d, ast), n1 = rd32(d, ast + 4), n2 = rd32(d, ast + 8);
        const uint8_t* flags = d + ast + 12;
        const uint32_t pond2 = rd32(d, ast + 16);
        if (n1 != n2 || n1 >= 4096 || flags[3] != 0 || flags[0] > 1 || flags[1] > 1 || flags[2] > 1) continue;
        if ((pond1 && !listed(ast)) || (pond2 && !listed(ast + 16))) continue;
        const uint32_t target = rd32(d, o);
        if (target > size - 0x18) continue;
        const uint32_t type = rd32(d, target);
        uint32_t at = 0;
        if (type == 2) {
            at = target + 0x10;
        } else if (type == 3) {
            at = target + 0x0C;
        } else {
            continue;
        }
        float v;
        std::memcpy(&v, d + at, 4);
        if (v >= 0.0f && v < kMinInterval - 1e-6f) {
            std::memcpy(d + at, &kMinInterval, 4);
            ++raised;
        }
    }
    return raised;
}

void* __stdcall hk_register(const void* name, uint8_t* data, uint32_t size) {
    int raised = 0;
    __try {
        raised = raise_intervals(data, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        raised = 0;
    }
    const int effects = ++g_effects;
    const int total = (g_raised += raised);
    // The first effect shows the hook was in place before the common effects loaded; then a running total.
    if (effects == 1 || effects % 500 == 0) {
        LOG_INFO("Effect spawn intervals: %d raised to 1/30 s in the %d effects loaded so far", total, effects);
    }
    return g_orig(name, data, size);
}

}  // namespace

bool sfx_install() {
    // mov eax,[0x013787C8] ; push esi ; push edi
    static const uint8_t kExpect[7] = {0xA1, 0xC8, 0x87, 0x37, 0x01, 0x56, 0x57};
    auto* site = reinterpret_cast<uint8_t*>(kRegisterEffect);
    if (std::memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        LOG_ERROR("Effect registration at %08X does not match this build. Not patching.", kRegisterEffect);
        return false;
    }
    constexpr size_t kStolen = 5;  // the mov uses an absolute address, so it can move as is
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, site, kStolen);
    tramp[kStolen] = 0xE9;
    const int32_t back = static_cast<int32_t>((kRegisterEffect + kStolen) - reinterpret_cast<uintptr_t>(tramp + kStolen + 5));
    std::memcpy(tramp + kStolen + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 32);
    g_orig = reinterpret_cast<RegisterFn>(tramp);

    DWORD old = 0;
    if (!VirtualProtect(site, kStolen, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_register) - (kRegisterEffect + 5));
    std::memcpy(site + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), site, kStolen);
    DWORD ignored = 0;
    VirtualProtect(site, kStolen, old, &ignored);
    LOG_INFO("Effect spawn intervals below 1/30 s are raised to 1/30 s as effects load (Remastered's 60 FPS rule)");
    return true;
}
