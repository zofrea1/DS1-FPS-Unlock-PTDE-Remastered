#pragma once

#include "settings.h"

// Validates the executable and reports what the mod can see. Phase 1 only observes:
// no game memory is modified yet.
bool patches_probe(const Settings& settings);
