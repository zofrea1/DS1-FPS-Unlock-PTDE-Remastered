#pragma once

// The game's low frame rate kick ("Framerate is insufficient for Online play. Returning to the Title menu.",
// system message 20001017). MoveMapStep (vtable NS_FRPG::MoveMapStep) keeps a frame rate meter at +0xF0, built by
// 0x100DDE0 and fed every frame by 0x100E0B0 (from the step update 0xEFD2F0, at 0xEFEAC3) unless a menu or a load
// holds the clock: it spreads the real time since the last frame over one-second buckets with the frame counts
// (0x100DE30) and keeps the last 30. Once it has 30 seconds, it stores their average at meter+8 and, at 15 FPS
// or less, sets meter+0xC (MoveMapStep+0xFC) at 0x100E083. While online (GameMan+0xB4E), the step's network checks
// (0xEFC2C0) then call the Lua handler OnFpsDisconnection (0xD78400, which shows 20001017 and asks for the title)
// and set GameMan+0xB4C, after which the title screen says "Error with Framerate detected and Online mode is not
// accessible" (system message 50530, title step 0xEE3470) and keeps the game offline until it is restarted.
// A single pause is enough: the game frozen for about 20 seconds (a debugger, a stuck driver, a phone app that
// pauses the game while another app is in front) leaves too few frames in the last 30 seconds.
// With FixFramerateKick the flag write is left out; the meter still measures (meter+8), nothing reads it.
bool framekick_install(bool fix);
