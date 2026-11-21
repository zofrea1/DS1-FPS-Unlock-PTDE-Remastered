#include "phases.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// The frame function's own direct calls (call rel32), in order. Its calls through registers are not timed; their
// share is the frame's ticks minus the sites'.
constexpr uint32_t kFrameSites[] = {0xF917E6, 0xF917F9, 0xF91807, 0xF9180D, 0xF9181C, 0xF91856, 0xF91865,
                                    0xF9187A, 0xF91892, 0xF918A6, 0xF918BB, 0xF918CA, 0xF918D8, 0xF918E7,
                                    0xF91901, 0xF91906, 0xF91910, 0xF91915, 0xF9194A};

struct Site {
    uint32_t ret;      // the caller's return address while the callee runs (the stub popped it)
    uint32_t pad;
    uint64_t start;    // counter at the call
    uint64_t ticks;    // counter ticks inside the callee, since the last report
    uint32_t address;  // the call instruction
    uint32_t target;   // the callee
    uint32_t calls;    // since the last report
    uint8_t* stub;
    uint32_t vt;       // a method table's slot instead of a call site: the table
    int slot;
    const volatile uint32_t* tid;  // the thread timed (others go straight through)
};
constexpr int kMaxSites = 200;
Site g_sites[kMaxSites];
int g_count = 0;
uint8_t* g_code = nullptr;  // stubs (executable)
size_t g_code_used = 0;
constexpr size_t kCodeSize = 200 * 128;
volatile uint32_t g_stub_tid = 0;    // the game thread (set by `phases on`)
volatile uint32_t g_render_tid = 0;  // the render thread (phases_render_frame)
// Render-thread call sites wait here until the render thread's next frame start patches them (it is then outside them).
SRWLOCK g_sites_lock = SRWLOCK_INIT;
uint32_t g_pending_radd[64];
volatile LONG g_pending_n = 0;

bool g_active = false;
uint64_t g_frame_ticks = 0;
uint32_t g_frames = 0;
uint64_t g_tsc0 = 0;
LARGE_INTEGER g_qpc0{};
double g_tsc_per_ms = 0.0;  // learned from the first report window

void put8(uint8_t*& p, uint8_t v) { *p++ = v; }
void put32(uint8_t*& p, uint32_t v) {
    std::memcpy(p, &v, 4);
    p += 4;
}

// The stub for one site: takes the return address off the stack (so the callee sees its arguments where the caller
// left them), reads the counter, calls the callee, adds the counter difference, returns to the caller. eax and edx
// (return values, register arguments) are kept around the counter reads; the flags are not (no caller expects them
// across a call).
uint8_t* build_stub(Site& s) {
    if (!g_code) {
        g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCodeSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_code) return nullptr;
    }
    if (g_code_used + 128 > kCodeSize) return nullptr;
    uint8_t* stub = g_code + g_code_used;
    uint8_t* p = stub;
    // Another thread than the game's goes straight to the callee (the stub keeps one return address per site, so only
    // the game thread is timed): push eax / mov eax, fs:[0x24] / cmp eax, [g_stub_tid] / pop eax / jne other.
    put8(p, 0x50);
    put8(p, 0x64); put8(p, 0xA1); put32(p, 0x24);
    put8(p, 0x3B); put8(p, 0x05); put32(p, reinterpret_cast<uint32_t>(s.tid ? s.tid : &g_stub_tid));
    put8(p, 0x58);
    put8(p, 0x0F); put8(p, 0x85);
    uint8_t* jne_rel = p;
    put32(p, 0);
    const uint32_t ret = reinterpret_cast<uint32_t>(&s.ret);
    const uint32_t start = reinterpret_cast<uint32_t>(&s.start);
    const uint32_t ticks = reinterpret_cast<uint32_t>(&s.ticks);
    const uint32_t calls = reinterpret_cast<uint32_t>(&s.calls);
    put8(p, 0x8F); put8(p, 0x05); put32(p, ret);                        // pop [ret]
    put8(p, 0x50); put8(p, 0x52);                                       // push eax; push edx
    put8(p, 0x0F); put8(p, 0x31);                                       // rdtsc
    put8(p, 0xA3); put32(p, start);                                     // mov [start], eax
    put8(p, 0x89); put8(p, 0x15); put32(p, start + 4);                  // mov [start+4], edx
    put8(p, 0x5A); put8(p, 0x58);                                       // pop edx; pop eax
    if (s.vt) {
        put8(p, 0xFF); put8(p, 0x15); put32(p, reinterpret_cast<uint32_t>(&s.target));  // call [target]
    } else {
        put8(p, 0xE8); put32(p, s.target - (reinterpret_cast<uint32_t>(p) + 4));  // call target
    }
    put8(p, 0x50); put8(p, 0x52);                                       // push eax; push edx
    put8(p, 0x0F); put8(p, 0x31);                                       // rdtsc
    put8(p, 0x2B); put8(p, 0x05); put32(p, start);                      // sub eax, [start]
    put8(p, 0x1B); put8(p, 0x15); put32(p, start + 4);                  // sbb edx, [start+4]
    put8(p, 0x01); put8(p, 0x05); put32(p, ticks);                      // add [ticks], eax
    put8(p, 0x11); put8(p, 0x15); put32(p, ticks + 4);                  // adc [ticks+4], edx
    put8(p, 0xFF); put8(p, 0x05); put32(p, calls);                      // inc dword [calls]
    put8(p, 0x5A); put8(p, 0x58);                                       // pop edx; pop eax
    put8(p, 0xFF); put8(p, 0x25); put32(p, ret);                        // jmp [ret]
    // other: the callee, with the caller's return address still on the stack
    const uint32_t other = reinterpret_cast<uint32_t>(p);
    const uint32_t rel = other - (reinterpret_cast<uint32_t>(jne_rel) + 4);
    std::memcpy(jne_rel, &rel, 4);
    if (s.vt) {
        put8(p, 0xFF); put8(p, 0x25); put32(p, reinterpret_cast<uint32_t>(&s.target));  // jmp [target]
    } else {
        put8(p, 0xE9); put32(p, s.target - (reinterpret_cast<uint32_t>(p) + 4));       // jmp target
    }
    g_code_used += 128;
    return stub;
}

// Times the call at `address`. Game thread only, at a frame start (no other thread runs the sites).
const char* add_site(uint32_t address, const volatile uint32_t* tid = nullptr) {
    for (int i = 0; i < g_count; ++i) {
        if (g_sites[i].address == address) return "already timed";
    }
    if (g_count >= kMaxSites) return "too many sites";
    auto* at = reinterpret_cast<uint8_t*>(address);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE))) {
        return "not code";
    }
    if (at[0] != 0xE8) return "not a call rel32";
    int32_t rel = 0;
    std::memcpy(&rel, at + 1, 4);
    Site& s = g_sites[g_count];
    s = Site{};
    s.address = address;
    s.target = address + 5 + static_cast<uint32_t>(rel);
    s.tid = tid;
    s.stub = build_stub(s);
    if (!s.stub) return "no room for the stub";
    DWORD old = 0;
    if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return "could not unprotect";
    const int32_t new_rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(s.stub) - (address + 5));
    std::memcpy(at + 1, &new_rel, 4);
    DWORD ignored = 0;
    VirtualProtect(at + 1, 4, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    ++g_count;
    return "timed";
}

void report_groups(double tsc_per_ms, uint32_t frames);

// Times every call of method `slot` of the class whose method table is `vt` (all its objects): the table's entry
// goes to a stub that calls the original. For methods only the game thread calls (the stub keeps one return
// address per site).
const char* add_vt(uint32_t vt, int slot, const volatile uint32_t* tid = nullptr) {
    for (int i = 0; i < g_count; ++i) {
        if (g_sites[i].vt == vt && g_sites[i].slot == slot) return "already timed";
    }
    if (g_count >= kMaxSites || slot < 0 || slot > 200) return "no";
    auto* entry = reinterpret_cast<uint32_t*>(vt + 4 * slot);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(entry, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return "not mapped";
    Site& s = g_sites[g_count];
    s = Site{};
    s.vt = vt;
    s.slot = slot;
    s.tid = tid;
    s.address = vt + 4 * slot;
    s.target = *entry;
    s.stub = build_stub(s);
    if (!s.stub) return "no room for the stub";
    DWORD old = 0;
    if (!VirtualProtect(entry, 4, PAGE_READWRITE, &old)) return "could not unprotect";
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), static_cast<LONG>(reinterpret_cast<uint32_t>(s.stub)));
    DWORD ignored = 0;
    VirtualProtect(entry, 4, old, &ignored);
    ++g_count;
    return "timed";
}

void report() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    const uint64_t tsc = __rdtsc();
    const double ms = 1000.0 * static_cast<double>(now.QuadPart - g_qpc0.QuadPart) / static_cast<double>(f.QuadPart);
    if (ms <= 0.0 || g_frames == 0) return;
    g_tsc_per_ms = static_cast<double>(tsc - g_tsc0) / ms;
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "[phases] %.1fs %u frames, ms per frame: frame %.3f |", ms / 1000.0,
                          g_frames, static_cast<double>(g_frame_ticks) / g_tsc_per_ms / g_frames);
    for (int i = 0; i < g_count; ++i) {
        Site& s = g_sites[i];
        if (n > static_cast<int>(sizeof(line)) - 60) {
            LOG_INFO("%s", line);
            n = std::snprintf(line, sizeof(line), "[phases]   ...");
        }
        // Past the frame function's own sites, only those that take some time (0.002 ms a frame or more).
        if (i >= static_cast<int>(sizeof(kFrameSites) / sizeof(kFrameSites[0])) &&
            static_cast<double>(s.ticks) / g_tsc_per_ms / g_frames < 0.002) {
            s.ticks = 0;
            s.calls = 0;
            continue;
        }
        if (s.vt) {
            n += std::snprintf(line + n, sizeof(line) - n, " vt%X/%d %.3f (%.1f/f)", s.vt, s.slot,
                               static_cast<double>(s.ticks) / g_tsc_per_ms / g_frames,
                               static_cast<double>(s.calls) / g_frames);
        } else {
            n += std::snprintf(line + n, sizeof(line) - n, " %X %.3f%s(%.1f/f)", s.target,
                               static_cast<double>(s.ticks) / g_tsc_per_ms / g_frames, s.calls > g_frames ? "+" : "",
                               static_cast<double>(s.calls) / g_frames);
        }
        s.ticks = 0;
        s.calls = 0;
    }
    LOG_INFO("%s", line);
    report_groups(g_tsc_per_ms, g_frames);
    g_frame_ticks = 0;
    g_frames = 0;
    g_tsc0 = tsc;
    g_qpc0 = now;
}

// The task manager (0xEFF320, most of the frame) calls its six task groups in a loop: mov edx, [eax+8] / push ecx /
// fstp dword [esp] / call edx at 0xEFF371..0xEFF379 (a group's update with the frame time). Those nine bytes (no branch
// lands inside them; the loop's skip goes to 0xEFF37A) become a call to group_stub and four nops: the stub does the
// same three steps, times the call by group (6 - edi: edi counts the groups down from 6, kept by the callee) and goes
// on at 0xEFF37A.
constexpr uint32_t kGroupPatch = 0xEFF371;
uint32_t g_group_ret = 0;
uint32_t g_group_back = 0xEFF37A;
uint64_t g_group_start = 0;
uint64_t g_group_ticks[8] = {};
uint32_t g_group_target[8] = {};
bool g_groups_on = false;

__declspec(naked) void group_stub() {
    __asm {
        pop dword ptr [g_group_ret]
        mov edx, dword ptr [eax + 8]
        push ecx
        fstp dword ptr [esp]
        push eax
        push edx
        rdtsc
        mov dword ptr [g_group_start], eax
        mov dword ptr [g_group_start + 4], edx
        pop edx
        pop eax
        mov ecx, 6
        sub ecx, edi
        mov dword ptr [g_group_target + ecx * 4], edx
        mov ecx, dword ptr [esi]  // the group object (ecx at the original call)
        call edx
        push eax
        push edx
        rdtsc
        sub eax, dword ptr [g_group_start]
        sbb edx, dword ptr [g_group_start + 4]
        mov ecx, 6
        sub ecx, edi
        add dword ptr [g_group_ticks + ecx * 8], eax
        adc dword ptr [g_group_ticks + ecx * 8 + 4], edx
        pop edx
        pop eax
        jmp dword ptr [g_group_back]
    }
}

const char* groups_on() {
    if (g_groups_on) return "already timed";
    auto* at = reinterpret_cast<uint8_t*>(kGroupPatch);
    static const uint8_t kExpect[9] = {0x8B, 0x50, 0x08, 0x51, 0xD9, 0x1C, 0x24, 0xFF, 0xD2};
    if (std::memcmp(at, kExpect, 9) != 0) return "unexpected bytes";
    DWORD old = 0;
    if (!VirtualProtect(at, 9, PAGE_EXECUTE_READWRITE, &old)) return "could not unprotect";
    at[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&group_stub) - (kGroupPatch + 5));
    std::memcpy(at + 1, &rel, 4);
    std::memset(at + 5, 0x90, 4);
    DWORD ignored = 0;
    VirtualProtect(at, 9, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 9);
    g_groups_on = true;
    return "timed";
}

// Each task: TaskItem::Run (0xEFF670) hands the frame to its delegate object: mov edx, [ecx] / push eax / mov eax,
// [edx+8] / push ecx / fstp dword [esp] / call eax at 0xEFF68C..0xEFF697, then ret 4 at 0xEFF698 (the branches go to
// 0xEFF685 and 0xEFF698, none inside). Those twelve bytes become a call to task_stub and a jump to the ret; the stub
// keeps everything on the stack (any thread may run it) and times the delegate's call by its class (method table).
constexpr uint32_t kTaskPatch = 0xEFF68C;
bool g_tasks_on = false;
uint32_t g_game_tid = 0;
struct TaskStat {
    uint32_t vt;
    uint32_t calls;
    uint64_t ticks;
};
TaskStat g_task_stats[256];

void __cdecl task_add(uint32_t vt, uint32_t lo, uint32_t hi) {
    if (GetCurrentThreadId() != g_game_tid) return;
    const uint64_t t = (static_cast<uint64_t>(hi) << 32) | lo;
    uint32_t h = (vt >> 3) & 255;
    for (int i = 0; i < 256; ++i, h = (h + 1) & 255) {
        TaskStat& e = g_task_stats[h];
        if (e.vt == vt || e.vt == 0) {
            e.vt = vt;
            e.calls++;
            e.ticks += t;
            return;
        }
    }
}

// In: eax = the TaskItem, ecx = its delegate, st0 = the frame time, [esp] = the return to the patch site + 5.
__declspec(naked) void task_stub() {
    __asm {
        push eax                    // the TaskItem
        mov edx, dword ptr [ecx]
        push edx                    // the delegate's method table (the key)
        push ecx                    // the delegate
        rdtsc
        push edx
        push eax                    // [esp] start lo, +4 hi, +8 delegate, +12 table, +16 TaskItem, +20 return
        mov ecx, dword ptr [esp + 8]
        mov edx, dword ptr [esp + 12]
        push dword ptr [esp + 16]   // the TaskItem (second argument)
        mov eax, dword ptr [edx + 8]
        push ecx                    // first argument: the frame time
        fstp dword ptr [esp]
        call eax                    // pops its two arguments
        rdtsc
        sub eax, dword ptr [esp]
        sbb edx, dword ptr [esp + 4]
        push edx
        push eax
        push dword ptr [esp + 20]   // the table
        call task_add
        add esp, 12 + 20
        ret
    }
}

const char* tasks_on() {
    if (g_tasks_on) return "already timed";
    g_game_tid = GetCurrentThreadId();
    auto* at = reinterpret_cast<uint8_t*>(kTaskPatch);
    static const uint8_t kExpect[12] = {0x8B, 0x11, 0x50, 0x8B, 0x42, 0x08, 0x51, 0xD9, 0x1C, 0x24, 0xFF, 0xD0};
    if (std::memcmp(at, kExpect, 12) != 0) return "unexpected bytes";
    DWORD old = 0;
    if (!VirtualProtect(at, 12, PAGE_EXECUTE_READWRITE, &old)) return "could not unprotect";
    uint8_t code[12];
    code[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&task_stub) - (kTaskPatch + 5));
    std::memcpy(code + 1, &rel, 4);
    code[5] = 0xEB;  // jmp 0xEFF698
    code[6] = 0x05;
    std::memset(code + 7, 0x90, 5);
    std::memcpy(at, code, 12);
    DWORD ignored = 0;
    VirtualProtect(at, 12, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 12);
    g_tasks_on = true;
    return "timed";
}

// Step state calls (0x4557C0, the step dispatcher every *Step task runs through: MoveMapStep's world update and the
// rest): push ebp / push ecx / mov ecx, esi / fstp dword [esp] / call eax at 0x4557DE..0x4557E6 (no branch lands
// inside). The nine bytes become a call to step_stub and a short jump to 0x4557E7; timed by the state function.
constexpr uint32_t kStepPatch = 0x4557DE;
bool g_steps_on = false;

__declspec(naked) void step_stub() {
    __asm {
        push eax                    // the state function (the key)
        rdtsc
        push edx
        push eax                    // [esp] start lo, +4 hi, +8 function, +12 return
        mov eax, dword ptr [esp + 8]
        push ebp
        push ecx
        mov ecx, esi
        fstp dword ptr [esp]
        call eax                    // pops its two arguments
        rdtsc
        sub eax, dword ptr [esp]
        sbb edx, dword ptr [esp + 4]
        push edx
        push eax
        push dword ptr [esp + 16]
        call task_add
        add esp, 12 + 12
        ret
    }
}

const char* steps_on() {
    if (g_steps_on) return "already timed";
    if (!g_game_tid) g_game_tid = GetCurrentThreadId();
    auto* at = reinterpret_cast<uint8_t*>(kStepPatch);
    static const uint8_t kExpect[9] = {0x55, 0x51, 0x8B, 0xCE, 0xD9, 0x1C, 0x24, 0xFF, 0xD0};
    if (std::memcmp(at, kExpect, 9) != 0) return "unexpected bytes";
    DWORD old = 0;
    if (!VirtualProtect(at, 9, PAGE_EXECUTE_READWRITE, &old)) return "could not unprotect";
    uint8_t code[9];
    code[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&step_stub) - (kStepPatch + 5));
    std::memcpy(code + 1, &rel, 4);
    code[5] = 0xEB;  // jmp 0x4557E7
    code[6] = 0x02;
    code[7] = 0x90;
    code[8] = 0x90;
    std::memcpy(at, code, 9);
    DWORD ignored = 0;
    VirtualProtect(at, 9, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 9);
    g_steps_on = true;
    return "timed";
}

// The class name of a method table from the game's run-time type information (vtable[-1] = complete object
// locator, +12 = type descriptor, name at +8, ".?AV...@@").
bool rtti_name_raw(uint32_t vt, char* out, size_t n) {
    __try {
        const uint32_t col = *reinterpret_cast<const uint32_t*>(vt - 4);
        const uint32_t td = *reinterpret_cast<const uint32_t*>(col + 12);
        const char* name = reinterpret_cast<const char*>(td + 8);
        size_t i = 0;
        for (; i + 1 < n && name[i] && i < 80; ++i) out[i] = name[i];
        out[i] = 0;
        return i > 0;
    } __except (1) {
        return false;
    }
}

void report_tasks(double tsc_per_ms, uint32_t frames) {
    if ((!g_tasks_on && !g_steps_on) || frames == 0) return;
    // top 14 by time
    for (int k = 0; k < 14; ++k) {
        int best = -1;
        for (int i = 0; i < 256; ++i) {
            if (g_task_stats[i].vt && g_task_stats[i].ticks && (best < 0 || g_task_stats[i].ticks > g_task_stats[best].ticks)) best = i;
        }
        if (best < 0) break;
        TaskStat& e = g_task_stats[best];
        char name[96] = "?";
        rtti_name_raw(e.vt, name, sizeof(name));
        LOG_INFO("[phases]   task %-60s vt %08X %.3f ms per frame, %.1f calls", name, e.vt,
                 static_cast<double>(e.ticks) / tsc_per_ms / frames, static_cast<double>(e.calls) / frames);
        e.ticks = 0;
        e.calls = 0;
    }
    for (auto& e : g_task_stats) {
        e.ticks = 0;
        e.calls = 0;
    }
}

void report_groups(double tsc_per_ms, uint32_t frames) {
    report_tasks(tsc_per_ms, frames);
    if (!g_groups_on || frames == 0) return;
    char line[512];
    int n = std::snprintf(line, sizeof(line), "[phases] task groups (ms per frame):");
    for (int i = 0; i < 6; ++i) {
        n += std::snprintf(line + n, sizeof(line) - n, " %d:%X %.3f", i, g_group_target[i],
                           static_cast<double>(g_group_ticks[i]) / tsc_per_ms / frames);
        g_group_ticks[i] = 0;
    }
    LOG_INFO("%s", line);
}

}  // namespace

void phases_game_frame() {
    if (!g_active) return;
    ++g_frames;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    if (now.QuadPart - g_qpc0.QuadPart >= 5 * f.QuadPart) report();
}

void phases_frame_ticks(uint64_t ticks) {
    if (g_active) g_frame_ticks += ticks;
}

bool phases_active() { return g_active; }

void phases_render_frame() {
    const uint32_t t = GetCurrentThreadId();
    if (g_render_tid != t) g_render_tid = t;
    if (g_pending_n == 0) return;
    AcquireSRWLockExclusive(&g_sites_lock);
    for (LONG i = 0; i < g_pending_n; ++i) {
        LOG_INFO("[phases] render-thread call at %08X: %s", g_pending_radd[i], add_site(g_pending_radd[i], &g_render_tid));
    }
    g_pending_n = 0;
    ReleaseSRWLockExclusive(&g_sites_lock);
}

const char* phases_command(const char* verb, uint32_t address) {
    if (std::strcmp(verb, "on") == 0) {
        g_stub_tid = GetCurrentThreadId();
        if (!g_active) {
            for (uint32_t a : kFrameSites) add_site(a);
            QueryPerformanceCounter(&g_qpc0);
            g_tsc0 = __rdtsc();
            g_frames = 0;
            g_frame_ticks = 0;
            g_active = true;
            LOG_INFO("[phases] timing %d calls of the frame function", g_count);
        }
        return "on";
    }
    if (std::strcmp(verb, "d3d") == 0) {
        // Every method of the game's Direct3D device on the render thread, but SetSamplerState (69: the filter checks
        // that slot every frame) and the IUnknown three.
        if (!address) return "no device table";
        int n = 0;
        for (int slot = 3; slot < 119; ++slot) {
            if (slot == 69) continue;
            if (std::strcmp(add_vt(address, slot, &g_render_tid), "timed") == 0) ++n;
        }
        LOG_INFO("[phases] Direct3D device table %08X: %d methods timed on the render thread", address, n);
        return n ? "timed" : "none";
    }
    if (std::strcmp(verb, "steps") == 0) {
        const char* r = steps_on();
        LOG_INFO("[phases] step states: %s", r);
        return r;
    }
    if (std::strcmp(verb, "tasks") == 0) {
        const char* r = tasks_on();
        LOG_INFO("[phases] tasks: %s", r);
        return r;
    }
    if (std::strcmp(verb, "groups") == 0) {
        const char* r = groups_on();
        LOG_INFO("[phases] task groups: %s", r);
        return r;
    }
    if (std::strncmp(verb, "vt", 2) == 0 && verb[2] == ':') {
        // phases vt:SLOT VTABLE
        const char* r = add_vt(address, std::atoi(verb + 3));
        LOG_INFO("[phases] method %d of table %08X: %s", std::atoi(verb + 3), address, r);
        return r;
    }
    if (std::strcmp(verb, "radd") == 0) {
        // A call site the render thread runs: patched by the render thread at its next frame start.
        AcquireSRWLockExclusive(&g_sites_lock);
        if (g_pending_n < 64) g_pending_radd[g_pending_n++] = address;
        ReleaseSRWLockExclusive(&g_sites_lock);
        return "queued for the render thread";
    }
    if (std::strcmp(verb, "add") == 0) {
        AcquireSRWLockExclusive(&g_sites_lock);
        const char* r = add_site(address);
        ReleaseSRWLockExclusive(&g_sites_lock);
        LOG_INFO("[phases] call at %08X: %s", address, r);
        return r;
    }
    return "phases on | phases add ADDR";
}
