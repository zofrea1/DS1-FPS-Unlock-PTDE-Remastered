#pragma once

struct IDirect3DDevice9;

// GammaRamp (INI). The game applies Options > Brightness as a gamma ramp in fullscreen: SetGammaRampSceneEntity
// (0xFFB9F0) builds ramp[i] = (i / 255) ^ (src / dst) per channel from six floats the scene sends (command 0x8062),
// and 0x5848F0 passes it to IDirect3DDevice9::SetGammaRamp(0, D3DSGR_NO_CALIBRATION, ramp) when the game's copy of
// the present parameters (+0xA0C) says fullscreen (Windowed at +0xA2C). Windows ignores gamma ramps while the display
// runs in advanced colour (HDR, or Auto Color Management on an SDR display), so with the system Direct3D 9 the ramp
// does nothing there. DXVK applies a non-identity ramp itself, in its present shader, whenever the game is fullscreen,
// whatever the display does; there the two renderers show different pictures.
enum GammaRampMode {
    kGammaAuto = 0,  // under DXVK the ramp is left out while the game's display is in advanced colour
    kGammaOn = 1,    // the ramp always reaches Direct3D
    kGammaOff = 2,   // the ramp never does (the brightness option has no effect)
};

// Hooks SetGammaRamp on the device the game holds; every ramp that changes is logged (with the display's colour mode).
void gamma_hook_device(IDirect3DDevice9* game_device, int mode, bool dxvk);
// After a successful Reset: logs the colour mode of the game's display (it can change with fullscreen).
void gamma_after_reset(IDirect3DDevice9* game_device);
// Once a frame (render thread): notices NVIDIA RTX HDR (nvAIHDR.dll) joining the process and says so once. The NVIDIA
// app's RTX HDR applies to the game only when it runs on Vulkan (DXVK), never to the system Direct3D 9, so with it on
// the two look very different under Windows HDR (crushed shadows, highlights pushed to the display's peak, other
// colours); the frames themselves are identical (tools/test_rendermath/hdrprobe, renamed DARKSOULS.exe, reproduces it).
void gamma_frame();
