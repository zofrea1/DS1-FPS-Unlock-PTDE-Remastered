#include "script.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

// The game's Lua is 5.0 with FromSoft's changes: luaC_checkGC needs a non-zero GCthreshold, lua_open leaves it
// at 0 and a collection never raises it, so the collector never starts on its own. The game collects each
// state in full (fn 0x603E60, DLLua -> luaC_collectgarbage) when a countdown kept beside it runs out:
//   AiLuaMan ([[0x137DC74] + 0xF90]): [+0xC] -= step; at 0 it is reset to 0.5 s ([0x12DECCC])
//   the other state's manager ([[0x137D790] + 4]): [+0x58] -= step; at 0 it is reset to 1 s ([0x12DED00])
// The step is the engine's 1/30 that the frame hook rewrites with the frame time. The countdown is compiled
// into several places (the in-game step 0xEFC750, a second step 0xEFD2F0, standalone copies); each one
// converts the timer and the step to double and runs
//   subsd xmm0, xmm1 ; xorps xmm1, xmm1
// (timer - step, then xmm1 = 0 for the comparison). Those 7 bytes become a call to a stub that subtracts
// max(step, 1/30) instead.
constexpr uint32_t kCountdownSites[] = {
    0x00EFC8DA,  // AI, in-game step
    0x00EFC888,  // other, in-game step
    0x00EFE86E,  // AI, second step
    0x00EFE821,  // other, second step
    0x00CF70BC,  // AI, standalone (with a "now" flag)
    0x00D5D9DC,  // other, standalone (with a "now" flag)
    0x00CA9156,  // AI, standalone copies with no caller found
    0x00CA9206,
};
constexpr uint8_t kCountdownCode[7] = {0xF2, 0x0F, 0x5C, 0xC1, 0x0F, 0x57, 0xC9};

// luaD_throw (fn 0x698070, cdecl L, errcode): with no error handler ([L + 0x58] = 0) it calls the panic
// function [G + 0x28] and then exit(1). The first 6 bytes of that path (mov ecx,[eax+10h] ; mov edx,[ecx+28h])
// go to a stub that logs the error, then runs them.
constexpr uint32_t kPanicPath = 0x00698090;
constexpr uint32_t kPanicResume = 0x00698096;
constexpr uint8_t kPanicCode[6] = {0x8B, 0x48, 0x10, 0x8B, 0x51, 0x28};

// Pointer chains to each state's lua_State: root global, manager, DLLua, L ([DLLua + 0x18]).
// global_State = [L + 0x10], its nblocks (bytes allocated) = [G + 0x24].
constexpr uint32_t kAiRoot = 0x0137DC74;
constexpr uint32_t kOtherRoot = 0x0137D790;

alignas(8) const double g_min_step = 1.0 / 30.0;

struct ScriptMemory {
    std::atomic<uint32_t> low{UINT32_MAX};
    std::atomic<uint32_t> high{0};
};
ScriptMemory g_ai_memory;
ScriptMemory g_other_memory;

bool write_code(uint32_t address, const uint8_t* bytes, size_t size) {
    auto* p = reinterpret_cast<uint8_t*>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

bool write_branch(uint8_t opcode, uint32_t site, size_t size, const void* target) {
    uint8_t bytes[16];
    if (size < 5 || size > sizeof(bytes)) return false;
    bytes[0] = opcode;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(bytes + 1, &rel, 4);
    for (size_t i = 5; i < size; ++i) bytes[i] = 0x90;
    return write_code(site, bytes, size);
}

const uint8_t* script_state(uint32_t root, uint32_t manager_offset, uint32_t lua_offset) {
    __try {
        const auto* base = *reinterpret_cast<const uint8_t* const*>(root);
        if (!base) return nullptr;
        const auto* manager = *reinterpret_cast<const uint8_t* const*>(base + manager_offset);
        if (!manager) return nullptr;
        const auto* lua = *reinterpret_cast<const uint8_t* const*>(manager + lua_offset);
        return lua ? *reinterpret_cast<const uint8_t* const*>(lua + 0x18) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const uint8_t* ai_state() {
    return script_state(kAiRoot, 0xF90, 8);
}

const uint8_t* other_state() {
    return script_state(kOtherRoot, 4, 4);
}

bool script_bytes(const uint8_t* state, uint32_t* bytes) {
    if (!state) return false;
    __try {
        const auto* global = *reinterpret_cast<const uint8_t* const*>(state + 0x10);
        if (!global) return false;
        *bytes = *reinterpret_cast<const uint32_t*>(global + 0x24);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void sample(ScriptMemory& m, const uint8_t* state) {
    uint32_t bytes = 0;
    if (!script_bytes(state, &bytes)) return;
    if (bytes < m.low.load(std::memory_order_relaxed)) m.low.store(bytes, std::memory_order_relaxed);
    if (bytes > m.high.load(std::memory_order_relaxed)) m.high.store(bytes, std::memory_order_relaxed);
}

void take(ScriptMemory& m, char* out, size_t size) {
    const uint32_t low = m.low.exchange(UINT32_MAX, std::memory_order_relaxed);
    const uint32_t high = m.high.exchange(0, std::memory_order_relaxed);
    if (low == UINT32_MAX) {
        std::snprintf(out, size, "-");
        return;
    }
    std::snprintf(out, size, "%.2f-%.2f", low / 1048576.0, high / 1048576.0);
}

void __cdecl log_panic(const uint8_t* state, int code) {
    static const char* const kCodes[] = {"no error", "runtime error", "file error", "syntax error", "out of memory",
                                         "error in the error handler"};
    const char* kind = code >= 0 && code <= 5 ? kCodes[code] : "unknown error";
    const char* which = state == ai_state() ? "the AI scripts"
                                            : (state == other_state() ? "the other script state" : "a script state");
    uint32_t bytes = 0;
    char in_use[48] = "unknown";
    if (script_bytes(state, &bytes)) {
        std::snprintf(in_use, sizeof(in_use), "%.2f MB", bytes / 1048576.0);
    }
    char message[160] = {};
    if (code == 1) {
        // A runtime error leaves its message on top of the stack: TObject {int tt; Value at +8} (16 bytes) at
        // L->top ([L + 8]) - 16; a string (tt 4) is a TString with the length at +0xC and the text at +0x10.
        __try {
            const auto* top = *reinterpret_cast<const uint8_t* const*>(state + 8);
            if (*reinterpret_cast<const int*>(top - 0x10) == 4) {
                const auto* text = *reinterpret_cast<const uint8_t* const*>(top - 0x08);
                const uint32_t length = *reinterpret_cast<const uint32_t*>(text + 0x0C);
                const uint32_t n = length < sizeof(message) - 1 ? length : sizeof(message) - 1;
                std::memcpy(message, text + 0x10, n);
                for (uint32_t i = 0; i < n; ++i) {
                    if (message[i] < 0x20) message[i] = ' ';
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            message[0] = 0;
        }
    }
    LOG_ERROR("SCRIPT: %s hit an error that nothing catches (%s, Lua error %d); the game closes itself now "
              "(script memory in use: %s)%s%s",
              which, kind, code, in_use, message[0] ? ": " : "", message);
}

}  // namespace

bool script_install(bool fix_gc) {
    bool countdowns_match = true;
    for (uint32_t site : kCountdownSites) {
        if (std::memcmp(reinterpret_cast<const void*>(site), kCountdownCode, sizeof(kCountdownCode)) != 0) {
            LOG_ERROR("Script collection countdown at %08X does not match this build", site);
            countdowns_match = false;
        }
    }
    const bool panic_matches = std::memcmp(reinterpret_cast<const void*>(kPanicPath), kPanicCode, sizeof(kPanicCode)) == 0;
    if (!panic_matches) {
        LOG_ERROR("Script error path at %08X does not match this build", kPanicPath);
    }
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) return false;
    uint8_t* p = cave;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    };
    auto emit32 = [&](uint32_t v) {
        std::memcpy(p, &v, 4);
        p += 4;
    };
    // Countdown: xmm0 = timer, xmm1 = step (both double). The comiss after the call sets its own flags.
    uint8_t* countdown = p;
    emit({0xF2, 0x0F, 0x5F, 0x0D});  // maxsd xmm1, [g_min_step]
    emit32(reinterpret_cast<uint32_t>(&g_min_step));
    emit({0xF2, 0x0F, 0x5C, 0xC1});  // subsd xmm0, xmm1
    emit({0x0F, 0x57, 0xC9});        // xorps xmm1, xmm1
    emit({0xC3});                    // ret
    // Uncaught error: esp is as on entry to luaD_throw ([esp+4] = L, [esp+8] = code) and eax = L.
    uint8_t* panic = p;
    emit({0x50});                    // push eax
    emit({0xFF, 0x74, 0x24, 0x0C});  // push dword ptr [esp+0Ch]   (code)
    emit({0x50});                    // push eax                   (L)
    emit({0xB9});                    // mov ecx, log_panic
    emit32(reinterpret_cast<uint32_t>(&log_panic));
    emit({0xFF, 0xD1});              // call ecx
    emit({0x83, 0xC4, 0x08});        // add esp, 8
    emit({0x58});                    // pop eax
    for (uint8_t b : kPanicCode) emit({b});  // mov ecx, [eax+10h] ; mov edx, [ecx+28h]
    emit({0xE9});                    // jmp back (push eax ; call edx = G->panic(L), then exit(1))
    emit32(kPanicResume - (reinterpret_cast<uint32_t>(p) + 4));
    FlushInstructionCache(GetCurrentProcess(), cave, 256);

    bool ok = true;
    if (fix_gc) {
        if (!countdowns_match) {
            ok = false;
        } else {
            for (uint32_t site : kCountdownSites) {
                if (!write_branch(0xE8, site, sizeof(kCountdownCode), countdown)) {
                    LOG_ERROR("Could not patch the script collection countdown at %08X", site);
                    ok = false;
                }
            }
            if (ok) {
                LOG_INFO("Script garbage collection runs at least every 15 / 30 frames, as at 30 FPS (AI scripts 0.5 s, "
                         "other scripts 1 s; %u sites)",
                         static_cast<unsigned>(sizeof(kCountdownSites) / sizeof(kCountdownSites[0])));
            }
        }
    }
    if (panic_matches) {
        if (write_branch(0xE9, kPanicPath, sizeof(kPanicCode), panic)) {
            LOG_INFO("Script errors that close the game are logged (SCRIPT: lines)");
        } else {
            LOG_ERROR("Could not patch the script error path");
            ok = false;
        }
    }
    return ok && panic_matches;
}

void script_sample() {
    sample(g_ai_memory, ai_state());
    sample(g_other_memory, other_state());
}

void script_describe(char* out, unsigned size) {
    char ai[48];
    char other[48];
    take(g_ai_memory, ai, sizeof(ai));
    take(g_other_memory, other, sizeof(other));
    std::snprintf(out, size, "script memory MB (low-high) AI %s, other %s", ai, other);
}
