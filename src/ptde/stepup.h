#pragma once

// FixStepUp: lips a little higher than the step height climbed as at 30 FPS (see stepup.cpp).
bool stepup_install();
void stepup_set_enabled(bool on);
bool stepup_enabled();
long stepup_boosted_frames();
float stepup_last_boost();
