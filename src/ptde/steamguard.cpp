#include "steamguard.h"

#include "fpsu_buildcfg.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr double kCanaryEveryMs = 10.0;  // the canary asks Steam this often
constexpr double kGoWindowMs = 1.5;      // every-frame calls go ahead this long after a quick answer
constexpr double kWaitedMs = 50.0;       // a call that has waited this long gets a wider window ...
constexpr double kWideWindowMs = 5.0;    // ... (a whole frame at 200 FPS, still before the next question)
constexpr double kNoQuickMs = 500.0;     // no quick answer this long (Steam slow everywhere, or the canary gone):
constexpr double kStarveMs = 150.0;      // calls go ahead this often when no question is stuck
constexpr double kStuckMs = 1.0;         // a canary question out this long: Steam is in a pause
constexpr double kSlowMs = 4.0;          // a pause (and for the log)
constexpr double kInForceS = 60.0;       // the gate holds this long after the last pause seen

volatile LONG g_running = 0;
LONGLONG g_freq = 0;
volatile LONGLONG g_last_quick = 0;  // the end of the canary's latest quick answer
volatile LONGLONG g_last_pause = 0;  // the end of the canary's latest slow answer (0: none yet)
volatile LONGLONG g_canary_start = 0;
volatile LONG g_canary_out = 0;

// Counters for the diagnostics log.
volatile LONG g_calls_go = 0, g_calls_starved = 0, g_calls_held = 0, g_calls_free = 0, g_slow_notes = 0;

using RunCallbacksFn = void(__cdecl*)();
RunCallbacksFn g_run_callbacks = nullptr;
using AccessorFn = void*(__cdecl*)();
AccessorFn g_matchmaking = nullptr;
AccessorFn g_networking = nullptr;
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

// ------------------------------------------------------------------------------ the networking interface's queue
constexpr int kNetSlots = 22;  // ISteamNetworking005: SendP2PPacket .. GetMaxPacketSize
using SendFn = bool(__fastcall*)(void*, void*, uint64_t, const void*, uint32_t, int, int);
using PeerFn = bool(__fastcall*)(void*, void*, uint64_t);
using ChannelFn = bool(__fastcall*)(void*, void*, uint64_t, int);
void* g_net_orig[kNetSlots] = {};
void** g_net_vtable = nullptr;
struct NetCall {
    uint8_t slot;  // 0 send, 3 accept, 4 close session, 5 close channel
    void* self;
    uint64_t peer;
    int type, channel;
    LONGLONG queued;
    std::vector<uint8_t> data;
};
SRWLOCK g_net_lock = SRWLOCK_INIT;
std::vector<NetCall> g_net_queue;
HANDLE g_net_wake = nullptr;
volatile LONG g_net_done = 0;
volatile LONG g_net_wait_us_max = 0;

void net_push(NetCall&& c) {
    c.queued = now_qpc();
    AcquireSRWLockExclusive(&g_net_lock);
    if (g_net_queue.size() < 20000) g_net_queue.push_back(std::move(c));
    ReleaseSRWLockExclusive(&g_net_lock);
    SetEvent(g_net_wake);
}

bool __fastcall q_send(void* self, void*, uint64_t peer, const void* data, uint32_t size, int type, int channel) {
    const auto* b = static_cast<const uint8_t*>(data);
    net_push({0, self, peer, type, channel, 0, std::vector<uint8_t>(b, b + size)});
    return true;
}
bool __fastcall q_accept(void* self, void*, uint64_t peer) {
    net_push({3, self, peer, 0, 0, 0, {}});
    return true;
}
bool __fastcall q_close_session(void* self, void*, uint64_t peer) {
    net_push({4, self, peer, 0, 0, 0, {}});
    return true;
}
bool __fastcall q_close_channel(void* self, void*, uint64_t peer, int channel) {
    net_push({5, self, peer, 0, channel, 0, {}});
    return true;
}

DWORD WINAPI net_sender(void*) {
    std::vector<NetCall> calls;
    for (;;) {
        WaitForSingleObject(g_net_wake, 100);
        AcquireSRWLockExclusive(&g_net_lock);
        calls.swap(g_net_queue);
        ReleaseSRWLockExclusive(&g_net_lock);
        for (const NetCall& c : calls) {
            const LONGLONG wait_us = (now_qpc() - c.queued) * 1000000 / g_freq;
            if (wait_us > g_net_wait_us_max) InterlockedExchange(&g_net_wait_us_max, static_cast<LONG>(wait_us));
            switch (c.slot) {
                case 0:
                    reinterpret_cast<SendFn>(g_net_orig[0])(c.self, nullptr, c.peer, c.data.data(),
                                                            static_cast<uint32_t>(c.data.size()), c.type, c.channel);
                    break;
                case 3: reinterpret_cast<PeerFn>(g_net_orig[3])(c.self, nullptr, c.peer); break;
                case 4: reinterpret_cast<PeerFn>(g_net_orig[4])(c.self, nullptr, c.peer); break;
                default: reinterpret_cast<ChannelFn>(g_net_orig[5])(c.self, nullptr, c.peer, c.channel); break;
            }
            InterlockedIncrement(&g_net_done);
        }
        calls.clear();
    }
}

double take_net_wait_max() { return InterlockedExchange(&g_net_wait_us_max, 0) / 1000.0; }

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

void* __cdecl hk_networking() {
    void* net = g_networking();
    if (net && !g_net_vtable) steamguard_wrap_networking(net);
    return net;
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
            g_last_pause = t1;
            ++slow;
            worst = ms > worst ? ms : worst;
        }
#if FPSU_DIAGNOSTICS
        if (t1 >= next_stats) {
            next_stats = t1 + g_freq * 60;
            LOG_INFO("SteamPauseGuard: last minute: Steam answered the canary slowly %d times (longest %.1f ms, usual "
                     "%.3f ms); every-frame Steam calls: %ld with no pause lately, %ld right after a quick answer, %ld "
                     "with none for %.0f ms, %ld held back; %ld slow Steam answers to calls made elsewhere; %ld network "
                     "calls sent from the queue (longest wait %.1f ms)",
                     slow, worst, usual, InterlockedExchange(&g_calls_free, 0), InterlockedExchange(&g_calls_go, 0),
                     InterlockedExchange(&g_calls_starved, 0), kNoQuickMs, InterlockedExchange(&g_calls_held, 0),
                     InterlockedExchange(&g_slow_notes, 0), InterlockedExchange(&g_net_done, 0),
                     take_net_wait_max());
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
        if (patch_slot(import_slot("steam_api.dll", "SteamNetworking"), reinterpret_cast<void*>(&hk_networking),
                       &orig)) {
            g_networking = reinterpret_cast<AccessorFn>(orig);
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
    if (!g_last_pause || now - g_last_pause > static_cast<LONGLONG>(kInForceS * g_freq)) {
        InterlockedIncrement(&g_calls_free);  // Steam has not paused lately: every frame, as before
        *last = now;
        return true;
    }
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

void steamguard_wrap_networking(void* networking) {
    if (!networking || g_net_vtable || !g_freq) return;  // g_freq: the guard was started (Windows, on)
    void** vt = *static_cast<void***>(networking);
    void** copy = static_cast<void**>(VirtualAlloc(nullptr, kNetSlots * sizeof(void*), MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE));
    g_net_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE sender = g_net_wake ? CreateThread(nullptr, 0, net_sender, nullptr, 0, nullptr) : nullptr;
    if (!copy || !sender) {
        LOG_ERROR("SteamPauseGuard: could not set up the network queue; the game sends as before");
        return;
    }
    SetThreadPriority(sender, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(sender);
    std::memcpy(copy, vt, kNetSlots * sizeof(void*));
    std::memcpy(g_net_orig, vt, kNetSlots * sizeof(void*));
    copy[0] = reinterpret_cast<void*>(&q_send);
    copy[3] = reinterpret_cast<void*>(&q_accept);
    copy[4] = reinterpret_cast<void*>(&q_close_session);
    copy[5] = reinterpret_cast<void*>(&q_close_channel);
    g_net_vtable = copy;
    *static_cast<void***>(networking) = copy;
    LOG_INFO("SteamPauseGuard: the game's P2P sends go through a queue on a thread of their own");
}

void* steamguard_networking_original(int slot) {
    return g_net_vtable && slot >= 0 && slot < kNetSlots ? g_net_orig[slot] : nullptr;
}

void steamguard_before_lobby_request(void* matchmaking) {
    if (g_distance_orig && InterlockedExchange(&g_distance_pending, 0)) {
        g_distance_orig(matchmaking, nullptr, static_cast<int>(g_distance_value));
    }
}
