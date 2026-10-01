#include "inputdump.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <TlHelp32.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kObjectRva = 0x1CB5420;  // global pointer read by the HUD shortcut handler
constexpr uint32_t kWords = 0x180;          // 0x600 bytes
constexpr size_t kMaxSamples = 8000;

wchar_t g_dir[MAX_PATH] = {};

bool read_words(const void* from, uint32_t* out, uint32_t words) {
    __try {
        std::memcpy(out, from, words * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(out, 0, words * 4);
        return false;
    }
}

void* read_object(const uint8_t* base) {
    __try {
        return *reinterpret_cast<void* const*>(base + kObjectRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void write_capture(int number, const std::vector<uint32_t>& data, const std::vector<double>& ms) {
    const size_t samples = ms.size();
    if (samples < 2) return;
    std::vector<uint32_t> keep;
    for (uint32_t c = 0; c < kWords; ++c) {
        for (size_t s = 1; s < samples; ++s) {
            if (data[s * kWords + c] != data[c]) {
                keep.push_back(c);
                break;
            }
        }
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%sDSR-FPS-Unlock-input-%d.csv", g_dir, number);
    FILE* file = nullptr;
    _wfopen_s(&file, path, L"w");
    if (!file) return;
    std::fprintf(file, "row,ms");
    for (uint32_t c : keep) std::fprintf(file, ",+%03X", c * 4);
    std::fputc(10, file);
    for (size_t s = 0; s < samples; ++s) {
        std::fprintf(file, "%zu,%.3f", s, ms[s]);
        for (uint32_t c : keep) std::fprintf(file, ",%x", data[s * kWords + c]);
        std::fputc(10, file);
    }
    std::fclose(file);
    LOG_INFO("Input dump %d: %zu samples, %zu of %u words changed", number, samples, keep.size(), kWords);
}

// The HUD shortcut handler calls slot 6 of this object's vtable with 0xC or 0xD and acts when the
// answer is 0xF. This wraps that slot to log how the answer changes while a button is held.
using StateFn = int (*)(void* self, int index);
StateFn g_state_orig = nullptr;
int g_state_last[32];
LONG g_state_lines = 0;

int hook_state(void* self, int index) {
    const int result = g_state_orig(self, index);
    if (index >= 0 && index < 32 && g_state_last[index] != result + 1 && g_state_lines < 1500) {
        g_state_last[index] = result + 1;
        InterlockedIncrement(&g_state_lines);
        LOG_INFO("[input] state index=0x%X -> 0x%X", index, result);
    }
    return result;
}

void hook_state_slot(void* object) {
    static bool done = false;
    if (done || !object) return;
    done = true;
    void** vtable = nullptr;
    __try {
        vtable = *reinterpret_cast<void***>(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!vtable) return;
    void** slot = vtable + 6;
    LOG_INFO("[input] state object %p, vtable %p, slot 6 = %p", object, static_cast<void*>(vtable), *slot);
    g_state_orig = reinterpret_cast<StateFn>(*slot);
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = reinterpret_cast<void*>(hook_state);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old, &ignored);
        LOG_INFO("[input] state hook installed");
    }
}

DWORD WINAPI dump_thread(void*) {
    const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    bool recording = false;
    bool key_was = false;
    int capture = 0;
    std::vector<uint32_t> data;
    std::vector<double> ms;
    LARGE_INTEGER t0{};
    for (;;) {
        Sleep(1);
        const bool key = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
        if (key && !key_was) {
            if (recording) {
                recording = false;
                write_capture(++capture, data, ms);
                data.clear();
                ms.clear();
            } else {
                recording = true;
                QueryPerformanceCounter(&t0);
                LOG_INFO("Input dump: recording");
            }
        }
        key_was = key;
        if (!recording) continue;
        void* object = read_object(base);
        if (!object) continue;
        const size_t at = data.size();
        data.resize(at + kWords);
        read_words(object, &data[at], kWords);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        ms.push_back(1000.0 * static_cast<double>(now.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));
        if (ms.size() >= kMaxSamples) {
            recording = false;
            write_capture(++capture, data, ms);
            data.clear();
            ms.clear();
        }
    }
    return 0;
}

// Stamina finder. With the character at full hit points and full stamina, Scroll Lock scans the
// game's memory for the player's stats block as laid out in Prepare to Die Edition (hit points at
// +0xC, their maximum at +0x10, stamina at +0x28 and its maximum at +0x2C, all 32-bit integers)
// and then logs every change of the stamina word, with a millisecond timestamp, for four minutes.
bool plausible_stats(const uint8_t* a) {
    const int hp = *reinterpret_cast<const int*>(a + 0xC);
    const int hp_max = *reinterpret_cast<const int*>(a + 0x10);
    const int stamina = *reinterpret_cast<const int*>(a + 0x28);
    const int stamina_max = *reinterpret_cast<const int*>(a + 0x2C);
    return hp >= 100 && hp <= 6000 && hp == hp_max && stamina_max >= 60 && stamina_max <= 300 &&
           stamina == stamina_max;
}

// Hardware write watchpoint on the stamina word (once the finder knows which block is real).
// The write watchpoint was only needed to find the stamina tick code (now fixed); it slows the game.
constexpr bool kArmWriteWatch = false;

struct WriteHit {
    uintptr_t rip;
    uintptr_t ret[3];
    uintptr_t rdi;
    int value;
    int slot;
};
constexpr int kMaxHits = 4096;
WriteHit g_hits[kMaxHits];
std::atomic<int> g_hit_count{0};
volatile LONG g_watch_address_set = 0;
uintptr_t g_watch_address = 0;      // watchpoint 0: the stats block's stamina word
uintptr_t g_watch_address2 = 0;     // watchpoint 1: the character's own stamina word
volatile LONG g_second_armed = 0;

LONG CALLBACK on_stamina_write(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 3)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    const int which = (ctx->Dr6 & 1) ? 0 : 1;
    const int slot = g_hit_count.fetch_add(1, std::memory_order_relaxed);
    if (slot < kMaxHits) {
        WriteHit& h = g_hits[slot];
        h.rip = static_cast<uintptr_t>(ctx->Rip);
        // The first three words on the stack that look like return addresses into the game's code.
        const uintptr_t* stack = reinterpret_cast<const uintptr_t*>(ctx->Rsp);
        const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        int found = 0;
        for (int i = 0; i < 3; ++i) h.ret[i] = 0;
        for (int i = 0; i < 48 && found < 3; ++i) {
            uintptr_t v = 0;
            __try {
                v = stack[i];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (v > image + 0x1000 && v < image + 0x1100000) {
                h.ret[found++] = v;
            }
        }
        h.rdi = static_cast<uintptr_t>(ctx->Rdi);
        h.slot = which;
        h.value = *reinterpret_cast<const int*>(which == 0 ? g_watch_address : g_watch_address2);
    }
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void arm_stamina_watch(int slot, uintptr_t address) {
    if (slot == 0) {
        g_watch_address = address;
        AddVectoredExceptionHandler(1, on_stamina_write);
    } else {
        g_watch_address2 = address;
    }
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
            HANDLE th = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, e.th32ThreadID);
            if (!th) continue;
            const bool me = e.th32ThreadID == self;
            if (me || SuspendThread(th) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &ctx)) {
                    if (slot == 0) {
                        ctx.Dr0 = address;
                        ctx.Dr7 |= 1u | (1u << 16) | (3u << 18);  // write, 4 bytes
                    } else {
                        ctx.Dr1 = address;
                        ctx.Dr7 |= (1u << 2) | (1u << 20) | (3u << 22);
                    }
                    ctx.Dr6 = 0;
                    if (SetThreadContext(th, &ctx)) ++applied;
                }
                if (!me) ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &e));
    }
    CloseHandle(snap);
    LOG_INFO("[stamW] write watchpoint %d on %p set on %d threads", slot, reinterpret_cast<void*>(address), applied);
}

void scan_region(uintptr_t begin, uintptr_t end, const uint8_t** out, size_t* count, size_t limit) {
    __try {
        for (uintptr_t a = begin; a + 0x40 <= end && *count < limit; a += 4) {
            if (plausible_stats(reinterpret_cast<const uint8_t*>(a))) {
                out[(*count)++] = reinterpret_cast<const uint8_t*>(a);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int read_stamina(const uint8_t* block) {
    __try {
        return *reinterpret_cast<const int*>(block + 0x28);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

DWORD WINAPI stamina_thread(void*) {
    bool key_was = false;
    for (;;) {
        Sleep(20);
        const bool key = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
        const bool pressed = key && !key_was;
        key_was = key;
        if (!pressed) continue;
        static const uint8_t* found[4000];
        size_t count = 0;
        MEMORY_BASIC_INFORMATION info{};
        uintptr_t address = 0x10000;
        while (address < 0x7FFFFFFE0000ull && count < 4000 &&
               VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
            if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && (info.Protect & 0xFF) == PAGE_READWRITE &&
                (info.Protect & PAGE_GUARD) == 0 && info.RegionSize >= 0x100) {
                scan_region(begin, begin + info.RegionSize, found, &count, 4000);
            }
            address = begin + info.RegionSize;
        }
        LOG_INFO("[stamD] scan found %zu candidate stats blocks", count);
        if (count == 0) continue;
        // Most candidates are look-alikes that never change; the real stats block is the one whose
        // stamina word moves while sprinting, so only changes are logged.
        static int last[4000];
        for (int& l : last) l = -1;
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < 240000) {
            Sleep(1);
            {
                static int reported = 0;
                static ULONGLONG last_report = 0;
                const ULONGLONG now = GetTickCount64();
                if (g_watch_address_set && !g_second_armed && g_hit_count.load() > 0) {
                    // The first writer copies the character's stamina into the stats block; the character's
                    // own word (the real source) is at [rdi + 0x3F8] in that copy routine.
                    const int total = g_hit_count.load() < kMaxHits ? g_hit_count.load() : kMaxHits;
                    for (int h = 0; h < total; ++h) {
                        if (g_hits[h].slot == 0 && g_hits[h].rdi > 0x10000 &&
                            InterlockedExchange(&g_second_armed, 1) == 0) {
                            arm_stamina_watch(1, g_hits[h].rdi + 0x3F8);
                            break;
                        }
                    }
                }
                if (g_watch_address_set && now - last_report > 15000 && g_hit_count.load() > reported) {
                    last_report = now;
                    const int total = g_hit_count.load() < kMaxHits ? g_hit_count.load() : kMaxHits;
                    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                    struct Key { int slot; uintptr_t rip, r0, r1; int count, vmin, vmax; } keys[32] = {};
                    int nkeys = 0;
                    for (int h = 0; h < total; ++h) {
                        int k = 0;
                        for (; k < nkeys; ++k) {
                            if (keys[k].slot == g_hits[h].slot && keys[k].rip == g_hits[h].rip && keys[k].r0 == g_hits[h].ret[0]) break;
                        }
                        if (k == nkeys) {
                            if (nkeys == 32) continue;
                            keys[nkeys++] = {g_hits[h].slot, g_hits[h].rip, g_hits[h].ret[0], g_hits[h].ret[1], 0, 1 << 30, -(1 << 30)};
                        }
                        ++keys[k].count;
                        if (g_hits[h].value < keys[k].vmin) keys[k].vmin = g_hits[h].value;
                        if (g_hits[h].value > keys[k].vmax) keys[k].vmax = g_hits[h].value;
                    }
                    reported = g_hit_count.load();
                    LOG_INFO("[stamW] %d writes so far", total);
                    for (int k = 0; k < nkeys; ++k) {
                        LOG_INFO("[stamW]   slot %d writer rva %llX  ret rva %llX %llX  x%d  values %d..%d", keys[k].slot,
                                 static_cast<unsigned long long>(keys[k].rip - base),
                                 static_cast<unsigned long long>(keys[k].r0 - base),
                                 static_cast<unsigned long long>(keys[k].r1 - base), keys[k].count, keys[k].vmin,
                                 keys[k].vmax);
                    }
                }
            }
            for (size_t i = 0; i < count; ++i) {
                const int value = read_stamina(found[i]);
                if (value != last[i]) {
                    if (kArmWriteWatch && !g_watch_address_set && last[i] > 0 && value == last[i] - 1 && last[i] >= 60 && last[i] <= 300 &&
                        InterlockedExchange(&g_watch_address_set, 1) == 0) {
                        arm_stamina_watch(0, reinterpret_cast<uintptr_t>(found[i]) + 0x28);
                    }
                    last[i] = value;
                    LOG_INFO("[stamD] %zu %p stamina=%d", i, static_cast<const void*>(found[i]), value);
                }
            }
        }
        LOG_INFO("[stamD] done");
    }
    return 0;
}

}  // namespace

void inputdump_start(const wchar_t* dll_path) {
    wcsncpy_s(g_dir, dll_path, _TRUNCATE);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    if (HANDLE finder = CreateThread(nullptr, 0, stamina_thread, nullptr, 0, nullptr)) {
        CloseHandle(finder);
        LOG_INFO("Stamina finder: Scroll Lock at full health and stamina starts it");
    }
    HANDLE thread = CreateThread(nullptr, 0, dump_thread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        LOG_INFO("Input dump available: F12 starts and stops a recording");
    }
}
