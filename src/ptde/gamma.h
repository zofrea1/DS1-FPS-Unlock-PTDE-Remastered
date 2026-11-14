#pragma once

#include <windows.h>

#include <cstddef>

struct IDirect3DDevice9;

// GammaRamp (INI). The game applies Options > Brightness as a gamma ramp in fullscreen: SetGammaRampSceneEntity
// (0xFFB9F0) builds ramp[i] = (i / 255) ^ (src / dst) per channel from six floats the scene sends (command 0x8062),
// and 0x5848F0 passes it to IDirect3DDevice9::SetGammaRamp(0, D3DSGR_NO_CALIBRATION, ramp) when the game's copy of
// the present parameters (+0xA0C) says fullscreen (Windowed at +0xA2C). The system Direct3D 9 applies it with Windows
// HDR on as well (the Brightness option works there; checked on an AW3225QF, HDR on), and DXVK applies it itself in its
// present shader, so by default it is passed on in every case.
enum GammaRampMode {
    kGammaAuto = 0,  // the default: passed on (same as kGammaOn; older INIs say auto)
    kGammaOn = 1,    // the ramp always reaches Direct3D
    kGammaOff = 2,   // the ramp never does (the brightness option has no effect)
};

// Windows HDR on the primary display (the one Direct3D's default adapter shows): 1 on, 0 off, -1 unknown. text gets
// the display's name and colour mode for the log.
int primary_display_hdr(char* text, size_t size);
// The given monitor's colour mode: 1 Windows HDR, 2 advanced colour without HDR (Auto Color Management: the desktop is
// composed in FP16 as in HDR, SDR white at 80 nits), 0 standard, -1 unknown; and its SDR brightness setting as a
// multiple of 80 nits (what an SDR picture's white is shown at; 1 when unknown).
int display_hdr_white(HMONITOR monitor, float* white);

// Hooks SetGammaRamp on the device the game holds; every ramp that changes is logged (with the display's colour mode).
void gamma_hook_device(IDirect3DDevice9* game_device, int mode, bool dxvk);
// After a successful Reset: logs the colour mode of the game's display (it can change with fullscreen).
void gamma_after_reset(IDirect3DDevice9* game_device);
// Once a frame (render thread): notices NVIDIA RTX HDR (nvAIHDR.dll) joining the process and says so once. The NVIDIA
// app's RTX HDR applies to the game only when it runs on Vulkan (DXVK), never to the system Direct3D 9, so with it on
// the two look very different under Windows HDR (crushed shadows, highlights pushed to the display's peak, other
// colours); the frames themselves are identical (tools/test_rendermath/hdrprobe, renamed DARKSOULS.exe, reproduces it).
void gamma_frame();
