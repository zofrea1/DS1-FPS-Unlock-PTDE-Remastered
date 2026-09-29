#pragma once

struct Settings {
    bool fps_unlock = true;
    int target_fps = 120;
    // Writes DSR-FPS-Unlock-trace.csv with per-character update samples. Off by
    // default: the file is large and the hook costs a little per update.
    bool trace = false;
    // Feeds the move-control dispatcher the real frame time instead of a
    // hardcoded 1/60 (ladder slides overshoot the floor at high FPS without it).
    bool fix_move_dt = true;
    // Scales the ground step-down allowance to the frame rate (ledge/lip drop
    // boost and ladder fall-through at high FPS). Set false to A/B test.
    bool fix_step_down = true;
    // Hardware write-watch on character height (implies trace). Diagnostic only.
    bool watch = false;
};

// Reads DSR-FPS-Unlock.ini from the same directory as the DLL.
Settings settings_load(const wchar_t* dll_path);
