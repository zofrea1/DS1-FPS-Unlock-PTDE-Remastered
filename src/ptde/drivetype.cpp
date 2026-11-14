#include "drivetype.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace {

using GetDriveTypeFn = UINT(WINAPI*)(LPCWSTR);
GetDriveTypeFn g_real = nullptr;
bool g_cache = false;
bool g_measure = false;

// Up to 16 roots ("C:\", "\\server\share\", ...), lower case. Filled under g_lock; read without it once published.
constexpr int kRoots = 16;
struct Entry {
    wchar_t root[64];
    UINT type;
};
Entry g_roots[kRoots];
volatile LONG g_root_n = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

LARGE_INTEGER g_freq{};
volatile LONG g_calls = 0, g_asked = 0;
volatile LONG64 g_asked_ticks = 0, g_max_ticks = 0;
volatile LONG64 g_next_report = 0;

// The root of `path` as GetDriveTypeW would take it: "X:\" for a drive letter path, the path itself otherwise
// (lower case, at most 63 characters). False when it cannot be keyed (null, too long).
bool root_key(LPCWSTR path, wchar_t* out) {
    if (!path) return false;
    size_t n = 0;
    if (path[0] && path[1] == L':') {
        out[0] = static_cast<wchar_t>(std::towlower(path[0]));
        out[1] = L':';
        out[2] = L'\\';
        out[3] = 0;
        return true;
    }
    for (; path[n] && n < 63; ++n) out[n] = static_cast<wchar_t>(std::towlower(path[n]));
    if (path[n]) return false;
    out[n] = 0;
    return true;
}

bool lookup(const wchar_t* key, UINT* type) {
    // An acquire read of the count (the publisher increments it after the entry, with a barrier): under a translator
    // that does not keep x86's store order (Box64, FEX on the phones), a plain read could see the new count before
    // the entry's type and answer DRIVE_UNKNOWN.
    const LONG n = InterlockedCompareExchange(&g_root_n, 0, 0);
    for (LONG i = 0; i < n; ++i) {
        if (std::wcscmp(g_roots[i].root, key) == 0) {
            *type = g_roots[i].type;
            return true;
        }
    }
    return false;
}

void report(LONG64 now) {
    const LONG64 due = g_next_report;
    if (!due) {
        InterlockedCompareExchange64(&g_next_report, now + g_freq.QuadPart * 10, 0);
        return;
    }
    if (now < due || InterlockedCompareExchange64(&g_next_report, now + g_freq.QuadPart * 10, due) != due) return;
    const LONG calls = InterlockedExchange(&g_calls, 0);
    const LONG asked = InterlockedExchange(&g_asked, 0);
    const LONG64 ticks = InterlockedExchange64(&g_asked_ticks, 0);
    const LONG64 worst = InterlockedExchange64(&g_max_ticks, 0);
    if (!calls) return;
    const double ms = 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq.QuadPart);
    LOG_INFO("[drive] last 10 s: %ld GetDriveTypeW calls by the game, %ld asked Windows (%.1f ms in all, longest %.1f "
             "ms), %ld from the cache",
             calls, asked, ms, 1000.0 * static_cast<double>(worst) / static_cast<double>(g_freq.QuadPart),
             calls - asked);
}

UINT WINAPI hk_get_drive_type(LPCWSTR path) {
    if (g_measure) InterlockedIncrement(&g_calls);
    wchar_t key[64];
    const bool keyed = g_cache && root_key(path, key);
    UINT type = 0;
    if (keyed && lookup(key, &type)) {
        if (g_measure) {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            report(t.QuadPart);
        }
        return type;
    }
    LARGE_INTEGER a, b;
    if (g_measure) QueryPerformanceCounter(&a);
    type = g_real(path);
    const DWORD err = GetLastError();
    if (g_measure) {
        QueryPerformanceCounter(&b);
        const LONG64 d = b.QuadPart - a.QuadPart;
        InterlockedIncrement(&g_asked);
        InterlockedExchangeAdd64(&g_asked_ticks, d);
        for (LONG64 m = g_max_ticks; d > m; m = g_max_ticks) {
            if (InterlockedCompareExchange64(&g_max_ticks, d, m) == m) break;
        }
        report(b.QuadPart);
    }
    // Only real answers are kept (an unknown or missing root may appear later).
    if (keyed && type != DRIVE_UNKNOWN && type != DRIVE_NO_ROOT_DIR) {
        AcquireSRWLockExclusive(&g_lock);
        UINT existing = 0;
        if (!lookup(key, &existing) && g_root_n < kRoots) {
            Entry& e = g_roots[g_root_n];
            wcscpy_s(e.root, key);
            e.type = type;
            MemoryBarrier();
            InterlockedIncrement(&g_root_n);
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    SetLastError(err);
    return type;
}

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

}  // namespace

void drivetype_install(bool cache, bool measure) {
    if (!cache && !measure) {
        LOG_INFO("CacheDriveType: false (the game asks Windows for the drive type of every file it loads)");
        return;
    }
    QueryPerformanceFrequency(&g_freq);
    g_cache = cache;
    g_measure = measure;
    void** slot = import_slot("KERNEL32.dll", "GetDriveTypeW");
    DWORD old = 0;
    if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG_ERROR("CacheDriveType: the game's GetDriveTypeW import was not found; left alone");
        return;
    }
    g_real = reinterpret_cast<GetDriveTypeFn>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hk_get_drive_type));
    VirtualProtect(slot, sizeof(void*), old, &old);
    if (cache) {
        LOG_INFO("CacheDriveType: the loader's drive type question (GetDriveTypeW, asked for every file while it holds "
                 "the locks the game waits on) is answered once per drive%s",
                 measure ? "; [drive] lines every 10 s" : "");
    } else {
        LOG_INFO("CacheDriveType: false; the game's GetDriveTypeW calls are timed ([drive] lines every 10 s)");
    }
}
