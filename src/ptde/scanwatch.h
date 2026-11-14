#pragma once

#include <windows.h>

// Development (INI [Diagnostics] ScanWatch, diagnostics builds): what the display really does, for judging variable
// refresh while someone plays.
//  - A thread reads the primary display's scan line (D3DKMTGetScanLine, as tools/test_rendermath/scanmon) and notes
//    when each refresh starts. Every report (Profile's 5 s) logs how many refreshes there were and how far apart
//    (with variable refresh following a steady frame rate they are all one frame time), and how long after the
//    game's last flip each refresh started (following the frames: a short, steady delay; a display that refreshes
//    at its own pace: anything up to a whole refresh).
//  - Hotkeys for a tester driving the game: Ctrl+Shift+U logs the next 400 draws of the HUD and menu layout
//    (layout.cpp); Ctrl+Shift+P cycles DXVK's presenting through DXGI (timed, GPU when done, flip when ready);
//    Ctrl+Shift+H turns the scRGB swap chain on and off. dxgipresent.cpp shows the mode in the top right corner for
//    a few seconds after each change.
void scanwatch_start(bool monitor);
// A flip (or a Present on the system Direct3D 9) was asked for at `qpc`.
void scanwatch_note_flip(long long qpc);
void scanwatch_report();
