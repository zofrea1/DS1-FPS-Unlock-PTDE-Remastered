#include "steamguard.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstring>

namespace {

constexpr double kSlowMs = 4.0;        // a Steam answer this slow is the pause (Steam answers in well under 1 ms)
constexpr double kPauseMs = 25.0;      // the longest pause expected (15-20 ms seen)
constexpr double kLeadMs = 30.0;       // the gate closes this long before the earliest the pause could start
constexpr double kTailMs = 40.0;       // and opens this long after the latest it was due, when it never showed
constexpr int kIntervals = 6;
constexpr double kSpreadS = 0.12;      // the intervals learned from may differ by this much (5.00-5.05 s seen)

volatile LONG g_closed = 0;
volatile LONGLONG g_close_at = 0, g_open_at = 0;  // the latest closed span (qpc)
LONGLONG g_freq = 0;

SRWLOCK g_notes_lock = SRWLOCK_INIT;
struct Note {
    LONGLONG end;
    double ms;
};
Note g_notes[16];
int g_notes_n = 0;

using RunCallbacksFn = void(__cdecl*)();
RunCallbacksFn g_run_callbacks = nullptr;
volatile LONG g_skipped = 0;    // the game's SteamAPI_RunCallbacks calls skipped while closed
volatile LONG g_open_hits = 0;  // slow Steam answers to calls that started while the gate was open

LONGLONG now_qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double ms_of(LONGLONG ticks) { return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq); }
LONGLONG ticks_of_ms(double ms) { return static_cast<LONGLONG>(ms * static_cast<double>(g_freq) / 1000.0); }

void __cdecl hk_run_callbacks() {
    if (g_closed) {
        InterlockedIncrement(&g_skipped);
        return;
    }
    const LONGLONG t0 = now_qpc();
    g_run_callbacks();
    const LONGLONG t1 = now_qpc();
    steamguard_note(ms_of(t1 - t0), t1);
}

// The game's import-table slot for steam_api.dll!SteamAPI_RunCallbacks.
void** run_callbacks_slot() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), "steam_api.dll") != 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* slots = reinterpret_cast<void**>(base + desc->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "SteamAPI_RunCallbacks") == 0) return &slots[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------- the probe thread
using UtilsFn = void*(__cdecl*)();
using ServerTimeFn = uint32_t(__thiscall*)(void*);

struct Learner {
    LONGLONG last_end = 0;
    double intervals[kIntervals] = {};
    int n = 0;
    bool learned = false;
    double period = 0.0, lo = 0.0, hi = 0.0;  // seconds

    void recompute() {
        learned = false;
        if (n < 2) return;
        const int use = n < kIntervals ? n : kIntervals;
        double mn = 1e9, mx = 0.0, sum = 0.0;
        for (int i = 0; i < use; ++i) {
            const double v = intervals[(n - 1 - i) % kIntervals];
            mn = v < mn ? v : mn;
            mx = v > mx ? v : mx;
            sum += v;
        }
        if (mx - mn > kSpreadS) return;
        learned = true;
        period = sum / use;
        lo = mn;
        hi = mx;
    }

    // A pause seen ending at `end`. Returns false when it does not fit the rhythm learned.
    bool event(LONGLONG end) {
        if (last_end && end - last_end < g_freq / 2) {  // the same pause, seen twice
            if (end > last_end) last_end = end;
            return true;
        }
        bool fits = true;
        if (last_end) {
            const double gap = static_cast<double>(end - last_end) / static_cast<double>(g_freq);
            if (learned) {
                const int k = static_cast<int>(std::floor(gap / period + 0.5));
                fits = k >= 1 && k <= 6 && std::fabs(gap - k * period) <= 0.08 * k;
                if (fits) intervals[n++ % kIntervals] = gap / k;
            } else if (gap >= 1.0 && gap <= 30.0) {
                intervals[n++ % kIntervals] = gap;
            } else {
                n = 0;
            }
        }
        if (fits || !learned) last_end = end;
        const bool was = learned;
        recompute();
        if (was && !learned) n = 0;
        return fits;
    }
};

bool take_note(Note* out) {
    AcquireSRWLockExclusive(&g_notes_lock);
    const bool any = g_notes_n > 0;
    if (any) {
        *out = g_notes[0];
        std::memmove(g_notes, g_notes + 1, sizeof(Note) * (g_notes_n - 1));
        --g_notes_n;
    }
    ReleaseSRWLockExclusive(&g_notes_lock);
    return any;
}

DWORD WINAPI guard_thread(void*) {
    // Steam's utils interface, once the game has started the Steam API.
    UtilsFn get_utils = nullptr;
    void* utils = nullptr;
    for (int i = 0; !utils; ++i) {
        Sleep(500);
        if (i > 600) return 0;
        HMODULE api = GetModuleHandleW(L"steam_api.dll");
        if (!api) continue;
        get_utils = reinterpret_cast<UtilsFn>(GetProcAddress(api, "SteamUtils"));
        if (!get_utils) return 0;
        utils = get_utils();
    }
    // One question to Steam, timed; returns the time it took (ms) and its end.
    auto probe = [&](LONGLONG* end) -> double {
        void* u = get_utils();
        const LONGLONG t0 = now_qpc();
        if (u) {
            __try {
                reinterpret_cast<ServerTimeFn>((*reinterpret_cast<void***>(u))[3])(u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        *end = now_qpc();
        return ms_of(*end - t0);
    };
    Learner L;
    const LONGLONG start = now_qpc();
    LONGLONG next_stats = start + g_freq * 60;
    int seen = 0, missed = 0;
    bool told_none = false;
    Note note{};
    for (;;) {
        if (!L.learned) {
            // Learning: ask every 10 ms (every 100 ms after a minute without a steady pause).
            const bool long_search = now_qpc() - start > g_freq * 60;
            Sleep(long_search ? 100 : 10);
            LONGLONG end = 0;
            const double ms = probe(&end);
            if (ms >= kSlowMs) L.event(end);
            while (take_note(&note)) {
                if (note.ms >= kSlowMs) L.event(note.end);
            }
            if (L.learned) {
                LOG_INFO("SteamPauseGuard: Steam pauses every %.2f s (%.1f ms seen): on Windows it checks Wi-Fi on "
                         "the thread that answers games. The game's Steam calls now wait out each pause instead of "
                         "stalling a frame.", L.period, ms);
            } else if (long_search && !told_none) {
                told_none = true;
                LOG_INFO("SteamPauseGuard: no regular Steam pause found in a minute; still listening, less often");
            }
            continue;
        }
        // Predicted: sleep until shortly before the pause can start. A slow answer noted meanwhile (the pause came
        // early and hit a call) moves the schedule; the times are worked out again from it.
        const LONGLONG last_before = L.last_end;
        const LONGLONG due_lo = L.last_end + static_cast<LONGLONG>(L.lo * g_freq) - ticks_of_ms(kPauseMs + kLeadMs);
        const LONGLONG due_hi = L.last_end + static_cast<LONGLONG>(L.hi * g_freq) + ticks_of_ms(kTailMs);
        for (LONGLONG now = now_qpc(); now < due_lo && L.learned && L.last_end == last_before; now = now_qpc()) {
            const double left = ms_of(due_lo - now);
            Sleep(left > 20.0 ? 20 : (left > 2.0 ? static_cast<DWORD>(left) - 1 : 1));
            while (take_note(&note)) {
                if (note.ms >= kSlowMs) L.event(note.end);
            }
        }
        if (!L.learned || L.last_end != last_before) continue;
        g_close_at = now_qpc();
        InterlockedExchange(&g_closed, 1);
        bool got = false;
        while (now_qpc() < due_hi && !got) {
            LONGLONG end = 0;
            if (probe(&end) >= kSlowMs) got = L.event(end) || true;
            while (take_note(&note)) {
                if (note.ms >= kSlowMs) got = L.event(note.end) || got;
            }
            if (!got) Sleep(1);
        }
        InterlockedExchange(&g_closed, 0);
        g_open_at = now_qpc();
        if (got) {
            ++seen;
            missed = 0;
        } else if (++missed >= 3) {
            LOG_INFO("SteamPauseGuard: Steam's pause stopped coming on time; listening for it again");
            L = Learner{};
        } else {
            L.last_end += static_cast<LONGLONG>(L.period * g_freq);
        }
#if FPSU_DIAGNOSTICS
        if (now_qpc() >= next_stats) {
            next_stats = now_qpc() + g_freq * 60;
            LOG_INFO("SteamPauseGuard: in the last minute %d pauses waited out (period %.3f s, %.3f-%.3f), the game's "
                     "Steam callbacks skipped %ld times, %ld slow Steam answers to calls made while the gate was open",
                     seen, L.period, L.lo, L.hi, InterlockedExchange(&g_skipped, 0),
                     InterlockedExchange(&g_open_hits, 0));
            seen = 0;
        }
#else
        (void)next_stats;
#endif
    }
}

}  // namespace

void steamguard_start(bool enabled, bool hook_run_callbacks) {
    if (!enabled) {
        LOG_INFO("SteamPauseGuard: off");
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        LOG_INFO("SteamPauseGuard: not needed under Proton / Wine (the Steam Deck's Steam has no such pause)");
        return;
    }
    if (hook_run_callbacks) {
        void** slot = run_callbacks_slot();
        DWORD old = 0;
        if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            LOG_ERROR("SteamPauseGuard: the game's SteamAPI_RunCallbacks import was not found; off");
            return;
        }
        g_run_callbacks = reinterpret_cast<RunCallbacksFn>(*slot);
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hk_run_callbacks));
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old, &ignored);
    }
    if (HANDLE t = CreateThread(nullptr, 0, guard_thread, nullptr, 0, nullptr)) {
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(t);
        LOG_INFO("SteamPauseGuard: listening for a regular pause in Steam's answers");
    }
}

bool steamguard_closed() { return g_closed != 0; }

void steamguard_note(double ms, int64_t end_qpc) {
    if (!g_freq || ms < kSlowMs) return;
    const LONGLONG start = end_qpc - ticks_of_ms(ms);
    if (!(start >= g_close_at && start <= g_open_at) && !(g_closed && start >= g_close_at))
        InterlockedIncrement(&g_open_hits);
    AcquireSRWLockExclusive(&g_notes_lock);
    if (g_notes_n < 16) g_notes[g_notes_n++] = Note{end_qpc, ms};
    ReleaseSRWLockExclusive(&g_notes_lock);
}
