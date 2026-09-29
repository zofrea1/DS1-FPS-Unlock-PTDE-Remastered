#pragma once

#include <cstdint>

// Diagnostic only (INI Watch=true). Puts hardware write watchpoints on the
// vertical position of the busiest characters and logs the address of whatever
// writes a large step, so a physics displacement bug can be traced to code.
bool watch_start(const wchar_t* dll_path);
void watch_stop();
// Called by the tracer once per character update with the physics object.
void watch_note(const void* chr, void* phys);
