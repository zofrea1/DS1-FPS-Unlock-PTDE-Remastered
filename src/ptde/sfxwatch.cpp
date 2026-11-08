#include "sfxwatch.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace {

constexpr int kSlots = 4;
constexpr int kMaxHits = 64;
constexpr uint32_t kMaxTotal = 5000;
constexpr ULONGLONG kAfterFirstHit = 20000;  // ms the watch stays after the first hit

struct Hit {
    uint32_t eip = 0;
    int slot = -1;
    uint32_t count = 0;
    DWORD tid = 0;
    uint32_t ret[4] = {};
    bool logged = false;
};

int g_target = 0;
std::atomic<bool> g_captured{false};
std::atomic<bool> g_rearm{false};  // the effect loaded again (a new copy): the watch moves to it
void* g_addr[kSlots] = {};
const char* const kNames[kSlots] = {"lifetime (+4)", "offset (+8)", "count (+0xC)", "interval (+0x10)"};
std::atomic<bool> g_armed{false};
std::atomic<bool> g_done{false};
std::atomic<ULONGLONG> g_first_hit{0};
PVOID g_veh = nullptr;
Hit g_hits[kMaxHits];
int g_nhits = 0;
std::atomic<uint32_t> g_total{0};
std::atomic<int> g_lock{0};

uint32_t rd32(const uint8_t* d, uint32_t o) {
    uint32_t v;
    std::memcpy(&v, d + o, 4);
    return v;
}

bool in_text(uint32_t v) {
    return v >= 0x401000 && v < 0x10CC000;
}

// The handler runs with the thread's watchpoints still set, so it must not touch the watched fields: a read there
// (the first build logged each field's value) raised the exception again inside the handler while it held the lock,
// and the game hung as the area loaded (2026-11-07 20:31). The values are logged when the effect loads instead, and
// a nested exception on the same thread is let through untouched.
thread_local int t_depth = 0;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_armed) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* ctx = info->ContextRecord;
    const DWORD dr6 = ctx->Dr6;
    if ((dr6 & 0xF) == 0) return EXCEPTION_CONTINUE_SEARCH;
    if (t_depth > 0) {
        ctx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    ++t_depth;
    if (g_total.fetch_add(1) == 0) g_first_hit = GetTickCount64();
    for (int i = 0; i < kSlots; ++i) {
        if (!(dr6 & (1u << i)) || !g_addr[i]) continue;
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
            h->tid = GetCurrentThreadId();
            const uint32_t* sp = reinterpret_cast<const uint32_t*>(ctx->Esp);
            int n = 0;
            for (int s = 0; s < 128 && n < 4; ++s) {
                uint32_t w = 0;
                __try {
                    w = sp[s];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                if (in_text(w)) h->ret[n++] = w;
            }
        }
        if (h) ++h->count;
        g_lock.store(0, std::memory_order_release);
    }
    ctx->Dr6 = 0;
    --t_depth;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void apply(bool enable) {
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
                        // local enable, RW = 11 (read or write), LEN = 11 (4 bytes)
                        dr7 |= (1u << (i * 2)) | (3u << (16 + i * 4)) | (3u << (18 + i * 4));
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
    LOG_INFO("SfxWatch: watchpoints %s on %d threads", enable ? "set" : "lifted", applied);
}

void report(bool all) {
    Hit copy[kMaxHits];
    int n = 0;
    while (g_lock.exchange(1, std::memory_order_acquire) != 0) YieldProcessor();
    n = g_nhits;
    std::memcpy(copy, g_hits, sizeof(Hit) * n);
    for (int k = 0; k < g_nhits; ++k) g_hits[k].logged = true;
    g_lock.store(0, std::memory_order_release);
    for (int k = 0; k < n; ++k) {
        const Hit& h = copy[k];
        if (h.logged && !all) continue;
        LOG_INFO("SfxWatch: %08X touched the %s of effect %d's first type-2 timing, %u times (thread %lu; stack "
                 "%08X %08X %08X %08X)",
                 h.eip, h.slot >= 0 ? kNames[h.slot] : "?", g_target, h.count, h.tid, h.ret[0], h.ret[1], h.ret[2],
                 h.ret[3]);
    }
}

}  // namespace

void sfxwatch_set_target(int effect_id) {
    g_target = effect_id;
}

bool sfxwatch_wanted() {
    return g_target > 0;
}

void sfxwatch_on_register(const uint8_t* d, uint32_t size) {
    if (g_target <= 0 || g_done || !d || size < 0x40 || std::memcmp(d, "FXR\0", 4) != 0) return;
    const uint32_t start = rd32(d, 8);
    if (start + 8 > size || rd32(d, start) != 133 || static_cast<int>(rd32(d, start + 4)) != g_target) return;
    // The same walk as FixSfxSpawnRate (sfx.cpp): a pointer field at the end of an AST leads to a pond3 struct.
    const uint32_t table = rd32(d, 0x0C);
    const uint32_t count = rd32(d, 0x10);
    if (table > size || count > (size - table) / 4) return;
    std::vector<uint32_t> fields(count);
    for (uint32_t i = 0; i < count; ++i) fields[i] = rd32(d, table + i * 4);
    std::sort(fields.begin(), fields.end());
    auto listed = [&](uint32_t o) { return std::binary_search(fields.begin(), fields.end(), o); };
    for (uint32_t o : fields) {
        if (o < 0x14 || o > size - 4) continue;
        const uint32_t ast = o - 0x14;
        const uint32_t pond1 = rd32(d, ast), n1 = rd32(d, ast + 4), n2 = rd32(d, ast + 8);
        const uint8_t* flags = d + ast + 12;
        const uint32_t pond2 = rd32(d, ast + 16);
        if (n1 != n2 || n1 >= 4096 || flags[3] != 0 || flags[0] > 1 || flags[1] > 1 || flags[2] > 1) continue;
        if ((pond1 && !listed(ast)) || (pond2 && !listed(ast + 16))) continue;
        const uint32_t target = rd32(d, o);
        if (target > size - 0x18 || rd32(d, target) != 2) continue;
        uint8_t* block = const_cast<uint8_t*>(d) + target;
        for (int i = 0; i < kSlots; ++i) g_addr[i] = block + 4 + 4 * i;
        float v[4];
        std::memcpy(v, block + 4, 16);
        LOG_INFO("SfxWatch: effect %d loaded; its first type-2 timing at %p: lifetime %g, offset %g, count %d, "
                 "interval %g (watched from the next frame)",
                 g_target, block, v[0], v[1], static_cast<int>(rd32(block, 0xC)), v[3]);
        if (g_captured) g_rearm = true;
        g_captured = true;
        return;
    }
    LOG_INFO("SfxWatch: effect %d loaded, but it has no type-2 timing", g_target);
    g_done = true;
}

void sfxwatch_tick() {
    if (g_target <= 0 || g_done || !g_captured) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_armed || g_rearm) {
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_exception);
        g_armed = true;
        g_rearm = false;
        apply(true);
        return;
    }
    static ULONGLONG last = 0;
    if (now - last >= 2000) {
        last = now;
        report(false);
    }
    const ULONGLONG first = g_first_hit;
    if ((first && now - first >= kAfterFirstHit) || g_total >= kMaxTotal) {
        apply(false);
        g_armed = false;
        g_done = true;
        LOG_INFO("SfxWatch: done %llu ms after the first hit, %u hits; every instruction seen:", now - first,
                 static_cast<unsigned>(g_total));
        report(true);
    }
}
