#pragma once

// FixStepUp: lips a little higher than the step height climbed as at 30 FPS (see stepup.cpp).
bool stepup_install();
void stepup_set_enabled(bool on);
bool stepup_enabled();
void stepup_set_mode(int mode);  // 0 off, 1 boost (the first version), 2 window (the default)
int stepup_mode();
long stepup_boosted_frames();
float stepup_last_boost();
long stepup_redone_windows();
long stepup_blocked_entries();
