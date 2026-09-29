#pragma once

#include <atomic>
#include <cstdint>

// Published by the install thread before the scheduler is marked active.
inline std::atomic<uint32_t> g_target_fps{120};
inline std::atomic<int> g_scheduler_active{0};
inline std::atomic<int> g_is_game{0};
inline std::atomic<int> g_fix_move_dt{1};
inline std::atomic<int> g_fix_step_down{1};
inline uint8_t* g_image = nullptr;

inline uint8_t* image_rva(uintptr_t rva) {
    return g_image + rva;
}
