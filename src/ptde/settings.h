#pragma once

struct Settings {
    bool fps_unlock = true;
    // Frames per second the game should aim for (a cap). PTDE runs at 30 FPS natively.
    int target_fps = 120;
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
    // Frame-rate independence fixes for constants tuned for 30 FPS (see fixes.h).
    bool fix_slide = true;
    bool fix_damping = true;
    bool fix_timers = true;
    bool fix_smoothing = true;
    bool fix_graze = true;
    bool fix_ladder = true;
    bool fix_camera = true;
    // HUD gauge fill speed (HP, stamina, boss bars) and the loading screen swirl.
    bool fix_ui = true;
    // Stand the character up if it stays seated at a bonfire with no menu open (reverse hollowing bug).
    bool bonfire_unstick = true;
    // Diagnostic: log the HUD shortcut (D-pad) actions with timestamps.
    bool input_log = false;
    // Diagnostic: F9 records the player's state to a CSV; F4/F5/F6/F7 switch the frame
    // rate live to 30/60/120/240 FPS.
    bool trace = false;
    // Diagnostic: hash every vertex shader constant upload to count presents whose content did
    // not change. Costs noticeable CPU per frame; leave off for normal play.
    bool content_probe = false;
    // Diagnostic: hardware write watchpoints on player fields, e.g. "c:1B8,p:180". F8 arms/reports.
    char watch[96] = {};
    // Disable the game's catch-up loop that re-presents a frame until the display's
    // vblank counter reaches its 30 FPS schedule.
    bool skip_present_repeat = false;
    // Make the game window borderless and cover its monitor. Needs the game set to
    // windowed mode. Skipped if DSfix's own borderlessFullscreen is on.
    bool borderless = false;
    // Log a per-command summary for this many seconds after start (0 = off).
    int survey_seconds = 0;
};

// Reads PTDE-FPS-Unlock.ini from the same directory as the DLL.
Settings settings_load(const wchar_t* dll_path);
