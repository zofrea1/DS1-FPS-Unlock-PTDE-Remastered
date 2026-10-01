#pragma once

struct Settings {
    bool fps_unlock = true;
    int target_fps = 120;
    // Hand the game the measured frame time instead of a fixed 1/TargetFPS. TargetFPS then
    // only caps the frame rate, and the game keeps real-time speed when the rate dips.
    bool variable_frame_time = true;
    // De-duplicates menu button presses so one press is not seen several times per frame at
    // high frame rates. Turn off if menus ever stop responding.
    bool menu_input_filter = true;
    // Writes DSR-FPS-Unlock-trace.csv with per-character update samples. Off by
    // default: the file is large and the hook costs a little per update.
    bool trace = false;
    // Sprint slowdown near geometry, airborne momentum and slope slide scaled to the frame time.
    // HUD gauge fill speed (Estus, stamina and boss bars).
    bool fix_ui = true;
    // Sprint stamina drain ticks every 0.1 s on average at any frame rate (it rounded up to a whole frame).
    bool fix_stamina_tick = true;
    // D-pad hold (down: back to the first quick item) needs a quarter second at any frame rate.
    bool fix_dpad_hold = true;
    // Diagnostic: log the HUD shortcut (D-pad) actions with timestamps.
    bool input_log = false;
    bool fix_graze = true;
    bool fix_damping = true;
    bool fix_slide = true;
    // Feeds the move-control dispatcher the real frame time instead of a
    // hardcoded 1/60 (ladder slides overshoot the floor at high FPS without it).
    bool fix_move_dt = true;
    // Scales the ground step-down allowance to the frame rate (ledge/lip drop
    // boost and ladder fall-through at high FPS). Set false to A/B test.
    bool fix_step_down = true;
    // Makes the lock-on camera pan (target switches, locking onto an enemy near the edge of the
    // screen) take the same time at any frame rate. Set false to A/B test.
    bool fix_camera = true;
    // Pan the lock-on camera, and turn the body toward the target, at the speed of the original Dark
    // Souls (30 FPS) instead of the snappier Remastered speed (INI LockOnPtdeSpeed, old name
    // CameraPtdeSpeed). Needs FixCamera / FixLockOnTurn.
    bool camera_ptde_speed = false;
    // Lock-on body turn (torso, head and hips toward the target) follows the frame time.
    bool fix_lock_on_turn = true;
    // Bloodstain and wandering ghost replays play, and the player's replay data is recorded, in real time.
    bool fix_ghosts = true;
    // Run the player's movement physics at a fixed 60 steps a second like the game at its native 60 FPS,
    // with the visible position interpolated. Off by default (Remastered steps them every frame).
    bool fixed_rate_physics = false;
    // Hardware write-watch on character height (implies trace). Diagnostic only.
    bool watch = false;
};

// Reads DSR-FPS-Unlock.ini from the same directory as the DLL.
Settings settings_load(const wchar_t* dll_path);
