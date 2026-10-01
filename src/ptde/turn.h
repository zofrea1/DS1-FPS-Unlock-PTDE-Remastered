#pragma once

// Lock-on body turn. With a target locked, TurnAnim and WalkAnim_Twist turn the character's
// Upper_Root, Lower_Root, Spine, Spine1 and Head bones toward it through bone rotation controllers
// (vtable 0x1195C40). Each controller's update (fn 0xD901D0, stdcall(controller, out, pose)) moves its
// current angle [+0x20] toward the target angle [+0x1C] by a per-frame blend weight [+0x2C]
// (0.6 for the roots while turning, 0.2 for the head, 0.1 for the spine, or whatever the caller set
// that frame), with no frame time. At 120 FPS the torso and head turn to face the target four times
// too fast. For the duration of each update the weight is replaced by the one that does the same
// over this frame's time: 1 - (1 - w) ^ (dt * 30).
bool turn_install();
void turn_set_enabled(bool on);
bool turn_enabled();
