#pragma once

// MaxFPS cap.  limiter_set_early chooses which call waits. limiter_wait_early() is the game thread, at the
// start of a frame, before the pad, keyboard and mouse are read. limiter_wait_present() is the render thread,
// immediately before the picture is shown. The other call returns immediately. The schedule is shared, so both
// must not wait.

void limiter_start(int fps);
void limiter_set_early(bool early);
bool limiter_is_on();
void limiter_wait_early();
void limiter_wait_present();
// Milliseconds the cap has waited since the last take. The frame survey adds this in.
double limiter_take_wait_ms();
