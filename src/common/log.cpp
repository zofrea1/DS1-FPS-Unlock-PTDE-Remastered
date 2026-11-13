#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>
#include <string>

namespace {
FILE* g_file = nullptr;
std::mutex g_mu;       // the file
std::mutex g_pend_mu;  // g_pending
std::string g_pending;
volatile bool g_buffered = false;

// Under g_mu.
void write_pending() {
    std::string out;
    {
        std::lock_guard<std::mutex> lock(g_pend_mu);
        out.swap(g_pending);
    }
    if (!out.empty() && g_file) std::fwrite(out.data(), 1, out.size(), g_file);
}

DWORD WINAPI flusher(void*) {
    for (;;) {
        Sleep(250);
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_file) continue;
        write_pending();
        std::fflush(g_file);
    }
}
}  // namespace

void log_set_buffered(bool on) {
    static bool started = false;
    if (on && !started) {
        started = true;
        HANDLE h = CreateThread(nullptr, 0, flusher, nullptr, 0, nullptr);
        if (!h) return;
        CloseHandle(h);
    }
    g_buffered = on;
}

void log_init(const wchar_t* dll_path, const wchar_t* file_name) {
    wchar_t path[MAX_PATH];
    lstrcpynW(path, dll_path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) {
        slash[1] = 0;
    } else {
        path[0] = 0;
    }
    wchar_t full[MAX_PATH];
    _snwprintf_s(full, _TRUNCATE, L"%s%s", path, file_name);
    // Keep the previous run's log next to this one.
    wchar_t previous[MAX_PATH];
    _snwprintf_s(previous, _TRUNCATE, L"%s.prev", full);
    MoveFileExW(full, previous, MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(full, L"w", _SH_DENYNO);
}

void log_write(const char* level, const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (g_buffered && std::strcmp(level, "error") != 0) {
        char text[4096];
        int n = std::snprintf(text, sizeof(text), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] ", st.wYear, st.wMonth,
                              st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
        va_list ap;
        va_start(ap, fmt);
        const int m = std::vsnprintf(text + n, sizeof(text) - n, fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> lock(g_pend_mu);
        if (m >= 0 && n + m < static_cast<int>(sizeof(text))) {
            g_pending.append(text, n + m);
        } else {
            // Longer than the buffer: formatted again at its full length.
            g_pending.append(text, n);
            va_start(ap, fmt);
            const int need = std::vsnprintf(nullptr, 0, fmt, ap);
            va_end(ap);
            if (need > 0) {
                std::string big(static_cast<size_t>(need) + 1, '\0');
                va_start(ap, fmt);
                std::vsnprintf(&big[0], big.size(), fmt, ap);
                va_end(ap);
                g_pending.append(big.data(), static_cast<size_t>(need));
            }
        }
        g_pending.push_back('\n');
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_file) {
        return;
    }
    write_pending();
    std::fprintf(g_file, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] ", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_file, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

void log_close() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) {
        write_pending();
        std::fclose(g_file);
        g_file = nullptr;
    }
}
