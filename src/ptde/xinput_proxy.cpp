#include "xinput_proxy.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Defined with the exports below. Named here so the loader check can take its address.
extern "C" DWORD WINAPI XInputGetState(DWORD index, void* state);

#include <cstdio>

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
using SetStateFn = DWORD(WINAPI*)(DWORD, void*);
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, void*);
using EnableFn = void(WINAPI*)(BOOL);
using GetDSoundFn = DWORD(WINAPI*)(DWORD, void*, void*);
using GetBatteryFn = DWORD(WINAPI*)(DWORD, BYTE, void*);
using GetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, void*);

GetStateFn pGetState = nullptr;
SetStateFn pSetState = nullptr;
GetCapsFn pGetCaps = nullptr;
EnableFn pEnable = nullptr;
GetDSoundFn pGetDSound = nullptr;
GetBatteryFn pGetBattery = nullptr;
GetKeystrokeFn pGetKeystroke = nullptr;
char g_target[MAX_PATH] = {};

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
constexpr DWORD kNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED

template <typename T>
T load(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

HMODULE self_module() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&XInputGetState), &module);
    return module;
}

void narrow_path(const wchar_t* wide, char* out, int out_len) {
    if (!wide || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, out_len, nullptr, nullptr)) {
        out[0] = 0;
    }
}

// `mod` is usable when it is a different module and its XInputGetState is not our
// own export. A hit on ourselves would recurse until the stack overflows, which on
// the Steam Deck is the controller going dead while the keyboard still works.
bool bind_module(HMODULE mod, const wchar_t* label) {
    char text[MAX_PATH];
    narrow_path(label, text, MAX_PATH);
    if (!mod) {
        return false;
    }
    if (mod == self_module()) {
        LOG_INFO("xinput proxy: skipped %s (that module is this DLL)", text);
        return false;
    }
    auto get_state = load<GetStateFn>(mod, "XInputGetState");
    auto set_state = load<SetStateFn>(mod, "XInputSetState");
    if (!get_state || !set_state || reinterpret_cast<void*>(get_state) == reinterpret_cast<void*>(&XInputGetState)) {
        LOG_INFO("xinput proxy: skipped %s (no XInputGetState of its own)", text);
        return false;
    }
    pGetState = get_state;
    pSetState = set_state;
    pGetCaps = load<GetCapsFn>(mod, "XInputGetCapabilities");
    pEnable = load<EnableFn>(mod, "XInputEnable");
    pGetDSound = load<GetDSoundFn>(mod, "XInputGetDSoundAudioDeviceGuids");
    pGetBattery = load<GetBatteryFn>(mod, "XInputGetBatteryInformation");
    pGetKeystroke = load<GetKeystrokeFn>(mod, "XInputGetKeystroke");

    wchar_t loaded[MAX_PATH];
    if (GetModuleFileNameW(mod, loaded, MAX_PATH)) {
        narrow_path(loaded, text, MAX_PATH);
    }
    snprintf(g_target, sizeof(g_target), "%s", text);
    LOG_INFO("xinput proxy: forwarding to %s", text);
    return true;
}

bool try_file(const wchar_t* file) {
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, MAX_PATH)) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, _TRUNCATE, L"%s\\%s", sys, file);
        if (bind_module(LoadLibraryW(path), path)) {
            return true;
        }
    }
    // By name, so Wine's builtin (which is not a file in system32) can still load.
    // xinput1_3 itself is never tried this way: that name is already this DLL.
    return bind_module(LoadLibraryW(file), file);
}

// Last resort for a Windows system xinput1_3 whose full path still resolves to us.
// A different file name is a different module, so the loader will not hand back this DLL.
bool try_renamed_system() {
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) {
        return false;
    }
    wchar_t src[MAX_PATH];
    _snwprintf_s(src, _TRUNCATE, L"%s\\xinput1_3.dll", sys);
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) {
        return false;
    }
    wchar_t dst[MAX_PATH];
    _snwprintf_s(dst, _TRUNCATE, L"%sptde-xinput-host.dll", dir);
    if (!CopyFileW(src, dst, FALSE)) {
        return false;
    }
    return bind_module(LoadLibraryW(dst), dst);
}

// Loading from inside DllMain would run under the loader lock, so the real library
// is chosen on the first XInput call. xinput1_4 is preferred: it is the Windows 8+
// library and Wine's real controller backend, and a native override of xinput1_3
// does not apply to it. xinput9_1_0 is the Windows 7 library and covers GetState
// and SetState, which is all the game imports.
BOOL CALLBACK init_once(PINIT_ONCE, PVOID, PVOID*) {
    if (try_file(L"xinput1_4.dll") || try_file(L"xinput9_1_0.dll") || try_renamed_system()) {
        return TRUE;
    }
    LOG_ERROR("xinput proxy: no real XInput library could be loaded; controllers will be disconnected");
    return TRUE;
}

void ensure() {
    InitOnceExecuteOnce(&g_once, init_once, nullptr, nullptr);
}

const char* target_path() {
    ensure();
    return g_target;
}

}  // namespace

DWORD xinput_get_state(DWORD index, void* state) {
    ensure();
    return pGetState ? pGetState(index, state) : kNotConnected;
}

DWORD xinput_get_caps(DWORD index, void* caps) {
    ensure();
    return pGetCaps ? pGetCaps(index, 0, caps) : kNotConnected;
}

void xinput_log_target() {
    const char* path = target_path();
    if (path[0]) {
        LOG_INFO("xinput proxy: forwarding to %s", path);
    } else {
        LOG_ERROR("xinput proxy: no real XInput library could be loaded; controllers will be disconnected");
    }
}

extern "C" {

DWORD WINAPI XInputGetState(DWORD index, void* state) {
    return xinput_get_state(index, state);
}
DWORD WINAPI XInputSetState(DWORD index, void* vibration) {
    ensure();
    return pSetState ? pSetState(index, vibration) : kNotConnected;
}
DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, void* caps) {
    ensure();
    return pGetCaps ? pGetCaps(index, flags, caps) : kNotConnected;
}
void WINAPI XInputEnable(BOOL enable) {
    ensure();
    if (pEnable) {
        pEnable(enable);
    }
}
DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, void* render, void* capture) {
    ensure();
    return pGetDSound ? pGetDSound(index, render, capture) : kNotConnected;
}
DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE devType, void* battery) {
    ensure();
    return pGetBattery ? pGetBattery(index, devType, battery) : kNotConnected;
}
DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, void* keystroke) {
    ensure();
    return pGetKeystroke ? pGetKeystroke(index, reserved, keystroke) : kNotConnected;
}

}  // extern "C"
