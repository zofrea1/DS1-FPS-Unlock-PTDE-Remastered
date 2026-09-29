#pragma once

// Set by the frame hook while the startup intro logos are being skipped. The
// Present hooks check it and swallow the real Present so the logos run without
// waiting on the display.
extern volatile long g_intro_skipping;
