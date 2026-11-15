#include "inputcache.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

// The game's input manager and its classes (PTDE Steam build).
constexpr uint32_t kInputManager = 0x1378458;    // pointer to the manager; four slots at +8..+0x14
constexpr uint32_t kMouseClass = 0x10E195C;      // DLUID::MouseDevice method table
constexpr uint32_t kKeyboardClass = 0x10E1CBC;   // DLUID::KeyboardDevice method table
constexpr int kGetDeviceStateSlot = 9;           // IDirectInputDevice8::GetDeviceState

using GetStateFn = HRESULT(WINAPI*)(void*, DWORD, void*);

int g_mode = 0;          // configured
bool g_on = false;       // in force
bool g_hooked = false;
int g_tries = 0;
uint32_t g_game_tid = 0;
volatile uint32_t g_frame = 1;

// Up to four DirectInput method tables (Wine may use one for both kinds).
struct Table {
    void** vt;
    GetStateFn original;
};
Table g_tables[4];
int g_table_n = 0;

struct Cached {
    uint32_t frame;
    DWORD size;
    uint8_t bytes[256];
};
Cached g_cache[2];  // 0: 20-byte mouse states, 1: 256-byte keyboard states
volatile LONG g_cached = 0, g_passed = 0;

GetStateFn original_for(void* self) {
    void** vt = *reinterpret_cast<void***>(self);
    for (int i = 0; i < g_table_n; ++i) {
        if (g_tables[i].vt == vt) return g_tables[i].original;
    }
    return nullptr;
}

HRESULT WINAPI hook_get_state(void* self, DWORD size, void* data) {
    const GetStateFn original = original_for(self);
    if (!original) return E_FAIL;  // cannot happen: only tables we hooked lead here
    const int k = size == 20 ? 0 : size == 256 ? 1 : -1;
    if (!g_on || k < 0 || !data || GetCurrentThreadId() != g_game_tid) return original(self, size, data);
    Cached& c = g_cache[k];
    if (c.frame == g_frame && c.size == size) {
        std::memcpy(data, c.bytes, size);
        g_cached = g_cached + 1;
        return 0;  // DI_OK
    }
    const HRESULT hr = original(self, size, data);
    g_passed = g_passed + 1;
    if (hr == 0) {
        std::memcpy(c.bytes, data, size);
        c.size = size;
        c.frame = g_frame;
    }
    return hr;
}

bool read32(uint32_t a, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool hook_table(void** vt) {
    for (int i = 0; i < g_table_n; ++i) {
        if (g_tables[i].vt == vt) return true;
    }
    if (g_table_n >= 4) return false;
    void** entry = &vt[kGetDeviceStateSlot];
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    // The original first (other threads may call through the table as soon as the entry changes).
    g_tables[g_table_n].vt = vt;
    g_tables[g_table_n].original = reinterpret_cast<GetStateFn>(*entry);
    MemoryBarrier();
    ++g_table_n;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), static_cast<LONG>(reinterpret_cast<uintptr_t>(&hook_get_state)));
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(void*), old, &ignored);
    return true;
}

// Walks the input manager to the mouse and keyboard sub-devices' DirectInput devices (+0xD8) and hooks their tables.
int find_and_hook() {
    uint32_t mgr = 0;
    if (!read32(kInputManager, &mgr) || !mgr) return 0;
    int found = 0;
    for (uint32_t k = 8; k <= 0x14; k += 4) {
        uint32_t slot = 0, vmd = 0, b = 0, e = 0;
        if (!read32(mgr + k, &slot) || !slot || !read32(slot + 4, &vmd) || !vmd) continue;
        if (!read32(vmd + 0xD8, &b) || !read32(vmd + 0xDC, &e) || b == 0 || e < b || e - b > 64) continue;
        for (uint32_t a = b; a < e; a += 4) {
            uint32_t sub = 0, cls = 0, di = 0, divt = 0;
            if (!read32(a, &sub) || !sub || !read32(sub, &cls)) continue;
            if (cls != kMouseClass && cls != kKeyboardClass) continue;
            if (!read32(sub + 0xD8, &di) || !di || !read32(di, &divt) || !divt) continue;
            if (hook_table(reinterpret_cast<void**>(divt))) ++found;
        }
    }
    return found;
}

}  // namespace

void inputcache_configure(int mode) {
    g_mode = mode;
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    g_on = mode == 1 || (mode == 2 && wine);
    if (mode == 2 && !wine) LOG_INFO("CacheInputReads: auto, not under Wine: the game's own reads");
}

void inputcache_game_frame() {
    g_frame = g_frame + 1;
    if (g_hooked || g_mode == 0 || g_tries > 200) return;
    if ((g_frame & 31) != 0) return;
    ++g_tries;
    g_game_tid = GetCurrentThreadId();
    const int n = find_and_hook();
    if (n > 0) {
        g_hooked = true;
        LOG_INFO("CacheInputReads: %s; the keyboard and mouse are read once a frame for the game's four input users "
                 "(%d DirectInput devices, %d method table(s))",
                 g_on ? "on" : "off for now", n, g_table_n);
    } else if (g_tries == 200) {
        LOG_INFO("CacheInputReads: the game's keyboard and mouse devices were not found; left alone");
    }
}

int inputcache_set(int on) {
    if (on >= 0) {
        g_on = on != 0;
        g_cache[0].frame = g_cache[1].frame = 0;
        if (!g_hooked && g_on && g_mode == 0) g_mode = 1;  // allow hooking later
    }
    return g_on ? 1 : 0;
}

void inputcache_stats(unsigned long* cached, unsigned long* passed) {
    *cached = static_cast<unsigned long>(g_cached);
    *passed = static_cast<unsigned long>(g_passed);
}
