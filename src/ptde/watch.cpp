#include "watch.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kSlots = 4;
constexpr int kMaxHits = 600;
constexpr uint32_t kWorldChrMan = 0x0137DC70;

struct Spec {
    char base = 0;
    uint32_t offset = 0;
};

struct Hit {
    uint32_t eip = 0;
    int slot = -1;
    uint32_t count = 0;
    uint32_t first = 0, last = 0;
    float fmin = 0, fmax = 0;
    uint32_t ret[3] = {};
    DWORD tid = 0;
};

Spec g_spec[kSlots];
volatile uint32_t g_exec_ecx[kSlots] = {};
volatile uint32_t g_exec_arg0[kSlots] = {};
int g_nspec = 0;
void* g_addr[kSlots] = {};
Hit g_hits[kMaxHits];
int g_nhits = 0;
std::atomic<int> g_lock{0};
std::atomic<bool> g_armed{false};
PVOID g_veh = nullptr;
bool g_f8_down = false;
ULONGLONG g_armed_at = 0;

const void* rd_ptr(const void* p) {
    if (!p) return nullptr;
    __try {
        return *static_cast<const void* const*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool in_text(uint32_t v) {
    return v >= 0x401000 && v < 0x1200000;
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    const DWORD dr6 = ctx->Dr6;
    if ((dr6 & 0xF) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    for (int i = 0; i < kSlots; ++i) {
        if (!(dr6 & (1u << i)) || !g_addr[i]) continue;
        const bool exec = g_spec[i].base == 'x';
        if (exec) {
            ctx->EFlags |= 0x10000;  // resume flag: run the instruction this time
            // Remember the object the function was entered with: ecx (thiscall) and the first
            // stack argument. The trace dumps what they point to while the watch is armed.
            g_exec_ecx[i] = ctx->Ecx;
            uint32_t arg0 = 0;
            __try {
                arg0 = *reinterpret_cast<const uint32_t*>(ctx->Esp + 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            g_exec_arg0[i] = arg0;
        }
        const uint32_t v = exec ? 0 : *static_cast<volatile uint32_t*>(g_addr[i]);
        float f;
        std::memcpy(&f, &v, 4);
        while (g_lock.exchange(1, std::memory_order_acquire) != 0) YieldProcessor();
        Hit* h = nullptr;
        for (int k = 0; k < g_nhits; ++k) {
            if (g_hits[k].eip == ctx->Eip && g_hits[k].slot == i) {
                h = &g_hits[k];
                break;
            }
        }
        if (!h && g_nhits < kMaxHits) {
            h = &g_hits[g_nhits++];
            h->eip = ctx->Eip;
            h->slot = i;
            h->first = v;
            h->fmin = f;
            h->fmax = f;
            h->tid = GetCurrentThreadId();
            const uint32_t* sp = reinterpret_cast<const uint32_t*>(ctx->Esp);
            int n = 0;
            for (int s = 0; s < 96 && n < 3; ++s) {
                uint32_t w = 0;
                __try {
                    w = sp[s];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                if (in_text(w)) h->ret[n++] = w;
            }
        }
        if (h) {
            ++h->count;
            h->last = v;
            if (std::isfinite(f)) {
                if (f < h->fmin) h->fmin = f;
                if (f > h->fmax) h->fmax = f;
            }
        }
        g_lock.store(0, std::memory_order_release);
    }
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void apply_registers(bool enable) {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 e{};
    e.dwSize = sizeof(e);
    int applied = 0;
    if (Thread32First(snap, &e)) {
        do {
            if (e.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                                   e.th32ThreadID);
            if (!th) continue;
            const bool me = e.th32ThreadID == self;
            if (me || SuspendThread(th) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &ctx)) {
                    DWORD* regs[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
                    DWORD dr7 = 0;
                    for (int i = 0; i < kSlots && enable; ++i) {
                        if (!g_addr[i]) continue;
                        *regs[i] = reinterpret_cast<DWORD>(g_addr[i]);
                        if (g_spec[i].base == 'x') {
                            dr7 |= (1u << (i * 2));  // execute breakpoint: RW = 00, LEN = 00
                        } else {
                            dr7 |= (1u << (i * 2)) | (1u << (16 + i * 4)) | (3u << (18 + i * 4));
                        }
                    }
                    ctx.Dr7 = dr7;
                    ctx.Dr6 = 0;
                    if (SetThreadContext(th, &ctx)) ++applied;
                }
                if (!me) ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &e));
    }
    CloseHandle(snap);
    LOG_INFO("Watch: debug registers %s on %d threads", enable ? "set" : "cleared", applied);
}

void arm() {
    const uint8_t* wcm = static_cast<const uint8_t*>(rd_ptr(reinterpret_cast<const void*>(kWorldChrMan)));
    const uint8_t* first = static_cast<const uint8_t*>(rd_ptr(wcm ? wcm + 4 : nullptr));
    const uint8_t* chr = static_cast<const uint8_t*>(rd_ptr(first));
    const uint8_t* mc = static_cast<const uint8_t*>(rd_ptr(chr ? chr + 0x28 : nullptr));
    const uint8_t* phys = static_cast<const uint8_t*>(rd_ptr(mc ? mc + 0x1C : nullptr));
    if (!chr || !mc || !phys) {
        LOG_ERROR("Watch: player structures not found");
        return;
    }
    // The physics proxy's position vector (what the game's setPosition/getPosition at
    // 0x8F2380/0x8F2370 write and read); "y" watches it.
    const uint8_t* proxy_pos = nullptr;
    if (const void* proxy = rd_ptr(phys + 0x38)) {
        void* result = nullptr;
        __try {
            using GetPos = void*(__fastcall*)(const void*, void*);
            result = reinterpret_cast<GetPos>(0x008F2370)(proxy, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        proxy_pos = static_cast<const uint8_t*>(result);
    }
    const uint8_t* proxy_vel = nullptr;
    if (const void* proxy = rd_ptr(phys + 0x38)) {
        void* result = nullptr;
        __try {
            using GetVel = void*(__fastcall*)(const void*, void*);
            result = reinterpret_cast<GetVel>(0x008F23B0)(proxy, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        proxy_vel = static_cast<const uint8_t*>(result);
    }
    LOG_INFO("Watch: chr=%p mc=%p phys=%p proxy position=%p velocity=%p", chr, mc, phys, proxy_pos, proxy_vel);
    {
        // The physics body, as floats and hex, to read fields that the CSV does not cover.
        char line[256];
        for (int o = 0; o < 0x260; o += 0x10) {
            uint32_t w[4] = {};
            __try {
                std::memcpy(w, phys + o, 16);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            float f[4];
            std::memcpy(f, w, 16);
            std::snprintf(line, sizeof(line), "  phys+%03X: %08X %08X %08X %08X | %g %g %g %g", o, w[0], w[1], w[2], w[3],
                          f[0], f[1], f[2], f[3]);
            LOG_INFO("%s", line);
        }
    }
    // CameraMan ([0x137847C]) holds two camera objects: "j" = the first, "k" = the second (the
    // one whose matrix the game renders with).
    const uint8_t* cman = static_cast<const uint8_t*>(rd_ptr(reinterpret_cast<const void*>(0x0137847C)));
    const uint8_t* camera0 = static_cast<const uint8_t*>(rd_ptr(cman ? cman + 4 : nullptr));
    const uint8_t* camera1 = static_cast<const uint8_t*>(rd_ptr(cman ? cman + 8 : nullptr));
    LOG_INFO("Watch: camera objects %p %p", camera0, camera1);
    // "g" = PlayerGameData ([[0x1378700] + 8]): hit points at +0xC, stamina near +0x28.
    const uint8_t* game_data = nullptr;
    if (const uint8_t* manager = static_cast<const uint8_t*>(rd_ptr(reinterpret_cast<const void*>(0x01378700)))) {
        game_data = static_cast<const uint8_t*>(rd_ptr(manager + 8));
    }
    g_nhits = 0;
    for (int i = 0; i < kSlots; ++i) {
        g_addr[i] = nullptr;
        if (i >= g_nspec) continue;
        if (g_spec[i].base == 'g') {
            g_addr[i] = const_cast<uint8_t*>(game_data + g_spec[i].offset);
            LOG_INFO("Watch: slot %d = g+0x%X at %p", i, g_spec[i].offset, g_addr[i]);
            continue;
        }
        const uint8_t* base = g_spec[i].base == 'c'   ? chr
                              : g_spec[i].base == 'm' ? mc
                              : g_spec[i].base == 'y' ? proxy_pos
                              : g_spec[i].base == 'v' ? proxy_vel
                              : g_spec[i].base == 'x' ? nullptr
                              : g_spec[i].base == 'k' ? camera1
                              : g_spec[i].base == 'j' ? camera0
                                                      : phys;
        g_addr[i] = const_cast<uint8_t*>(base + g_spec[i].offset);
        LOG_INFO("Watch: slot %d = %c+0x%X at %p", i, g_spec[i].base, g_spec[i].offset, g_addr[i]);
    }
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_exception);
    apply_registers(true);
    g_armed = true;
    g_armed_at = GetTickCount64();
}

void report() {
    g_armed = false;
    apply_registers(false);
    while (g_lock.exchange(1, std::memory_order_acquire) != 0) YieldProcessor();
    LOG_INFO("Watch: %d writers", g_nhits);
    for (int i = 0; i < g_nhits; ++i) {
        const Hit& h = g_hits[i];
        LOG_INFO("  slot%d %c+0x%X  eip=%08X n=%u  first=%08X (%g) last=%08X  min=%g max=%g  ret=%08X %08X %08X tid=%lu",
                 h.slot, g_spec[h.slot].base, g_spec[h.slot].offset, h.eip, h.count, h.first,
                 *reinterpret_cast<const float*>(&h.first), h.last, h.fmin, h.fmax, h.ret[0], h.ret[1], h.ret[2],
                 h.tid);
    }
    g_lock.store(0, std::memory_order_release);
}

}  // namespace

void watch_set_spec(const char* spec) {
    g_nspec = 0;
    const char* p = spec;
    while (*p && g_nspec < kSlots) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        const char base = *p++;
        if (*p == ':') ++p;
        char* end = nullptr;
        const uint32_t off = std::strtoul(p, &end, 16);
        if (end == p) break;
        g_spec[g_nspec].base = base;
        g_spec[g_nspec].offset = off;
        ++g_nspec;
        p = end;
    }
    if (g_nspec) LOG_INFO("Watch: %d slot(s) configured; F8 arms and disarms", g_nspec);
}

void watch_poll() {
    if (g_nspec == 0) return;
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !g_f8_down) {
        if (g_armed) report(); else arm();
    }
    g_f8_down = down;
    if (g_armed && GetTickCount64() - g_armed_at > 40000) report();
}

void watch_arm_addresses(const uint32_t* addresses, int count) {
    if (g_armed) return;
    if (count > kSlots) count = kSlots;
    g_nspec = count;
    g_nhits = 0;
    for (int i = 0; i < kSlots; ++i) g_addr[i] = nullptr;
    for (int i = 0; i < count; ++i) {
        g_spec[i].base = 'a';
        g_spec[i].offset = addresses[i];
        g_addr[i] = reinterpret_cast<void*>(addresses[i]);
        LOG_INFO("Watch: write watchpoint %d on %08X", i, addresses[i]);
    }
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_exception);
    apply_registers(true);
    g_armed = true;
    g_armed_at = GetTickCount64();
}

void watch_report_now() {
    if (g_armed) report();
}

uint32_t watch_exec_ecx(int slot) {
    return slot >= 0 && slot < kSlots ? g_exec_ecx[slot] : 0;
}

uint32_t watch_exec_arg0(int slot) {
    return slot >= 0 && slot < kSlots ? g_exec_arg0[slot] : 0;
}
