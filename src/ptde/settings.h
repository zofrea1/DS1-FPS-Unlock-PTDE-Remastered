#pragma once

struct Settings {
    bool fps_unlock = true;
    // Frames per second the game should aim for (a cap). PTDE runs at 30 FPS natively.
    int target_fps = 240;
    // Overwrite the engine's fixed 1/30 step with the measured frame time.
    bool driver = true;
    // Change the game's swap-interval bookkeeping the way DSfix does (2 -> 5) so the
    // render thread stops waiting for two vertical blanks per frame.
    bool vblank_patch = true;
    // Exclusive fullscreen refresh rate: -1 leaves the game's choice alone, 0 uses the
    // highest rate the display offers at the game's resolution, N asks for N Hz.
    int fullscreen_refresh = -1;
    // Press F10 in a busy scene to sample the render thread for 15 s and log where the time goes.
    bool profile = false;
    // Disable the game's catch-up loop that re-presents a frame until the display's
    // vblank counter reaches its 30 FPS schedule.
    bool skip_present_repeat = false;
    // Make the game window borderless and cover its monitor. Needs the game set to
    // windowed mode. Skipped if DSfix's own borderlessFullscreen is on.
    bool borderless = false;
    // Log a per-command summary for this many seconds after start (0 = off).
    int survey_seconds = 30;
};

// Reads PTDE-FPS-Unlock.ini from the same directory as the DLL.
Settings settings_load(const wchar_t* dll_path);
