#include "footik.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

constexpr uint32_t kPelvisGainSite = 0x00FE1991;   // mulsd xmm0, qword ptr [0x11E82D0]  (0.2)
constexpr uint32_t kPelvisDecaySite = 0x00FE193C;  // movss xmm0, dword ptr [esi+0x2EC]   (0.6)
constexpr uint32_t kPelvisDecayNext = 0x00FE1944;
constexpr uint32_t kSolveCall = 0x00ED4BF4;        // call 0x7B1A50 (input, output, pose; ecx = solver)
constexpr uint32_t kSolve = 0x007B1A50;
constexpr uint32_t kOnOffGain = 0x3E4CCCCD;        // 0.2f, the solver input's onOffGain [+0x68]

volatile long g_enabled = 1;
bool g_installed = false;
float g_n = 1.0f;                       // frame time * 30
alignas(8) double g_pelvis_gain = 0.2;  // 0.2 over this frame's time
uint32_t g_onoff_bits = kOnOffGain;     // the solver's 0.2 over this frame's time, as float bits

// stdcall(raw bits of v) -> eax = bits of v ^ n for 0 < v < 1, v unchanged otherwise or while the fix is off.
// Preserves every other register and the flags.
__declspec(naked) void pow_thunk() {
    __asm {
        mov eax, [esp + 4]
        pushfd
        cmp dword ptr [g_enabled], 0
        je done
        test eax, eax
        jle done                  // v <= 0: unchanged
        cmp eax, 0x3F800000
        jge done                  // v >= 1: unchanged
        sub esp, 4
        fld dword ptr [g_n]       // n
        fld dword ptr [esp + 12]  // v, n
        fyl2x                     // y = n * log2(v)
        fld st(0)                 // y, y
        frndint                   // i, y
        fsub st(1), st            // i, f = y - i
        fxch st(1)                // f, i
        f2xm1                     // 2^f - 1, i
        fld1
        faddp st(1), st           // 2^f, i
        fscale                    // 2^y, i
        fstp st(1)                // v ^ n
        fstp dword ptr [esp]
        mov eax, [esp]
        add esp, 4
    done:
        popfd
        ret 4
    }
}

// The pelvis gain and the solver's on/off gain for n = frame time * 30, or the game's 0.2 while the fix is off.
void write_gains(double n) {
    const double gain = g_enabled ? 1.0 - std::pow(0.8, n) : 0.2;
    g_pelvis_gain = gain;
    const float bits_source = static_cast<float>(gain);
    std::memcpy(&g_onoff_bits, &bits_source, 4);
}

bool write_bytes(uint32_t va, const void* bytes, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(va), size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(va), bytes, size);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(va), size);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(va), size, old, &ignored);
    return true;
}

// ---------------------------------------------------------------- other characters
constexpr uint32_t kOthersSite = 0x00FE1798;  // mov edx, dword ptr [eax+0A4h]  (the "is this our player" slot)
constexpr uint32_t kWorldChrManVar = 0x0137D644;
constexpr uint32_t kPlayerCtrlPlace = 0x0118CEC8;  // PlayerCtrl vtable slot 0x6C: 0xFE1740
constexpr uint32_t kPlace = 0x00FE1740;
volatile long g_npcs = 0;     // FootIkNpcs
volatile long g_players = 0;  // FootIkPlayers (Prepare to Co-op)
bool g_others_installed = false;
volatile long g_last_others = 0;  // the decision of the last call (for the profile)

// Another player's character: WorldChrMan+0x24 {count, entries of 0x20, character first}, entry 0 being ours.
bool other_player(uint32_t wcm, uint32_t chr) {
    const uint32_t n = *reinterpret_cast<const uint32_t*>(wcm + 0x24);
    const uint32_t entries = *reinterpret_cast<const uint32_t*>(wcm + 0x28);
    if (!entries || n > 256) return false;
    for (uint32_t i = 1; i < n; ++i) {
        if (*reinterpret_cast<const uint32_t*>(entries + i * 0x20) == chr) return true;
    }
    return false;
}

uint32_t ctrl_at(uint32_t ctrl, uint32_t off) { return *reinterpret_cast<const uint32_t*>(ctrl + off); }

// cdecl(chr, ctrl), asked only when the game said no: place this character's feet too.
int __cdecl others_wanted(uint32_t chr, uint32_t ctrl) {
    const long npcs = g_npcs, players = g_players;
    int yes = 0;
    if (npcs || players) {
        __try {
            // What 0xFE1740 reads past the question with no check (+0x1C, +0x2C), and the legs' solvers.
            const uint32_t wcm = *reinterpret_cast<const uint32_t*>(kWorldChrManVar);
            if (chr && wcm && ctrl_at(ctrl, 0x1C) && ctrl_at(ctrl, 0x2C) &&
                (ctrl_at(ctrl, 0x2BC) || ctrl_at(ctrl, 0x2C0))) {
                yes = other_player(wcm, chr) ? (players ? 1 : 0) : (npcs ? 1 : 0);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            yes = 0;
        }
    }
    g_last_others = yes;
    return yes;
}

// In place of the slot's address: ecx = the character, esi = its controller (0xFE1740's this). Returns al.
__declspec(naked) void others_check() {
    __asm {
        push ecx
        mov eax, [ecx]
        call dword ptr [eax + 0xA4]  // the game's own answer: our player
        pop ecx
        test al, al
        jnz done
        push esi
        push ecx
        call others_wanted
        add esp, 8
    done:
        ret
    }
}

// ---------------------------------------------------------------- profile (diagnostics)
using PlaceFn = float(__thiscall*)(void* ctrl, uint32_t dt_bits, uint32_t arg, uint32_t arg2);  // ret 0xC
PlaceFn g_place_orig = reinterpret_cast<PlaceFn>(kPlace);
bool g_prof_on = false;
LARGE_INTEGER g_prof_freq{};
struct ProfBucket {
    long long calls = 0;
    long long ticks = 0;
};
ProfBucket g_prof[3];  // 0: ours, 1: others placed, 2: others left (the question said no)
long long g_prof_frames = 0;

// Both arguments pass through as raw dwords: the second is a pointer, and one handed on as a float would go through
// the x87 stack, which changes a NaN pattern's bits (the first try crashed on a pointer so mangled).
float __fastcall place_timed(void* ctrl, void*, uint32_t dt_bits, uint32_t arg, uint32_t arg2) {
    LARGE_INTEGER t0, t1;
    g_last_others = 0;
    QueryPerformanceCounter(&t0);
    const float r = g_place_orig(ctrl, dt_bits, arg, arg2);
    QueryPerformanceCounter(&t1);
    const uint32_t chr = ctrl_at(reinterpret_cast<uint32_t>(ctrl), 0x10);
    const uint32_t wcm = *reinterpret_cast<const uint32_t*>(kWorldChrManVar);
    const bool ours = wcm && *reinterpret_cast<const uint32_t*>(wcm + 0x3C) == chr;
    ProfBucket& b = g_prof[ours ? 0 : g_last_others ? 1 : 2];
    ++b.calls;
    b.ticks += t1.QuadPart - t0.QuadPart;
    if (ours) ++g_prof_frames;
    return r;
}

}  // namespace

bool footik_install() {
    static const uint8_t kGainExpect[8] = {0xF2, 0x0F, 0x59, 0x05, 0xD0, 0x82, 0x1E, 0x01};
    static const uint8_t kDecayExpect[8] = {0xF3, 0x0F, 0x10, 0x86, 0xEC, 0x02, 0x00, 0x00};
    auto* call = reinterpret_cast<const uint8_t*>(kSolveCall);
    int32_t call_rel = 0;
    std::memcpy(&call_rel, call + 1, 4);
    if (std::memcmp(reinterpret_cast<const void*>(kPelvisGainSite), kGainExpect, 8) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kPelvisDecaySite), kDecayExpect, 8) != 0 || call[0] != 0xE8 ||
        static_cast<uint32_t>(kSolveCall + 5 + call_rel) != kSolve) {
        LOG_ERROR("Foot IK sites do not match this build. Not patching.");
        return false;
    }
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) {
        LOG_ERROR("Could not allocate the foot IK cave (Win32=%lu)", GetLastError());
        return false;
    }
    uint8_t* p = cave;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    };
    auto emit32 = [&](uint32_t v) {
        std::memcpy(p, &v, 4);
        p += 4;
    };
    auto emit_rel = [&](uint32_t target) { emit32(target - (reinterpret_cast<uint32_t>(p) + 4)); };

    // Pelvis decay: xmm0 = [esi+0x2EC] ^ n.
    uint8_t* decay_stub = p;
    emit({0x50});                                  // push eax
    emit({0xFF, 0xB6, 0xEC, 0x02, 0x00, 0x00});    // push dword ptr [esi+0x2EC]
    emit({0xE8});                                  // call pow_thunk
    emit_rel(reinterpret_cast<uint32_t>(&pow_thunk));
    emit({0x66, 0x0F, 0x6E, 0xC0});                // movd xmm0, eax
    emit({0x58});                                  // pop eax
    emit({0xE9});                                  // jmp back
    emit_rel(kPelvisDecayNext);

    // Solver call: the input's onOffGain, when it is the default 0.2, becomes 0.2 over this frame's time.
    // The input is a local the caller rebuilds every frame, so nothing has to be put back.
    uint8_t* solve_stub = p;
    emit({0x50});                                  // push eax
    emit({0x52});                                  // push edx
    emit({0x8B, 0x54, 0x24, 0x0C});                // mov edx, [esp+0xC]          (the input: edx, eax, return)
    emit({0x81, 0x7A, 0x68});                      // cmp dword ptr [edx+0x68], 0.2f
    emit32(kOnOffGain);
    emit({0x75, 0x08});                            // jne skip
    emit({0xA1});                                  // mov eax, [g_onoff_bits]
    emit32(reinterpret_cast<uint32_t>(&g_onoff_bits));
    emit({0x89, 0x42, 0x68});                      // mov [edx+0x68], eax
    emit({0x5A});                                  // skip: pop edx
    emit({0x58});                                  // pop eax
    emit({0xE9});                                  // jmp doFootPlacement
    emit_rel(kSolve);
    FlushInstructionCache(GetCurrentProcess(), cave, 0x100);

    const uint32_t gain = reinterpret_cast<uint32_t>(&g_pelvis_gain);
    uint8_t decay_jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t decay_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(decay_stub) - (kPelvisDecaySite + 5));
    std::memcpy(decay_jump + 1, &decay_rel, 4);
    const int32_t solve_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(solve_stub) - (kSolveCall + 5));
    if (!write_bytes(kPelvisGainSite + 4, &gain, 4) || !write_bytes(kPelvisDecaySite, decay_jump, 8) ||
        !write_bytes(kSolveCall + 1, &solve_rel, 4)) {
        LOG_ERROR("Could not patch the foot IK (Win32=%lu)", GetLastError());
        return false;
    }
    g_installed = true;
    LOG_INFO("Foot IK follows the frame time (pelvis 0.2 and 0.6, solver on/off 0.2, per 1/30 s; cave %p)", cave);
    return true;
}

void footik_update(double dt) {
    double n = dt * 30.0;
    if (!(n > 0.01)) n = 0.01;
    if (n > 8.0) n = 8.0;
    g_n = static_cast<float>(n);
    write_gains(n);
}

// The decay thunk reads g_enabled as it runs, so the gains change with it, not at the next frame's update.
void footik_set_enabled(bool on) {
    g_enabled = on ? 1 : 0;
    write_gains(g_n);
}

bool footik_enabled() {
    return g_installed && g_enabled != 0;
}

bool footik_others_install() {
    if (g_others_installed) return true;
    static const uint8_t kExpect[6] = {0x8B, 0x90, 0xA4, 0x00, 0x00, 0x00};
    if (std::memcmp(reinterpret_cast<const void*>(kOthersSite), kExpect, sizeof(kExpect)) != 0 ||
        *reinterpret_cast<const uint32_t*>(kPlayerCtrlPlace) != kPlace) {
        LOG_ERROR("Foot IK for other characters: the code is not this build's; only your character's feet are placed");
        return false;
    }
    uint8_t code[6] = {0xBA, 0, 0, 0, 0, 0x90};  // mov edx, others_check ; nop
    const uint32_t fn = reinterpret_cast<uint32_t>(&others_check);
    std::memcpy(code + 1, &fn, 4);
    if (!write_bytes(kOthersSite, code, sizeof(code))) {
        LOG_ERROR("Could not patch the foot IK question (Win32=%lu)", GetLastError());
        return false;
    }
    g_others_installed = true;
    LOG_INFO("Foot IK for other characters in place: human NPCs %s (FootIkNpcs), other players %s (FootIkPlayers)",
             g_npcs ? "on" : "off", g_players ? "on" : "off");
    return true;
}

void footik_set_npcs(bool on) {
    const long was = InterlockedExchange(&g_npcs, on ? 1 : 0);
    if (g_others_installed && was != (on ? 1 : 0)) {
        LOG_INFO("FootIkNpcs = %s: %s", on ? "true" : "false",
                 on ? "human NPCs (summoned phantoms too) place their feet on stairs and slopes"
                    : "only your character's feet are placed among the NPCs (the game's own)");
    }
}

void footik_set_players(bool on) {
    const long was = InterlockedExchange(&g_players, on ? 1 : 0);
    if (g_others_installed && was != (on ? 1 : 0)) {
        LOG_INFO("FootIkPlayers = %s: %s", on ? "true" : "false",
                 on ? "the other players' characters place their feet on stairs and slopes"
                    : "the other players' characters keep their animation's feet (the game's own)");
    }
}

bool footik_npcs() { return g_others_installed && g_npcs != 0; }
bool footik_players() { return g_others_installed && g_players != 0; }

void footik_profile_start() {
    if (!g_prof_on) {
        DWORD old = 0;
        const uint32_t wrapper = reinterpret_cast<uint32_t>(&place_timed);
        if (!VirtualProtect(reinterpret_cast<void*>(kPlayerCtrlPlace), 4, PAGE_READWRITE, &old)) return;
        *reinterpret_cast<uint32_t*>(kPlayerCtrlPlace) = wrapper;
        VirtualProtect(reinterpret_cast<void*>(kPlayerCtrlPlace), 4, old, &old);
        QueryPerformanceFrequency(&g_prof_freq);
        g_prof_on = true;
    }
    for (auto& b : g_prof) b = ProfBucket{};
    g_prof_frames = 0;
}

void footik_profile_report(char* out, size_t size) {
    if (!g_prof_on || !g_prof_freq.QuadPart) {
        std::snprintf(out, size, "footikprof: not started");
        return;
    }
    const double us = 1e6 / static_cast<double>(g_prof_freq.QuadPart);
    const double frames = g_prof_frames ? static_cast<double>(g_prof_frames) : 1.0;
    auto per_call = [&](const ProfBucket& b) { return b.calls ? b.ticks * us / b.calls : 0.0; };
    auto per_frame = [&](const ProfBucket& b) { return b.ticks * us / frames; };
    std::snprintf(out, size,
                  "footikprof %lld frames: ours %lld calls %.2f us/call %.2f us/frame; others placed %lld calls %.2f "
                  "us/call %.2f us/frame (%.2f a frame); others left %lld calls %.2f us/call %.2f us/frame",
                  g_prof_frames, g_prof[0].calls, per_call(g_prof[0]), per_frame(g_prof[0]), g_prof[1].calls,
                  per_call(g_prof[1]), per_frame(g_prof[1]), g_prof[1].calls / frames, g_prof[2].calls,
                  per_call(g_prof[2]), per_frame(g_prof[2]));
}
