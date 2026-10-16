#pragma once

// MaxFPS range. A value outside it is clamped to the nearest end.
constexpr int kMinMaxFps = 10;
constexpr int kMaxMaxFps = 1000;

struct Settings {
    // Master switch: false leaves the game completely unchanged (every fix and diagnostic off).
    bool fps_unlock = true;
    // Frame rate cap (INI MaxFPS, old name TargetFPS), kMinMaxFps..kMaxMaxFps.
    int target_fps = 120;
    // Hand the game the measured frame time instead of a fixed 1/TargetFPS. TargetFPS then
    // only caps the frame rate, and the game keeps real-time speed when the rate dips.
    bool variable_frame_time = true;
    // De-duplicates menu button presses so one press is not seen several times per frame at
    // high frame rates. Turn off if menus ever stop responding.
    bool menu_input_filter = true;
    // Stand the character up if it stays seated at a bonfire with no menu open.
    // INI key BonfireFix (BonfireUnstick is accepted). Default on when the key is absent.
    bool bonfire_unstick = true;
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
    // Jump take-off: keeps a running jump at full distance when the take-off loses one frame of root motion
    // (half-length jumps at 30 FPS). Set false to A/B test.
    bool fix_jump = true;
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
    // CameraPtdeSpeed). Needs FixCamera / FixLockOnTurn. On when the key is missing (since 1.8.0, like the
    // movement switches below; before, a missing key kept Remastered's speed).
    bool camera_ptde_speed = true;
    // Movement values of the original Dark Souls where Remastered differs. Remastered kept several per-frame
    // values of the 30 FPS game but applies them 60 times a second, and changed two.
    //   slide_ptde (SlideFrictionPtde): slide momentum off a steep slope fades at 0.65 per 1/30 s and stops
    //     below 0.1 m/s (Remastered 0.65 per 1/60 s, 0.07 m/s). Needs FixSlide.
    //   graze_ptde (GrazePtde): the graze slow-down and recovery run at 0.8 / 1.2 per 1/30 s. Needs FixGraze.
    //   step_down_ptde (StepDownPtde): the ground snap may pull the body down 0.4 per 1/30 s, and the ladder
    //     exit's one-frame snap is spread over 1/30 s. Needs FixStepDown.
    //   step_height_ptde (StepHeightPtde): step-up height 0.30 instead of 0.31.
    //   air_damping_ptde (AirDampingPtde): airborne momentum fades at 0.95 per 1/30 s (Remastered 0.975 per
    //     1/60 s). Needs FixDamping.
    bool slide_ptde = true;
    bool graze_ptde = true;
    bool step_down_ptde = true;
    bool step_height_ptde = true;
    bool air_damping_ptde = true;
    // Lock-on body turn (torso, head and hips toward the target) follows the frame time.
    bool fix_lock_on_turn = true;
    // Bloodstain and wandering ghost replays play, and the player's replay data is recorded, in real time.
    bool fix_ghosts = true;
    // Per-frame 1/60 timers and fades: weapon buff glow, ragdoll blend-in, item glow, a load queue countdown.
    bool fix_timers = true;
    // The follow camera's pivot and look-at point ease toward their targets by 1/60 of the gap a frame.
    bool fix_smoothing = true;
    // Velocities derived from one frame's movement times 60: the powered ragdoll and 3D sound (Doppler).
    bool fix_velocity = true;
    // Script garbage collection at least every 30 (AI) / 60 frames, as at 60 FPS. Without it the AI
    // script heap can fill at a high frame rate in a crowded area and the game closes itself.
    bool fix_script_memory = true;
    // Hardware write-watch on character height (implies trace). Diagnostic only.
    bool watch = false;
};

// Reads DSR-FPS-Unlock.ini from the same directory as the DLL.
Settings settings_load(const wchar_t* dll_path);
