#pragma once

// Installs the framerate patches. The game executable on disk is not modified.
// Returns false when this executable is not the supported build.
bool patches_apply();

// The ground snap's last decision for a physics body (for the trace): whether the lift happened and
// the factor the ladder-exit cap applied to the snap. False when the body has no record.
bool ground_snap_debug(const void* phys, float* factor, int* lifted);

// The follow camera's state right after its last update (for the trace): the step it got, the yaw/pitch
// blend weight it used (+0x23C), the look-at boost (+0x130) and its hold timer (+0x134), yaw and pitch,
// the camera position (+0x100), the lock-on target point (+0x250) and whether it is locked on (+0x260).
struct CameraTrace {
    float step = 0.0f;
    float weight = 0.0f;
    float boost = 0.0f;
    float boost_hold = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float target[3] = {0.0f, 0.0f, 0.0f};
    int locked = -1;
    const void* chr = nullptr;  // the character the camera follows (the update's third argument)
};
bool camera_trace(CameraTrace* out);

// The sprint graze check (fn 0x379B10) as seen since the last call for this move control (for the trace):
// how many times it ran, and for the first two runs the caller (RVA of the return address), the thread,
// the speed it was about to test (body movement / frame time) and the multiplier it started from.
struct GrazeTrace {
    unsigned calls = 0;
    unsigned caller[2] = {0, 0};
    unsigned thread[2] = {0, 0};
    float speed[2] = {-1.0f, -1.0f};
    float mul[2] = {-1.0f, -1.0f};
};
bool graze_trace_take(const void* mc, GrazeTrace* out);
