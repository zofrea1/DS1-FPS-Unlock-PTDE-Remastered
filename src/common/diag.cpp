#include "diag.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

namespace {

wchar_t g_dump_path[MAX_PATH] = {};
std::atomic<int> g_logged{0};
std::atomic<int> g_crash_reported{0};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

uint64_t (*g_steps)() = nullptr;
float (*g_frame_ms)() = nullptr;
void (*g_extra)(char*, unsigned) = nullptr;
void (*g_tick)() = nullptr;

// "module.dll+0x1234" for an address, or "unbacked memory" for code we or the game allocated.
void describe_address(const void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* slash = wcsrchr(path, L'\\');
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, name, sizeof(name), nullptr, nullptr);
        std::snprintf(out, size, "%s+0x%llX", name,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                      reinterpret_cast<uintptr_t>(module)));
    } else {
        std::snprintf(out, size, "unbacked memory");
    }
}

bool fatal_class(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
           code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
}

void describe_exception(const EXCEPTION_POINTERS* info, char* out, size_t size) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    char where[200];
    describe_address(record->ExceptionAddress, where, sizeof(where));
    int n = std::snprintf(out, size, "exception 0x%08lX at %p (%s), thread %lu", record->ExceptionCode,
                          record->ExceptionAddress, where, GetCurrentThreadId());
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2) {
        n += std::snprintf(out + n, size - n, ", %s address %p",
                           record->ExceptionInformation[0] == 0 ? "reading" : (record->ExceptionInformation[0] == 1 ? "writing" : "executing"),
                           reinterpret_cast<void*>(record->ExceptionInformation[1]));
    }
}

bool in_this_module(const void* address) {
    HMODULE module = nullptr, self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&in_this_module), &self);
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) &&
           module && module == self;
}

// The return address of the faulting function's frame. Our guarded reads keep an EBP frame (they use __try), so
// this names the code that passed the bad address rather than the read helper. 0 where it cannot be told.
uintptr_t caller_of(const CONTEXT* ctx) {
#if defined(_M_IX86)
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t ebp = ctx->Ebp;
    const uintptr_t low = reinterpret_cast<uintptr_t>(tib->StackLimit);
    const uintptr_t high = reinterpret_cast<uintptr_t>(tib->StackBase);
    if (ebp < low || ebp + 8 > high || (ebp & 3) != 0) return 0;
    return reinterpret_cast<const uintptr_t*>(ebp)[1];
#else
    (void)ctx;
    return 0;
#endif
}

// Faults inside this DLL are counted per place (fault address and caller): each new place is logged, and again
// at 10, 100, 1000 and 10000. The game's own faults keep the old limit of 25 lines.
struct FaultSite {
    uintptr_t ip = 0, caller = 0;
    long count = 0;
};
constexpr int kMaxSites = 128;
FaultSite g_sites[kMaxSites];
std::atomic<int> g_site_count{0};
std::atomic<int> g_site_lock{0};
std::atomic<bool> g_sites_full{false};

LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !fatal_class(info->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The watchpoint tooling raises single-step traps on purpose; those are not in this class.
    const void* ip = info->ExceptionRecord->ExceptionAddress;
    if (in_this_module(ip) && info->ContextRecord) {
        const uintptr_t caller = caller_of(info->ContextRecord);
        long count = 0;
        bool known = false;
        while (g_site_lock.exchange(1) != 0) {
        }
        const int n = g_site_count.load();
        for (int i = 0; i < n; ++i) {
            if (g_sites[i].ip == reinterpret_cast<uintptr_t>(ip) && g_sites[i].caller == caller) {
                count = ++g_sites[i].count;
                known = true;
                break;
            }
        }
        bool full_now = false;
        if (!known && n < kMaxSites) {
            g_sites[n] = FaultSite{reinterpret_cast<uintptr_t>(ip), caller, 1};
            g_site_count.store(n + 1);
            count = 1;
        } else if (!known) {
            full_now = !g_sites_full.exchange(true);
        }
        g_site_lock.store(0);
        if (full_now) {
            // this one is still logged; later new places are not
            LOG_INFO("First-chance faults: %d places in this DLL are counted; new places from here on are not logged",
                     kMaxSites);
            count = 1;
        }
        if (count == 1 || count == 10 || count == 100 || count == 1000 || count == 10000) {
            char text[512], from[128] = "unknown";
            describe_exception(info, text, sizeof(text));
            if (caller) describe_address(reinterpret_cast<const void*>(caller), from, sizeof(from));
            LOG_INFO("First-chance %s, called from %s (%ld time(s) here; the game may handle this itself)", text,
                     from, count);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_logged.fetch_add(1) >= 25) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char text[512];
    describe_exception(info, text, sizeof(text));
    LOG_INFO("First-chance %s (the game may handle this itself)", text);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
    if (g_crash_reported.exchange(1) == 0 && info && info->ExceptionRecord) {
        char text[512];
        describe_exception(info, text, sizeof(text));
        LOG_ERROR("CRASH: unhandled %s", text);
        if (g_dump_path[0]) {
            HANDLE file = CreateFileW(g_dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = info;
                mei.ClientPointers = FALSE;
                const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                                  static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                                                  &mei, nullptr, nullptr);
                CloseHandle(file);
                LOG_ERROR("CRASH: minidump %s", ok ? "written next to the DLL" : "could not be written");
            }
        }
    }
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI watchdog(void*) {
    uint64_t last = g_steps ? g_steps() : 0;
    uint64_t heartbeat_base = last;
    int quiet_seconds = 0;
    int stall_lines = 0;
    bool stalled = false;
    int since_heartbeat = 0;
    for (;;) {
        Sleep(1000);
        ++since_heartbeat;
        if (g_tick) {
            g_tick();
        }
        const uint64_t now = g_steps ? g_steps() : 0;
        if (now == 0) {
            since_heartbeat = 0;  // nothing is being stepped yet (startup, menus before the hook is active)
            continue;
        }
        if (heartbeat_base == 0) {
            heartbeat_base = now;
        }
        if (now == last) {
            ++quiet_seconds;
            if (quiet_seconds >= 6 && (quiet_seconds == 6 || quiet_seconds % 15 == 0) && stall_lines < 12) {
                ++stall_lines;
                stalled = true;
                LOG_ERROR("STALL: no simulation step for %d s (counter stuck at %llu; loading screens also cause this)", quiet_seconds,
                          static_cast<unsigned long long>(now));
            }
        } else {
            if (stalled) {
                LOG_INFO("Simulation resumed after %d s without a step", quiet_seconds);
                stalled = false;
            }
            quiet_seconds = 0;
            last = now;
        }
        if (since_heartbeat >= 30) {
            const uint64_t steps = now - heartbeat_base;
            if (steps > 0 || !stalled) {
                char extra[320] = {};
                if (g_extra) {
                    g_extra(extra, sizeof(extra));
                }
                LOG_INFO("Heartbeat: %llu steps in %d s (%.1f per second), frame time %.2f ms%s%s",
                         static_cast<unsigned long long>(steps), since_heartbeat,
                         static_cast<double>(steps) / since_heartbeat, g_frame_ms ? g_frame_ms() : 0.0f,
                         extra[0] ? ", " : "", extra);
            }
            heartbeat_base = now;
            since_heartbeat = 0;
        }
    }
    return 0;
}

}  // namespace

void diag_install(const wchar_t* dll_path, const wchar_t* dump_name) {
    wchar_t dir[MAX_PATH];
    lstrcpynW(dir, dll_path, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) {
        slash[1] = 0;
    } else {
        dir[0] = 0;
    }
    _snwprintf_s(g_dump_path, _TRUNCATE, L"%s%s", dir, dump_name);
    AddVectoredExceptionHandler(0, vectored_handler);
    g_previous_filter = SetUnhandledExceptionFilter(unhandled_filter);
}

void diag_watchdog_start(uint64_t (*steps)(), float (*frame_ms)(), void (*extra)(char* out, unsigned size),
                         void (*tick)()) {
    g_steps = steps;
    g_frame_ms = frame_ms;
    g_extra = extra;
    g_tick = tick;
    HANDLE thread = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    if (thread) {
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(thread);
    }
}
