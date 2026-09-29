#include "xinput_proxy.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
constexpr DWORD kNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED

template <typename T>
T load(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

// Loading the real DLL from inside DllMain would run under the loader lock, so it
// happens on the first XInput call instead.
BOOL CALLBACK init_once(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) {
        return TRUE;
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\xinput1_3.dll", sys);
    HMODULE real = LoadLibraryW(path);
    if (!real) {
        LOG_ERROR("xinput proxy: could not load the system xinput1_3.dll (%lu)", GetLastError());
        return TRUE;
    }
    pGetState = load<GetStateFn>(real, "XInputGetState");
    pSetState = load<SetStateFn>(real, "XInputSetState");
    pGetCaps = load<GetCapsFn>(real, "XInputGetCapabilities");
    pEnable = load<EnableFn>(real, "XInputEnable");
    pGetDSound = load<GetDSoundFn>(real, "XInputGetDSoundAudioDeviceGuids");
    pGetBattery = load<GetBatteryFn>(real, "XInputGetBatteryInformation");
    pGetKeystroke = load<GetKeystrokeFn>(real, "XInputGetKeystroke");
    LOG_INFO("xinput proxy: forwarding to the system xinput1_3.dll");
    return TRUE;
}

void ensure() {
    InitOnceExecuteOnce(&g_once, init_once, nullptr, nullptr);
}

}  // namespace

extern "C" {

DWORD WINAPI XInputGetState(DWORD index, void* state) {
    ensure();
    return pGetState ? pGetState(index, state) : kNotConnected;
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
