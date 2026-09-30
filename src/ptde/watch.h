#pragma once

// Diagnostic (INI Watch = c:1B8,p:180,...): F8 arms hardware write watchpoints on up to
// four dwords of the player's structures ("c" = character, "m" = movement controller,
// "p" = physics, offsets in hex) and F8 again reports, per writing instruction, how often
// it wrote, the range of values, and where it was called from.
void watch_set_spec(const char* spec);

// Called from the render thread once per frame.
void watch_poll();
