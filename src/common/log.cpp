#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <share.h>

namespace {
FILE* g_file = nullptr;
std::mutex g_mu;
}  // namespace

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
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_file) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
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
        std::fclose(g_file);
        g_file = nullptr;
    }
}
