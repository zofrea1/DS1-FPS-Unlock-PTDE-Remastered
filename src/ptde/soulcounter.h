#pragma once

// HUD soul counter (FrpgMenuDlgFEGetSoul::update, 0xFADA70). Once a frame, 0xFADCB1 adds the whole step in
// ecx to the shown value at [dialog+0x108]. The step grows with the amount still left (+1, then more above
// 10, +10 above 100, and so on). The frame time is an argument and the game uses it only for the pause after
// the count, so at 144 FPS a pickup finishes several times too soon. The step is multiplied by the frame
// time * 30 and the fraction is carried to the next frame, with the same sign as the step. The game's own
// clamp, just after this add, still stops the shown value on the target. One update adds at most four of the
// game's steps, so a hitch cannot skip the count ahead. The carry starts over on a new dialog, a gap of
// 300 ms, or a change of direction. This is not the bar fill (FixUI) and not the loading swirl.
bool soulcounter_install();
