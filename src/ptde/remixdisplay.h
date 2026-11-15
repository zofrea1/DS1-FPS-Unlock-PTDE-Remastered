#pragma once

// RTX Remix's bridge (d3d9_remix.dll beside a Remix-converting d3d9.dll, or the game-folder d3d9.dll itself) sets the
// display mode for exclusive fullscreen on its own with ChangeDisplaySettingsExW, at 60 Hz whatever refresh rate the
// game's device asked for. remix_display_install puts FullscreenRefreshRate on that call: its import in the bridge is
// wrapped so a mode at the bridge's size gets the rate this mod would choose (0: the display's highest at that size,
// N: N Hz or the highest below it). `refresh` < 0 leaves it alone. Safe to call more than once.
void remix_display_install(int refresh);
