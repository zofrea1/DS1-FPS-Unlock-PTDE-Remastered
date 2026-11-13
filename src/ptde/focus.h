#pragma once

// The game reads the keyboard, mouse and gamepads only while GetForegroundWindow() is its own window (the input
// manager's per-frame update 0xF72500 stores the answer at +0x4C and otherwise sets every device inactive; 0xF6FB54
// does the same for the window object). focus_install wraps that import (0x010CC2BC) and logs each change of the
// answer: the game leaving the foreground and which window took it, and its return. Under Wine (Proton, GameNative)
// a window manager that does not hand the focus back after the game was paused leaves the gamepad dead until the
// game window is clicked; the log shows that case.
void focus_install();
