#pragma once

// Caps the ground-snap pull at the bottom of a ladder slide (see snap.cpp).
bool snap_install();
long snap_capped_count();
long snap_wall_rise_count();  // frames whose rise up a wall was cut (snap.cpp)
struct SnapWallRise {
    const void* body;
    float pos[3];  // the body's frame-start position
    float snap, lift, dt;
    unsigned long long ms;
};
unsigned snap_wall_rise_events(SnapWallRise* out, unsigned max);  // newest first
// FixStepUp's redone window (stepup.cpp): the next snap of BODY ends one WINDOW_DT frame that replaces its last
// REPLACED frames (the step-down reach counts it as one 30 FPS frame).
void snap_redo_begin(const void* body, float window_dt, unsigned replaced);
void snap_redo_end();
void snap_set_enabled(bool on);
bool snap_enabled();
// Last snap call for a body (for the trace): proxy height minus frame-start height, the factor
// applied, whether the lift ran, and a running call count.
bool snap_debug(const void* body, float* dy, float* factor, int* lifted, unsigned* calls);
