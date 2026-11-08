#include "sfxwatch.h"
#include "fpsu_buildcfg.h"

#if FPSU_DIAGNOSTICS

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kSlots = 4;
constexpr int kMaxHits = 96;
constexpr uint32_t kMaxTotal = 20000;
constexpr ULONGLONG kAfterFirstHit = 90000;  // ms the runtime watch stays after its first hit
constexpr int kSearchObjects = 6000;
constexpr uint32_t kSearchSpan = 0x400;

enum Phase { kLoad = 0, kRuntime = 1 };

struct Hit {
    uint32_t eip = 0;
    int slot = -1;
    int phase = 0;
    uint32_t count = 0;
    DWORD tid = 0;
    uint32_t ret[4] = {};
    uint32_t reg[8] = {};  // eax ebx ecx edx esi edi ebp esp, at the first hit
    bool logged = false;
};

int g_target = 0;
std::atomic<bool> g_done{false};
std::atomic<int> g_phase{-1};         // -1 nothing armed
void* g_addr[kSlots] = {};
uint32_t g_values[kSlots] = {};      // the block's lifetime, offset, count, interval as loaded
const uint8_t* g_block = nullptr;     // in the registration buffer
const char* const kNames[kSlots] = {"lifetime (+4)", "offset (+8)", "count (+0xC)", "interval (+0x10)"};
std::atomic<bool> g_runtime_pending{false};
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
// and the game hung as the area loaded (2026-11-07 20:31). A nested exception on the same thread is let through.
thread_local int t_depth = 0;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const int phase = g_phase;
    if (phase < 0) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* ctx = info->ContextRecord;
    const DWORD dr6 = ctx->Dr6;
    if ((dr6 & 0xF) == 0) return EXCEPTION_CONTINUE_SEARCH;
    if (t_depth > 0) {
        ctx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    ++t_depth;
    if (g_total.fetch_add(1) == 0 || (phase == kRuntime && g_first_hit == 0)) g_first_hit = GetTickCount64();
    for (int i = 0; i < kSlots; ++i) {
        if (!(dr6 & (1u << i)) || !g_addr[i]) continue;
        while (g_lock.exchange(1, std::memory_order_acquire) != 0) YieldProcessor();
        Hit* h = nullptr;
        for (int k = 0; k < g_nhits; ++k) {
            if (g_hits[k].eip == ctx->Eip && g_hits[k].slot == i && g_hits[k].phase == phase) {
                h = &g_hits[k];
                break;
            }
        }
        if (!h && g_nhits < kMaxHits) {
            h = &g_hits[g_nhits++];
            h->eip = ctx->Eip;
            h->slot = i;
            h->phase = phase;
            h->tid = GetCurrentThreadId();
            const uint32_t r[8] = {ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp};
            std::memcpy(h->reg, r, sizeof(r));
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

DWORD dr7_for_slots() {
    DWORD dr7 = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (!g_addr[i]) continue;
        // local enable, RW = 11 (read or write), LEN = 11 (4 bytes)
        dr7 |= (1u << (i * 2)) | (3u << (16 + i * 4)) | (3u << (18 + i * 4));
    }
    return dr7;
}

bool set_thread(HANDLE th, bool enable) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &ctx)) return false;
    DWORD* regs[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
    for (int i = 0; i < kSlots; ++i) *regs[i] = enable && g_addr[i] ? reinterpret_cast<DWORD>(g_addr[i]) : 0;
    ctx.Dr7 = enable ? dr7_for_slots() : 0;
    ctx.Dr6 = 0;
    return SetThreadContext(th, &ctx) != 0;
}

// Every thread but this one is suspended for the change; this one sets its own (debug registers only).
void apply_all(bool enable) {
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
                if (set_thread(th, enable)) ++applied;
                if (!me) ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &e));
    }
    CloseHandle(snap);
    LOG_INFO("SfxWatch: runtime watchpoints %s on %d threads", enable ? "set" : "lifted", applied);
}

bool set_self(bool enable) {
    HANDLE th = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT, FALSE, GetCurrentThreadId());
    if (!th) return false;
    const bool ok = set_thread(th, enable);
    CloseHandle(th);
    return ok;
}

void report(bool all) {
    Hit copy[kMaxHits];
    while (g_lock.exchange(1, std::memory_order_acquire) != 0) YieldProcessor();
    const int n = g_nhits;
    std::memcpy(copy, g_hits, sizeof(Hit) * n);
    for (int k = 0; k < g_nhits; ++k) g_hits[k].logged = true;
    g_lock.store(0, std::memory_order_release);
    for (int k = 0; k < n; ++k) {
        const Hit& h = copy[k];
        if (h.logged && !all) continue;
        LOG_INFO("SfxWatch: [%s] %08X touched the %s, %u times (thread %lu; stack %08X %08X %08X %08X; eax %08X ebx "
                 "%08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X)",
                 h.phase == kLoad ? "load" : "runtime", h.eip, h.slot >= 0 ? kNames[h.slot] : "?", h.count, h.tid,
                 h.ret[0], h.ret[1], h.ret[2], h.ret[3], h.reg[0], h.reg[1], h.reg[2], h.reg[3], h.reg[4], h.reg[5],
                 h.reg[6], h.reg[7]);
    }
}

bool readable(const void* p, void* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// How many bytes from p can be read without touching a guard page (another thread's stack) or an uncommitted one.
uint32_t readable_span(uint32_t p, uint32_t want) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return 0;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    if ((mbi.Protect & ok) == 0) return 0;
    const uint32_t end = reinterpret_cast<uint32_t>(mbi.BaseAddress) + static_cast<uint32_t>(mbi.RegionSize);
    return (end - p) < want ? end - p : want;
}

bool heap_pointer(uint32_t v) {
    return v >= 0x00010000 && v < 0x7FFE0000 && (v & 3) == 0 && !(v >= 0x00400000 && v < 0x01600000);
}

// Breadth first through what the effect's resource points to, for the block's lifetime, offset and count side by
// side (the library's copy of the timing). Returns the address of the lifetime.
uint8_t* find_copy(const void* root) {
    std::vector<uint32_t> queue;
    std::unordered_set<uint32_t> seen;
    queue.push_back(reinterpret_cast<uint32_t>(root));
    seen.insert(queue.back());
    std::vector<uint8_t> buf(kSearchSpan);
    size_t head = 0;
    int found = 0;
    uint8_t* first = nullptr;
    while (head < queue.size() && static_cast<int>(head) < kSearchObjects) {
        const uint32_t obj = queue[head++];
        const uint32_t span = readable_span(obj, kSearchSpan) & ~3u;
        if (span < 0x10 || !readable(reinterpret_cast<const void*>(obj), buf.data(), span)) continue;
        for (uint32_t o = 0; o + 12 <= span; o += 4) {
            if (rd32(buf.data(), o) == g_values[0] && rd32(buf.data(), o + 4) == g_values[1] &&
                rd32(buf.data(), o + 8) == g_values[2]) {
                uint8_t* at = reinterpret_cast<uint8_t*>(obj + o);
                if (at == g_block + 4) continue;  // the registration buffer itself
                ++found;
                LOG_INFO("SfxWatch: the timing's values found at %p (object %08X +0x%X, %zu objects searched)%s", at,
                         obj, o, head, found == 1 ? ": watched" : "");
                if (!first) first = at;
                if (found >= 6) return first;
            }
        }
        for (uint32_t o = 0; o + 4 <= span; o += 4) {
            const uint32_t v = rd32(buf.data(), o);
            if (!heap_pointer(v)) continue;
            if (seen.size() > 40000) break;
            if (!seen.insert(v).second) continue;
            queue.push_back(v);
        }
    }
    if (!first) LOG_INFO("SfxWatch: the timing's values were not found in the %zu objects searched", head);
    return first;
}

}  // namespace

void sfxwatch_set_target(int effect_id) {
    g_target = effect_id;
}

bool sfxwatch_wanted() {
    return g_target > 0;
}

bool sfxwatch_before_load(const uint8_t* d, uint32_t size) {
    if (g_target <= 0 || g_done || g_phase >= 0 || !d || size < 0x40 || std::memcmp(d, "FXR\0", 4) != 0) return false;
    const uint32_t start = rd32(d, 8);
    if (start + 8 > size || rd32(d, start) != 133 || static_cast<int>(rd32(d, start + 4)) != g_target) return false;
    // The same walk as FixSfxSpawnRate (sfx.cpp): a pointer field at the end of an AST leads to a pond3 struct.
    const uint32_t table = rd32(d, 0x0C);
    const uint32_t count = rd32(d, 0x10);
    if (table > size || count > (size - table) / 4) return false;
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
        g_block = d + target;
        for (int i = 0; i < kSlots; ++i) {
            g_addr[i] = const_cast<uint8_t*>(g_block) + 4 + 4 * i;
            g_values[i] = rd32(g_block, 4 + 4 * i);
        }
        float v[4];
        std::memcpy(v, g_block + 4, 16);
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_exception);
        g_phase = kLoad;
        const bool ok = set_self(true);
        LOG_INFO("SfxWatch: effect %d loads; its first type-2 timing (lifetime %g, offset %g, count %d, interval %g) "
                 "watched on the loading thread: %s",
                 g_target, v[0], v[1], static_cast<int>(g_values[2]), v[3], ok ? "yes" : "no (could not set)");
        return true;
    }
    LOG_INFO("SfxWatch: effect %d loads, but it has no type-2 timing", g_target);
    g_done = true;
    return false;
}

void sfxwatch_after_load(const void* resource) {
    if (g_phase != kLoad) return;
    set_self(false);
    g_phase = -1;
    LOG_INFO("SfxWatch: effect %d loaded (resource %p); what read its timing while it loaded:", g_target, resource);
    report(true);
    uint8_t* copy = resource ? find_copy(resource) : nullptr;
    if (!copy) {
        g_done = true;
        return;
    }
    for (int i = 0; i < kSlots; ++i) g_addr[i] = copy + 4 * i;
    while (g_lock.exchange(1, std::memory_order_acquire) != 0) YieldProcessor();
    g_nhits = 0;
    g_lock.store(0, std::memory_order_release);
    g_total = 0;
    g_first_hit = 0;
    g_runtime_pending = true;  // armed on every thread from the next frame (the game thread)
}

void sfxwatch_tick() {
    if (g_target <= 0 || g_done) return;
    if (g_runtime_pending) {
        g_runtime_pending = false;
        g_phase = kRuntime;
        apply_all(true);
        return;
    }
    if (g_phase != kRuntime) return;
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG last = 0;
    if (now - last >= 2000) {
        last = now;
        report(false);
    }
    const ULONGLONG first = g_first_hit;
    if ((first && now - first >= kAfterFirstHit) || g_total >= kMaxTotal) {
        apply_all(false);
        g_phase = -1;
        g_done = true;
        LOG_INFO("SfxWatch: done %llu ms after the first runtime hit, %u hits; every instruction seen:", now - first,
                 static_cast<unsigned>(g_total));
        report(true);
    }
}

#else  // A release build leaves this diagnostic out (see fpsu_buildcfg.h).

void sfxwatch_set_target(int) {}
bool sfxwatch_wanted() { return false; }
bool sfxwatch_before_load(const uint8_t*, uint32_t) { return false; }
void sfxwatch_after_load(const void*) {}
void sfxwatch_tick() {}

#endif  // FPSU_DIAGNOSTICS
