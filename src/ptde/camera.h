#pragma once

// Makes the follow camera's per-frame smoothing frame-rate independent.
//
// ChrFollowCam::Update (0xF02720) smooths the camera toward its target with blend weights that
// are applied once per frame and were tuned for 30 FPS: the yaw/pitch state uses [camera+0x238],
// the camera distance [camera+0x1BC], the look-at point [+0x1C0]/[+0x1C4] (locked on) and
// [+0x1A4]/[+0x1B0], plus the pivot, yaw settling, automatic turn, stick smoothing and parameter
// changes (see camera.cpp). At 120 FPS the same per-frame weights finish a pan in a fraction of
// the time, so switching lock-on targets snaps. Each weight is replaced by the equivalent for this
// frame's duration: a per-frame weight k applied n = dt * 30 times has the weight 1 - (1 - k)^n.
bool camera_install();
void camera_set_enabled(bool on);
bool camera_enabled();
