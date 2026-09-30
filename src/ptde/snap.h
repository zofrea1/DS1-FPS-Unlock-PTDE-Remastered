#pragma once

// Caps the ground-snap pull at the bottom of a ladder slide (see snap.cpp).
bool snap_install();
long snap_capped_count();
void snap_set_enabled(bool on);
bool snap_enabled();
