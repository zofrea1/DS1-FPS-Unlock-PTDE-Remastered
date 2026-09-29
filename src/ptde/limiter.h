#pragma once

// Inline hook on the engine's own frame limiter (0x6455D0). During the startup intro
// the limiter is set to about 10 frames a second, which is what keeps the intro
// running for ~11 seconds. While the intro is being skipped this hook returns
// immediately; otherwise it forwards to the original.
bool limiter_install();
