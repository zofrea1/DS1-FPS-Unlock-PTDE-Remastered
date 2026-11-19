#pragma once

// FixLadderSlide: a slide down a ladder takes as long at any frame rate as at 60 FPS (see ladder.cpp).
void ladder_configure(bool on);
// Once a simulation step, before the game's update (dt = the step it is about to take, in seconds). Only called
// from the simulation hook, which exists only on the supported executable.
void ladder_update(double dt);
