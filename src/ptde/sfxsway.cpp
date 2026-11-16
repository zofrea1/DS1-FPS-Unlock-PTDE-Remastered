#include "sfxsway.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

#if FPSU_DIAGNOSTICS

namespace {

enum Kind : int {
    kSwayCall = 0,     // 0x9E6050: Yurayura's per-call function (counter + 1)
    kSwayUpdate,       // 0x9E6180: Yurayura's update
    kFollowCall,       // 0x9E50C0: PartialFollow's per-call function (counter + 1)
    kFollowUpdate,     // 0x9E5350: PartialFollow's update
    kTurnSway,         // 0xA50100 from Yurayura's update
    kTurnFollow,       // 0xA50100 from PartialFollow's update
    kTurnOther,        // 0xA50100 from anywhere else
    kKinds
};

volatile LONG g_counts[kKinds];
volatile LONG g_top[2];  // the highest counter (+0x10) an update saw: Yurayura, PartialFollow

void top(int i, uint32_t self) {
    LONG v = 0;
    __try {
        v = *reinterpret_cast<const LONG*>(self + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    LONG cur = g_top[i];
    while (v > cur) {
        const LONG seen = InterlockedCompareExchange(&g_top[i], v, cur);
        if (seen == cur) break;
        cur = seen;
    }
}

void __cdecl note(int kind, uint32_t self, uint32_t ret) {
    if (kind == kSwayUpdate) top(0, self);
    if (kind == kFollowUpdate) top(1, self);
    if (kind == kTurnSway) {  // the re-randomizer: sorted by the update it returns to
        kind = ret >= 0x009E6180 && ret < 0x009E6360 ? kTurnSway
             : ret >= 0x009E5350 && ret < 0x009E5880 ? kTurnFollow
                                                     : kTurnOther;
    }
    InterlockedIncrement(&g_counts[kind]);
}

void* g_tramp[5] = {};

// Each stub: the registers kept, note(kind, ecx, the return address), then the moved instructions and on.
#define SWAY_STUB(name, kind, slot)                    \
    __declspec(naked) void name() {                    \
        __asm { pushad }                               \
        __asm { push dword ptr [esp + 32] }            \
        __asm { push ecx }                             \
        __asm { push kind }                            \
        __asm { call note }                            \
        __asm { add esp, 12 }                          \
        __asm { popad }                                \
        __asm { jmp dword ptr [g_tramp + slot * 4] }   \
    }

// The kind as a number (inline assembly takes no enum names): kSwayCall 0, kSwayUpdate 1, kFollowCall 2,
// kFollowUpdate 3, kTurnSway 4.
SWAY_STUB(stub_sway_call, 0, 0)
SWAY_STUB(stub_sway_update, 1, 1)
SWAY_STUB(stub_follow_call, 2, 2)
SWAY_STUB(stub_follow_update, 3, 3)
SWAY_STUB(stub_turn, 4, 4)

struct Site {
    uint32_t at;
    uint8_t bytes[6];
    int len;  // whole instructions moved to the trampoline
    void (*stub)();
};

// 0x9E6050: push ebp ; mov ebp,esp ; push -1. The others: push ebx ; mov ebx,esp ; sub esp,8 (the aligned frame).
const Site kSites[5] = {
    {0x009E6050, {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x00}, 5, &stub_sway_call},
    {0x009E6180, {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08}, 6, &stub_sway_update},
    {0x009E50C0, {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08}, 6, &stub_follow_call},
    {0x009E5350, {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08}, 6, &stub_follow_update},
    {0x00A50100, {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08}, 6, &stub_turn},
};

// Particles emitted: the six emitter shapes' per-call functions (Cone 0x9ED540, Square 0x9EC8A0, Circle 0x9EBA60,
// Sphere 0x9EA7A0, Box 0x9E92A0, EqualDistance 0x9E7A80) take the number of new particles as their third stack
// argument: their sum is every particle the effects emit.
volatile LONG g_emit_calls = 0;
volatile LONG g_emit_particles = 0;
void* g_emit_tramp[6] = {};

void __stdcall emit_note(uint32_t count) {
    InterlockedIncrement(&g_emit_calls);
    if (count < 100000) InterlockedExchangeAdd(&g_emit_particles, static_cast<LONG>(count));
}

#define EMIT_STUB(name, slot)                              \
    __declspec(naked) void name() {                        \
        __asm { pushad }                                   \
        __asm { push dword ptr [esp + 32 + 12] }           \
        __asm { call emit_note }                           \
        __asm { popad }                                    \
        __asm { jmp dword ptr [g_emit_tramp + slot * 4] }  \
    }
EMIT_STUB(emit_stub_0, 0)
EMIT_STUB(emit_stub_1, 1)
EMIT_STUB(emit_stub_2, 2)
EMIT_STUB(emit_stub_3, 3)
EMIT_STUB(emit_stub_4, 4)
EMIT_STUB(emit_stub_5, 5)
const uint32_t kEmitSites[6] = {0x009ED540, 0x009EC8A0, 0x009EBA60, 0x009EA7A0, 0x009E92A0, 0x009E7A80};
void (*const kEmitStubs[6])() = {&emit_stub_0, &emit_stub_1, &emit_stub_2, &emit_stub_3, &emit_stub_4, &emit_stub_5};

void install_emit_watch() {
    static const uint8_t kHead[6] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};  // push ebx ; mov ebx,esp ; sub esp,8
    for (uint32_t a : kEmitSites) {
        if (std::memcmp(reinterpret_cast<const void*>(a), kHead, 6) != 0) {
            LOG_ERROR("SfxSway: emitter per-call at %08X does not match. Not counting particles.", a);
            return;
        }
    }
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 6 * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) return;
    for (int i = 0; i < 6; ++i) {
        uint8_t* t = mem + i * 16;
        std::memcpy(t, kHead, 6);
        t[6] = 0xE9;
        const int32_t back = static_cast<int32_t>((kEmitSites[i] + 6) - reinterpret_cast<uintptr_t>(t + 11));
        std::memcpy(t + 7, &back, 4);
        g_emit_tramp[i] = t;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, 6 * 16);
    for (int i = 0; i < 6; ++i) {
        auto* p = reinterpret_cast<uint8_t*>(kEmitSites[i]);
        DWORD old = 0;
        if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return;
        p[0] = 0xE9;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(kEmitStubs[i]) - (kEmitSites[i] + 5));
        std::memcpy(p + 1, &rel, 4);
        p[5] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), p, 6);
        DWORD ignored = 0;
        VirtualProtect(p, 6, old, &ignored);
    }
}

bool g_on = false;
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_since{};
LONG g_frames = 0;
int g_lines = 0;
constexpr int kMaxLines = 1800;

}  // namespace

bool sfxsway_install() {
    // A site FixSfxSway hooked already starts with its jmp: the watch goes in front of it and then on to it.
    bool chained[5] = {};
    for (int i = 0; i < 5; ++i) {
        const Site& s = kSites[i];
        chained[i] = *reinterpret_cast<const uint8_t*>(s.at) == 0xE9;
        if (!chained[i] && std::memcmp(reinterpret_cast<const void*>(s.at), s.bytes, s.len) != 0) {
            LOG_ERROR("SfxSway: the code at %08X does not match this build. Not watching.", s.at);
            return false;
        }
    }
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 5 * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) return false;
    for (int i = 0; i < 5; ++i) {
        const Site& s = kSites[i];
        uint8_t* t = mem + i * 16;
        if (chained[i]) {
            int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(s.at + 1), 4);
            const uintptr_t target = s.at + 5 + rel;
            t[0] = 0xE9;
            const int32_t to = static_cast<int32_t>(target - reinterpret_cast<uintptr_t>(t + 5));
            std::memcpy(t + 1, &to, 4);
        } else {
            std::memcpy(t, s.bytes, s.len);
            t[s.len] = 0xE9;
            const int32_t back = static_cast<int32_t>((s.at + s.len) - reinterpret_cast<uintptr_t>(t + s.len + 5));
            std::memcpy(t + s.len + 1, &back, 4);
        }
        g_tramp[i] = t;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, 5 * 16);
    for (int i = 0; i < 5; ++i) {
        const Site& s = kSites[i];
        const int len = chained[i] ? 5 : s.len;
        auto* p = reinterpret_cast<uint8_t*>(s.at);
        DWORD old = 0;
        if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return false;
        p[0] = 0xE9;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(s.stub) - (s.at + 5));
        std::memcpy(p + 1, &rel, 4);
        for (int k = 5; k < len; ++k) p[k] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), p, len);
        DWORD ignored = 0;
        VirtualProtect(p, len, old, &ignored);
    }
    install_emit_watch();
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_since);
    g_on = true;
    LOG_INFO("SfxSway: the effect sway movements (Yurayura, PartialFollow) are counted once a second while they run");
    return true;
}

void sfxsway_tick() {
    if (!g_on) return;
    ++g_frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double secs = static_cast<double>(now.QuadPart - g_since.QuadPart) / static_cast<double>(g_freq.QuadPart);
    if (secs < 1.0) return;
    LONG c[kKinds];
    for (int i = 0; i < kKinds; ++i) c[i] = InterlockedExchange(&g_counts[i], 0);
    const LONG top_sway = InterlockedExchange(&g_top[0], 0), top_follow = InterlockedExchange(&g_top[1], 0);
    const LONG frames = g_frames;
    g_frames = 0;
    g_since = now;
    const LONG ecalls = InterlockedExchange(&g_emit_calls, 0), eparts = InterlockedExchange(&g_emit_particles, 0);
    if (ecalls && g_lines < kMaxLines) {
        LOG_INFO("SfxEmit: %.2f s, %ld frames (%.0f FPS): %ld emitter calls, %ld particles emitted", secs, frames,
                 frames / secs, ecalls, eparts);
    }
    if ((c[kSwayUpdate] == 0 && c[kFollowUpdate] == 0) || g_lines >= kMaxLines) return;
    ++g_lines;
    LOG_INFO("SfxSway: %.2f s, %ld frames (%.0f FPS): sway per-call %ld, updates %ld, turns %ld (top counter %ld); "
             "follow per-call %ld, updates %ld, turns %ld (top counter %ld); other turns %ld",
             secs, frames, frames / secs, c[kSwayCall], c[kSwayUpdate], c[kTurnSway], top_sway, c[kFollowCall],
             c[kFollowUpdate], c[kTurnFollow], top_follow, c[kTurnOther]);
}

#else  // A release build leaves this diagnostic out (see fpsu_buildcfg.h).

bool sfxsway_install() { return false; }
void sfxsway_tick() {}

#endif  // FPSU_DIAGNOSTICS
