#pragma once

// FixLadderSlide: a slide down a ladder takes as long at any frame rate as at 30 FPS (see ladder.cpp).
void ladder_configure(bool on);
// Once a frame, before the game's update (dt = the step it is about to take, in seconds).
void ladder_update(double dt);
