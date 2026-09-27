#pragma once

// Installs the framerate patches. The game executable on disk is not modified.
// Returns false when this executable is not the supported build.
bool patches_apply();
