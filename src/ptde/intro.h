#pragma once

// Set by the frame hook while the startup intro logos are being skipped. The
// Present hooks check it and swallow the real Present so the logos run without
// waiting on the display.
extern volatile long g_intro_skipping;

// 0 = waiting for the intro run, 1 = inside it, 2 = finished. Read by the wait hooks.
extern volatile long g_intro_state;
