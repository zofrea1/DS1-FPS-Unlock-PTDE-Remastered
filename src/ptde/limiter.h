#pragma once

// MaxFPS cap. One point waits (FPSLimitPoint):
//  * kLimitEarly: limiter_wait_early(), the game thread at the start of a frame, before the pad, keyboard and mouse
//    are read (least input lag);
//  * kLimitPresent: limiter_wait_present(), the render thread just before a frame's first new picture is shown
//    (most even frame timing, slightly more lag);
//  * kLimitBoundary: limiter_frame_boundary(), the render thread when it takes the next frame (1.8.0's point), used
//    only when the hook for the chosen point could not be installed.
// limiter_frame_boundary() runs at every frame boundary whatever the point: it also lets the next Present wait.

enum LimitPoint { kLimitEarly, kLimitPresent, kLimitBoundary };

// Once, before the hooks are in.
void limiter_start(int fps);
// A new rate from any thread (the Trace keys); the waiting thread takes it up at its next wait.
void limiter_set_fps(int fps);
// The rate last asked for (0 = no cap), from any thread.
int limiter_requested_fps();
void limiter_set_point(LimitPoint point);
LimitPoint limiter_point();
bool limiter_is_on();
void limiter_wait_early();
void limiter_wait_present();
void limiter_frame_boundary();
// Milliseconds the cap has waited since the last take. The frame survey adds this in.
double limiter_take_wait_ms();
