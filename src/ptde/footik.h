#pragma once

// Foot IK (stairs and slopes). The character's feet are placed on the ground by Havok's
// hkaFootPlacementIkSolver, one per leg (0xED4A70 builds the solver input, ctor 0x7B1510, and calls
// doFootPlacement 0x7B1A50 at 0xED4BF4), and its pelvis is lowered so both feet reach (0xFE1740, a ChrIns
// method). Three of the steps are fixed fractions per frame with no frame time:
//  * the pelvis offset [chr+0x2C8] moves toward the legs' vertical error by 0.2 of it a frame (the double
//    at 0x11E82D0, loaded at 0xFE1991), clamped to +-0.5;
//  * with no foot on the ground it decays by [chr+0x2EC] (0.6, from 0x1161970 in the constructor) a frame
//    (loaded at 0xFE193C);
//  * the solver blends its weight toward on (1) or off (0) by the input's onOffGain [+0x68] (0.2) a call.
// At 120 FPS the pelvis followed every step of a staircase about three times as fast as at 30 FPS, so the
// body bobbed with each step. Each fraction is now applied per 1/30 s: 1 - 0.8 ^ (frame time * 30) for the
// gains and 0.6 ^ (frame time * 30) for the decay. The solver's other gains (ground height, foot planted,
// raised, unlock) are 1.0, applied in full each call, which does not depend on the frame rate.
bool footik_install();
// Once per frame with the step handed to the game.
void footik_update(double dt);
void footik_set_enabled(bool on);
bool footik_enabled();
