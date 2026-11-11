#include "steamguard.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr double kCanaryEveryMs = 10.0;  // the canary asks Steam this often
constexpr double kGoWindowMs = 1.5;      // every-frame calls go ahead this long after a quick answer
constexpr double kWaitedMs = 50.0;       // a call that has waited this long gets a wider window ...
constexpr double kWideWindowMs = 5.0;    // ... (a whole frame at 200 FPS, still before the next question)
constexpr double kNoQuickMs = 500.0;     // no quick answer this long (Steam slow everywhere, or the canary gone):
constexpr double kStarveMs = 150.0;      // calls go ahead this often when no question is stuck
constexpr double kStuckMs = 1.0;         // a canary question out this long: Steam is in a pause
constexpr double kSlowMs = 4.0;          // for the log

volatile LONG g_running = 0;
LONGLONG g_freq = 0;
volatile LONGLONG g_last_quick = 0;  // the end of the canary's latest quick answer
volatile LONGLONG g_canary_start = 0;
volatile LONG g_canary_out = 0;

// Counters for the diagnostics log.
volatile LONG g_calls_go = 0, g_calls_starved = 0, g_calls_held = 0, g_slow_notes = 0;

using RunCallbacksFn = void(__cdecl*)();
RunCallbacksFn g_run_callbacks = nullptr;
using AccessorFn = void*(__cdecl*)();
AccessorFn g_matchmaking = nullptr;
using DistanceFilterFn = void(__fastcall*)(void*, void*, int);
using RequestLobbyListFn = uint64_t(__fastcall*)(void*, void*);
DistanceFilterFn g_distance_orig = nullptr;
RequestLobbyListFn g_request_orig = nullptr;  // standalone only
volatile LONG g_distance_pending = 0;
volatile LONG g_distance_value = 0;
void** g_own_vtable = nullptr;  // standalone only

LONGLONG now_qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double ms_of(LONGLONG ticks) { return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq); }
LONGLONG ticks_of_ms(double ms) { return static_cast<LONGLONG>(ms * static_cast<double>(g_freq) / 1000.0); }

// ------------------------------------------------------------------------------------------ the game's imports
void __cdecl hk_run_callbacks() {
    static int64_t last = 0;
    if (!steamguard_frame_call(&last)) return;  // Steam keeps the callbacks for a later frame
    const LONGLONG t0 = now_qpc();
    g_run_callbacks();
    const LONGLONG t1 = now_qpc();
    steamguard_note(ms_of(t1 - t0), t1);
}

void __fastcall hk_distance_filter(void*, void*, int value) {
    InterlockedExchange(&g_distance_value, value);
    InterlockedExchange(&g_distance_pending, 1);
}

uint64_t __fastcall hk_request_lobby_list(void* self, void*) {
    steamguard_before_lobby_request(self);
    return g_request_orig(self, nullptr);
}

// The game's Steam matchmaking object gets a vtable copy with the two methods above (the FPS Unlock on its own).
void* __cdecl hk_matchmaking() {
    void* mm = g_matchmaking();
    if (mm && !g_own_vtable) {
        constexpr int kSlots = 38;  // ISteamMatchmaking009
        void** vt = *static_cast<void***>(mm);
        void** copy = static_cast<void**>(VirtualAlloc(nullptr, kSlots * sizeof(void*), MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_READWRITE));
        if (copy) {
            std::memcpy(copy, vt, kSlots * sizeof(void*));
            g_request_orig = reinterpret_cast<RequestLobbyListFn>(vt[4]);
            copy[4] = reinterpret_cast<void*>(&hk_request_lobby_list);
            steamguard_take_distance_filter(copy);
            g_own_vtable = copy;
            *static_cast<void***>(mm) = copy;
        }
    }
    return mm;
}

void** import_slot(const char* dll, const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* slots = reinterpret_cast<void**>(base + desc->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) == 0) return &slots[i];
        }
    }
    return nullptr;
}

bool patch_slot(void** slot, void* hook, void** orig) {
    DWORD old = 0;
    if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *slot;
    InterlockedExchangePointer(slot, hook);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

// ---------------------------------------------------------------------------------------------- the canary
using UtilsFn = void*(__cdecl*)();
using ServerTimeFn = uint32_t(__thiscall*)(void*);

void ask(void* u) {
    __try {
        reinterpret_cast<ServerTimeFn>((*reinterpret_cast<void***>(u))[3])(u);  // ISteamUtils::GetServerRealTime
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

DWORD WINAPI canary_thread(void*) {
    UtilsFn get_utils = nullptr;
    for (int i = 0; !get_utils || !get_utils(); ++i) {  // once the game has started the Steam API
        Sleep(500);
        if (i > 600) return 0;
        HMODULE api = GetModuleHandleW(L"steam_api.dll");
        if (api) get_utils = reinterpret_cast<UtilsFn>(GetProcAddress(api, "SteamUtils"));
        if (api && !get_utils) return 0;
    }
    InterlockedExchange(&g_running, 1);
    double recent[32] = {};
    int n = 0, slow = 0;
    double worst = 0.0;
    LONGLONG next_stats = now_qpc() + g_freq * 60;
    for (;;) {
        void* u = get_utils();
        const LONGLONG t0 = now_qpc();
        if (u) {
            g_canary_start = t0;
            InterlockedExchange(&g_canary_out, 1);
            ask(u);
            InterlockedExchange(&g_canary_out, 0);
        }
        const LONGLONG t1 = now_qpc();
        const double ms = ms_of(t1 - t0);
        // Quick: under 1 ms, or under 3x the usual answer where Steam is slower everywhere.
        double sorted[32];
        const int m = n < 32 ? n : 32;
        std::memcpy(sorted, recent, sizeof(double) * m);
        std::sort(sorted, sorted + m);
        const double usual = m ? sorted[m / 2] : 0.1;
        recent[n++ % 32] = ms;
        if (u && ms <= (std::max)(kStuckMs, 3.0 * usual)) g_last_quick = t1;
        if (ms >= kSlowMs) {
            ++slow;
            worst = ms > worst ? ms : worst;
        }
#if FPSU_DIAGNOSTICS
        if (t1 >= next_stats) {
            next_stats = t1 + g_freq * 60;
            LOG_INFO("SteamPauseGuard: last minute: Steam answered the canary slowly %d times (longest %.1f ms, usual "
                     "%.3f ms); every-frame Steam calls: %ld right after a quick answer, %ld with none for %.0f ms, "
                     "%ld held back; %ld slow Steam answers to calls made elsewhere",
                     slow, worst, usual, InterlockedExchange(&g_calls_go, 0), InterlockedExchange(&g_calls_starved, 0),
                     kNoQuickMs, InterlockedExchange(&g_calls_held, 0), InterlockedExchange(&g_slow_notes, 0));
            slow = 0;
            worst = 0.0;
        }
#else
        (void)next_stats;
#endif
        const double rest = kCanaryEveryMs - ms_of(now_qpc() - t0);
        Sleep(rest >= 1.0 ? static_cast<DWORD>(rest) : 1);
    }
}

}  // namespace

void steamguard_start(bool enabled, bool hook_game_imports) {
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
    if (hook_game_imports) {
        void* orig = nullptr;
        if (patch_slot(import_slot("steam_api.dll", "SteamAPI_RunCallbacks"), reinterpret_cast<void*>(&hk_run_callbacks),
                       &orig)) {
            g_run_callbacks = reinterpret_cast<RunCallbacksFn>(orig);
        } else {
            LOG_ERROR("SteamPauseGuard: the game's SteamAPI_RunCallbacks import was not found; off");
            return;
        }
        if (patch_slot(import_slot("steam_api.dll", "SteamMatchmaking"), reinterpret_cast<void*>(&hk_matchmaking),
                       &orig)) {
            g_matchmaking = reinterpret_cast<AccessorFn>(orig);
        }
    }
    if (HANDLE t = CreateThread(nullptr, 0, canary_thread, nullptr, 0, nullptr)) {
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(t);
        LOG_INFO("SteamPauseGuard: on (every-frame Steam calls go right after a quick answer from Steam)");
    }
}

bool steamguard_frame_call(int64_t* last) {
    if (!g_running) return true;
    const LONGLONG now = now_qpc();
    const LONGLONG since_quick = now - g_last_quick;
    const bool waited = now - *last >= ticks_of_ms(kWaitedMs);
    bool go = since_quick >= 0 && since_quick <= ticks_of_ms(waited ? kWideWindowMs : kGoWindowMs);
    if (go) {
        InterlockedIncrement(&g_calls_go);
    } else if (!steamguard_paused() && since_quick >= ticks_of_ms(kNoQuickMs) && now - *last >= ticks_of_ms(kStarveMs)) {
        go = true;
        InterlockedIncrement(&g_calls_starved);
    } else {
        InterlockedIncrement(&g_calls_held);
    }
    if (go) *last = now;
    return go;
}

bool steamguard_paused() {
    return g_running && g_canary_out && now_qpc() - g_canary_start >= ticks_of_ms(kStuckMs);
}

void steamguard_note(double ms, int64_t end_qpc) {
    (void)end_qpc;
    if (g_freq && ms >= kSlowMs) InterlockedIncrement(&g_slow_notes);
}

void steamguard_take_distance_filter(void** vtable_copy) {
    if (!vtable_copy || vtable_copy[9] == reinterpret_cast<void*>(&hk_distance_filter)) return;
    g_distance_orig = reinterpret_cast<DistanceFilterFn>(vtable_copy[9]);
    vtable_copy[9] = reinterpret_cast<void*>(&hk_distance_filter);
}

void steamguard_before_lobby_request(void* matchmaking) {
    if (g_distance_orig && InterlockedExchange(&g_distance_pending, 0)) {
        g_distance_orig(matchmaking, nullptr, static_cast<int>(g_distance_value));
    }
}
