#pragma once

// Caps the ground-snap pull at the bottom of a ladder slide (see snap.cpp).
bool snap_install();
long snap_capped_count();
long snap_wall_rise_count();  // frames whose rise up a wall was cut (snap.cpp)
void snap_set_enabled(bool on);
bool snap_enabled();
// Last snap call for a body (for the trace): proxy height minus frame-start height, the factor
// applied, whether the lift ran, and a running call count.
bool snap_debug(const void* body, float* dy, float* factor, int* lifted, unsigned* calls);
