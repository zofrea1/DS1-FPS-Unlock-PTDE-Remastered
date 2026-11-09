#pragma once

#include <atomic>
#include <cstdint>

// Published by the install thread before the scheduler is marked active.
inline std::atomic<uint32_t> g_target_fps{120};
inline std::atomic<int> g_scheduler_active{0};
inline std::atomic<int> g_is_game{0};
inline std::atomic<int> g_fix_move_dt{1};
inline std::atomic<int> g_fix_step_down{1};
inline std::atomic<int> g_fix_camera{1};
inline std::atomic<int> g_fix_graze{1};
inline std::atomic<int> g_fix_ui{1};
// 1 = the HUD bars move at the original game's speed (half Remastered's). INI HudPtdeSpeed, needs FixUI.
inline std::atomic<int> g_hud_ptde_speed{1};
inline std::atomic<int> g_fix_hold{1};
inline std::atomic<int> g_fix_stamina_tick{1};
inline std::atomic<int> g_input_log{0};
inline std::atomic<int> g_fix_damping{1};
inline std::atomic<int> g_fix_slide{1};
inline std::atomic<int> g_fix_jump{1};
// Steep slides keep their contact bit through a miss shorter than 0.95 of a native frame (0.95 / 30 s with
// SlidePtdeSpeed, 0.95 / 60 s without), a slide's turn keeps its root motion, and the soul counter follows the
// frame time (INI FixSlideContact, FixSlideSteer, FixSoulCounter).
inline std::atomic<int> g_fix_slide_contact{1};
inline std::atomic<int> g_fix_slide_steer{1};
inline std::atomic<int> g_fix_soul_counter{1};
// 1 = FixSlideContact / FixSlideSteer keep the original game's 30 FPS feel at any frame rate (INI
// SlidePtdeSpeed); 0 = Remastered's own 60 FPS feel.
inline std::atomic<int> g_slide_ptde_speed{1};
// Kick (forward + R1) and jumping attack (forward + R2): the stick flick and the press that goes with it follow
// native frames in real time (INI FixStickFlick); the roll direction's memory of the stick covers native frames
// (INI FixRollDirection). Both of the original game's 30 FPS with StickInputPtdeSpeed, else of Remastered's own 60.
inline std::atomic<int> g_fix_stick_flick{1};
inline std::atomic<int> g_fix_roll_direction{1};
inline std::atomic<int> g_fix_switchback{1};
inline std::atomic<int> g_stick_input_ptde_speed{1};
// SpEffect lifetimes expire on native frames (INI FixSpEffectTiming): of the original game's 30 FPS with
// SpEffectPtdeSpeed, else of Remastered's own 60 FPS.
inline std::atomic<int> g_fix_speffect_timing{1};
inline std::atomic<int> g_speffect_ptde_speed{1};
// Bullet hit records run out on native frames (INI FixBulletHitTiming): the original game's 30 FPS with
// BulletPtdeSpeed, else Remastered's own 60 FPS.
inline std::atomic<int> g_fix_bullet_hit_timing{1};
// Per-frame constants of a fixed 60 FPS frame (see patch_frame_constants): timers and fades, the camera
// pivot ease, and velocities derived from one frame's movement.
inline std::atomic<int> g_fix_timers{1};
inline std::atomic<int> g_fix_smoothing{1};
inline std::atomic<int> g_fix_velocity{1};
// Script garbage collection at least every 30 / 60 frames (see patch_script_gc). INI FixScriptMemory.
inline std::atomic<int> g_fix_script_memory{1};
// 1 = the lock-on camera pans, and the body turns toward the target, at the original Prepare to Die
// Edition speed (half the Remastered speed). INI LockOnPtdeSpeed (CameraPtdeSpeed is the old name).
inline std::atomic<int> g_camera_ptde_speed{1};
// 1 = movement uses the original game's value where Remastered differs (INI SlideFrictionPtde, GrazePtde,
// StepDownPtde, StepHeightPtde, AirDampingPtde; see Settings).
inline std::atomic<int> g_slide_ptde{1};
inline std::atomic<int> g_graze_ptde{1};
inline std::atomic<int> g_step_down_ptde{1};
inline std::atomic<int> g_step_height_ptde{1};
inline std::atomic<int> g_air_damping_ptde{1};
// 1 = projectiles at the original game's rates (INI BulletPtdeSpeed): a ground-crawling bullet's landing friction
// and ground factor per 1/30 s, repeating bullet volleys on the 30 FPS schedule; 0 = Remastered's (per 1/60 s).
inline std::atomic<int> g_bullet_ptde_speed{1};
// 1 = the game starts offline (INI StartOffline): the launch setting is set to Start Offline when the boot flow reads it.
inline std::atomic<int> g_start_offline{1};
// 1 = the start-up logos are skipped (INI SkipIntro).
inline std::atomic<int> g_skip_intro{1};
inline std::atomic<int> g_fix_lock_on_turn{1};
// Foot IK follows the frame time (INI FixFootIk), at the original game's speed with FootIkPtdeSpeed.
inline std::atomic<int> g_fix_foot_ik{1};
inline std::atomic<int> g_foot_ik_ptde_speed{1};
inline std::atomic<int> g_fix_ghosts{1};
// 1 = the game's step is the measured frame time (TargetFPS only caps the frame rate);
// 0 = the game's step is a fixed 1/TargetFPS (the game slows down when the rate is not held).
inline std::atomic<int> g_variable_dt{1};
// The step handed to the game for the frame in flight, in seconds. Written by the simulation
// hook once per frame; 1/TargetFPS until the first frame and whenever variable_dt is off.
inline std::atomic<float> g_frame_dt{1.0f / 120.0f};
// Longest measured frame handed to the game as it is. It leaves headroom over the lowest MaxFPS
// (10), so the game keeps real time down to 8 FPS; a longer frame (a hitch, loading, alt-tab)
// becomes one step of this length instead of a jump.
inline constexpr float kMaxFrameStep = 1.0f / 8.0f;
// Number of simulation steps seen, for the stall watchdog.
inline std::atomic<uint64_t> g_sim_count{0};
// 1 = the menu input de-duplication is installed (MenuInputFilter).
inline std::atomic<int> g_menu_filter{1};
inline std::atomic<uint64_t> g_menu_suppressed{0};
inline uint8_t* g_image = nullptr;

inline uint8_t* image_rva(uintptr_t rva) {
    return g_image + rva;
}
