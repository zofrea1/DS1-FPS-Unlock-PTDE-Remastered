#pragma once

// Makes the follow camera's per-frame smoothing frame-rate independent.
//
// ChrFollowCam::Update (0xF02720) smooths the camera toward its target with blend weights that
// are applied once per frame and were tuned for 30 FPS: the lock-on yaw/pitch state uses
// [camera+0x238], and the orientation filter uses [camera+0x1BC]. At 120 FPS the same per-frame
// weights finish a pan in a quarter of the time, so switching lock-on targets snaps. Before each
// update the weights are replaced by the equivalent for this frame's duration: a per-frame
// weight k applied n = dt * 30 times has the combined weight 1 - (1 - k)^n.
bool camera_install();
void camera_set_enabled(bool on);
bool camera_enabled();
