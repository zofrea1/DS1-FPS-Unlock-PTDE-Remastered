#pragma once

// In-engine cutscene camera. Real-time cutscenes advance the clock and the characters every frame, but the
// camera reader 0xD09DA0 (called from 0xFC2084) copies the nearest key and never blends: key A when the
// amount is below 0.5, key B otherwise. Keys sit on whole 30 FPS frames, so at 60 FPS the camera holds for
// two frames and then jumps. The zoom reader already blends and is left alone. Position (0xD09E9B) and
// rotation (0xD09F1A) are blended by the amount the game computed: position is A + (B - A) * amount, rotation
// is the three angles with the short-way delta in (-pi, pi). The fourth component stays the key the game
// would have picked. The nearest key is kept when the amount is outside (0, 1) or not a number, when the
// keys are more than 1.5 frames apart, or when the step is a cut (more than 1 m, or more than 0.5 rad).
// A position cut keeps the rotation choice for that same call. Nothing here is sent to other players.
bool cutscenecam_install();
