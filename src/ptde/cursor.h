#pragma once

// HideMouseCursor: the game never hides the mouse pointer (it only sets the arrow as its window class cursor), so a
// controller player has it sitting on the screen. Over the game window the pointer is hidden once the mouse has not
// moved for two seconds, and comes back as soon as it moves (menus that take the mouse keep working).
//
// A thread waits for the game window and subclasses it; the hiding itself happens on the window's own thread
// (WM_SETCURSOR, and a timer the subclass starts there).
bool cursor_start();
