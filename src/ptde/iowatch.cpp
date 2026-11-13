#include "iowatch.h"

#include "fpsu_buildcfg.h"
#include "log.h"
#include "profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if FPSU_DIAGNOSTICS

namespace {

constexpr double kSlowMs = 30.0;     // a single read or wait logged one by one
constexpr int kSlowLogged = 300;     // per session
constexpr double kWindowS = 5.0;

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using OverlappedFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using CloseHandleFn = BOOL(WINAPI*)(HANDLE);

CreateFileWFn g_create_w = nullptr;
CreateFileAFn g_create_a = nullptr;
ReadFileFn g_read = nullptr;
OverlappedFn g_overlapped = nullptr;
CloseHandleFn g_close = nullptr;

LARGE_INTEGER g_freq{};
std::mutex g_lock;
std::unordered_map<HANDLE, std::string> g_names;  // open files by handle (base name)

struct ThreadStat {
    DWORD tid = 0;
    int reads = 0;
    double bytes = 0.0;
    double read_ms = 0.0;
    int waits = 0;
    double wait_ms = 0.0;
    double worst_ms = 0.0;
    int opens = 0;
    double open_ms = 0.0;
};
std::vector<ThreadStat> g_stats;
LONGLONG g_window_end = 0;
int g_slow_logged = 0;

double ms_between(LONGLONG a, LONGLONG b) {
    return 1000.0 * static_cast<double>(b - a) / static_cast<double>(g_freq.QuadPart);
}

LONGLONG now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void thread_label(DWORD tid, char* out, size_t n) {
    if (tid == g_game_thread_id) {
        std::snprintf(out, n, "game");
    } else if (tid == g_render_thread_id) {
        std::snprintf(out, n, "render");
    } else {
        std::snprintf(out, n, "thread %lu", tid);
    }
}

ThreadStat& stat_for(DWORD tid) {
    for (ThreadStat& s : g_stats) {
        if (s.tid == tid) return s;
    }
    g_stats.push_back(ThreadStat{});
    g_stats.back().tid = tid;
    return g_stats.back();
}

std::string name_of(HANDLE h) {
    auto it = g_names.find(h);
    return it == g_names.end() ? std::string("?") : it->second;
}

// Called with g_lock held; fills `report` when a window ended.
void maybe_report(LONGLONG t, std::string& report) {
    if (!g_window_end) {
        g_window_end = t + static_cast<LONGLONG>(kWindowS * g_freq.QuadPart);
        return;
    }
    if (t < g_window_end) return;
    g_window_end = t + static_cast<LONGLONG>(kWindowS * g_freq.QuadPart);
    std::sort(g_stats.begin(), g_stats.end(), [](const ThreadStat& a, const ThreadStat& b) {
        return a.read_ms + a.wait_ms + a.open_ms > b.read_ms + b.wait_ms + b.open_ms;
    });
    char line[1200];
    int o = std::snprintf(line, sizeof(line), "[io] last %.0f s:", kWindowS);
    int shown = 0;
    for (const ThreadStat& s : g_stats) {
        if (!s.reads && !s.waits && !s.opens) continue;
        if (shown++ >= 6 || o >= static_cast<int>(sizeof(line)) - 160) break;
        char who[24];
        thread_label(s.tid, who, sizeof(who));
        o += std::snprintf(line + o, sizeof(line) - o,
                           " %s: %d reads %.1f MB %.0f ms, %d waits %.0f ms, %d opens %.0f ms, slowest %.0f ms;", who,
                           s.reads, s.bytes / (1024.0 * 1024.0), s.read_ms, s.waits, s.wait_ms, s.opens, s.open_ms,
                           s.worst_ms);
    }
    if (shown) report = line;
    for (ThreadStat& s : g_stats) {
        const DWORD tid = s.tid;
        s = ThreadStat{};
        s.tid = tid;
    }
}

void note(const char* kind, HANDLE h, double ms, double bytes, uint64_t offset, bool is_read, bool is_wait) {
    const DWORD tid = GetCurrentThreadId();
    std::string report;
    char slow[400] = "";
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ThreadStat& s = stat_for(tid);
        if (is_read) {
            ++s.reads;
            s.bytes += bytes;
            s.read_ms += ms;
        } else if (is_wait) {
            ++s.waits;
            s.wait_ms += ms;
            s.bytes += bytes;
        } else {
            ++s.opens;
            s.open_ms += ms;
        }
        if (ms > s.worst_ms) s.worst_ms = ms;
        if (ms >= kSlowMs && g_slow_logged < kSlowLogged) {
            ++g_slow_logged;
            char who[24];
            thread_label(tid, who, sizeof(who));
            std::snprintf(slow, sizeof(slow), "[io] %s: %s %s took %.1f ms (%.0f KB at offset %llu)", who, kind,
                          name_of(h).c_str(), ms, bytes / 1024.0, static_cast<unsigned long long>(offset));
        }
        maybe_report(now(), report);
    }
    if (slow[0]) LOG_INFO("%s", slow);
    if (!report.empty()) LOG_INFO("%s", report.c_str());
}

std::string base_name_w(LPCWSTR path) {
    if (!path) return "?";
    const wchar_t* b = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') b = p + 1;
    }
    char out[96];
    std::snprintf(out, sizeof(out), "%ls", b);
    return out;
}

std::string base_name_a(LPCSTR path) {
    if (!path) return "?";
    const char* b = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') b = p + 1;
    }
    return std::string(b).substr(0, 95);
}

HANDLE WINAPI hk_create_w(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                          HANDLE tmpl) {
    const LONGLONG a = now();
    HANDLE h = g_create_w(path, access, share, sa, disp, flags, tmpl);
    const double ms = ms_between(a, now());
    const DWORD err = GetLastError();
    if (h != INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_names[h] = base_name_w(path);
    }
    note("open", h, ms, 0.0, 0, false, false);
    SetLastError(err);
    return h;
}

HANDLE WINAPI hk_create_a(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                          HANDLE tmpl) {
    const LONGLONG a = now();
    HANDLE h = g_create_a(path, access, share, sa, disp, flags, tmpl);
    const double ms = ms_between(a, now());
    const DWORD err = GetLastError();
    if (h != INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_names[h] = base_name_a(path);
    }
    note("open", h, ms, 0.0, 0, false, false);
    SetLastError(err);
    return h;
}

BOOL WINAPI hk_read(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    uint64_t offset = 0;
    if (ov) {
        offset = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
    } else {
        LONG high = 0;
        const DWORD low = SetFilePointer(h, 0, &high, FILE_CURRENT);
        if (low != INVALID_SET_FILE_POINTER) offset = (static_cast<uint64_t>(static_cast<DWORD>(high)) << 32) | low;
    }
    const LONGLONG a = now();
    const BOOL ok = g_read(h, buf, n, got, ov);
    const double ms = ms_between(a, now());
    const DWORD err = GetLastError();
    note(ov ? "read (overlapped)" : "read", h, ms, static_cast<double>(n), offset, true, false);
    SetLastError(err);
    return ok;
}

BOOL WINAPI hk_overlapped(HANDLE h, LPOVERLAPPED ov, LPDWORD got, BOOL wait) {
    const LONGLONG a = now();
    const BOOL ok = g_overlapped(h, ov, got, wait);
    const double ms = ms_between(a, now());
    const DWORD err = GetLastError();
    if (wait) {
        const uint64_t offset = ov ? ((static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset) : 0;
        note("wait for a read", h, ms, ok && got ? static_cast<double>(*got) : 0.0, offset, false, true);
    }
    SetLastError(err);
    return ok;
}

BOOL WINAPI hk_close(HANDLE h) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_names.erase(h);
    }
    return g_close(h);
}

// The game's import slot for `dll`!`fn`, or null.
void** import_slot(const char* dll, const char* fn) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<void**>(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), fn) == 0) return &slots[i];
        }
    }
    return nullptr;
}

template <typename Fn>
bool wrap(const char* fn, Fn hook, Fn* original) {
    void** slot = import_slot("KERNEL32.dll", fn);
    if (!slot) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = reinterpret_cast<Fn>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(hook));
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

void iowatch_install() {
    if (g_read) return;
    QueryPerformanceFrequency(&g_freq);
    int n = 0;
    n += wrap("CreateFileW", &hk_create_w, &g_create_w);
    n += wrap("CreateFileA", &hk_create_a, &g_create_a);
    n += wrap("ReadFile", &hk_read, &g_read);
    n += wrap("GetOverlappedResult", &hk_overlapped, &g_overlapped);
    n += wrap("CloseHandle", &hk_close, &g_close);
    LOG_INFO("[io] timing the game's file reads per thread (%d of 5 imports): an [io] line every %.0f s, reads and waits "
             "of %.0f ms or more one by one",
             n, kWindowS, kSlowMs);
}

#else

void iowatch_install() {}

#endif
