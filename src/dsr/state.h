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
inline std::atomic<int> g_input_log{0};
inline std::atomic<int> g_fix_damping{1};
inline std::atomic<int> g_fix_slide{1};
// 1 = the lock-on camera pans at the original Prepare to Die Edition speed (half the Remastered speed).
inline std::atomic<int> g_camera_ptde_speed{0};
// 1 = the game's step is the measured frame time (TargetFPS only caps the frame rate);
// 0 = the game's step is a fixed 1/TargetFPS (the game slows down when the rate is not held).
inline std::atomic<int> g_variable_dt{1};
// The step handed to the game for the frame in flight, in seconds. Written by the simulation
// hook once per frame; 1/TargetFPS until the first frame and whenever variable_dt is off.
inline std::atomic<float> g_frame_dt{1.0f / 120.0f};
// Number of simulation steps seen, for the stall watchdog.
inline std::atomic<uint64_t> g_sim_count{0};
// 1 = the menu input de-duplication is installed (MenuInputFilter).
inline std::atomic<int> g_menu_filter{1};
inline std::atomic<uint64_t> g_menu_suppressed{0};
inline uint8_t* g_image = nullptr;

inline uint8_t* image_rva(uintptr_t rva) {
    return g_image + rva;
}
