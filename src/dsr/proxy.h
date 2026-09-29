#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Forwards DirectInput8Create to the system DLL and scales the system mouse
// once the high-refresh scheduler is active. The game imports only this symbol.
extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance, DWORD version, const GUID* interface_id, void** output, void* outer);
