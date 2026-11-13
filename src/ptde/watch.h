#pragma once

// Diagnostic (INI Watch = c:1B8,p:180,...): F8 arms hardware write watchpoints on up to
// four dwords of the player's structures ("c" = character, "m" = movement controller,
// "p" = physics, offsets in hex) and F8 again reports, per writing instruction, how often
// it wrote, the range of values, and where it was called from.
#include <cstdint>

void watch_set_spec(const char* spec);

// Called from the render thread once per frame.
void watch_poll();

// The ecx and first stack argument the last time an execute slot ("x:ADDRESS") fired, or 0.
uint32_t watch_exec_ecx(int slot);
uint32_t watch_exec_arg0(int slot);

// Arms a hardware write watchpoint on one absolute address (the gauge recorder uses this to find
// what writes a value it saw change), and reports the writers.
void watch_arm_addresses(const uint32_t* addresses, int count);
// The same with a mode per address: 'x' execute breakpoint (the report has each caller and the first argument), 'r'
// read/write watchpoint (the report also lists the first accesses in order; never on a stack slot: the exception
// dispatch writes its context onto the stack and a watchpoint firing there ends the process), anything else a write
// watchpoint.
void watch_arm_modes(const uint32_t* addresses, const char* modes, int count);
void watch_report_now();
