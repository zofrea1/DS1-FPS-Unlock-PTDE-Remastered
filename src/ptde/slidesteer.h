#pragma once

// Slide steering. 0xD98580 asks Havok (0x7BF200, the only caller, at 0xD98623) for blended root motion.
// While the live layer weight is not above the skeleton threshold at [skeleton+0x20] (0.1, constant
// 0x110B1D8), 0x7BF5D5 divides the motion by that threshold. A slide's turn (animations 7063 and 7064)
// restarts every frame, so the only live layer is one frame old: about 0.104 after 1/30 s, about 0.012
// after 1/144 s, and the body barely moves. Both loads of the threshold inside this call (0x7BF429 and
// 0x7BF58A) are multiplied by (min(frame time * 30, 1))^2. The stored 0.1 is not written, and pose blending
// never enters this function. At 30 FPS and below the factor is 1.
bool slidesteer_install();
// Once per frame with the step handed to the game. No effect until slidesteer_install has succeeded.
void slidesteer_update(double dt);
