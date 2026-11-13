#pragma once

// Bit-identical SSE replacements for small vector helpers the render thread calls tens of thousands of times a
// frame (each entry jumps to the replacement).
//
//  core (FastRenderMath): the engine's four-float add (0x4078D0), subtract (0x417420), multiply (0x5537E0) and
//    multiply-add (0x553700), ported Xbox 360 vector code compiled as scalar SSE through double, and the 3x4 matrix
//    add (0x421E70: out in ecx, A in eax, B on the stack, callee pops it) built from three adds, whose callers stall on
//    reading back four 4-byte stores as two 8-byte loads. A double sum, difference or product of two floats rounds to
//    the same float as the float operation (double has more than twice float's precision), so addps/subps/mulps give
//    the same bits; the multiply-add rounds a*b+c once from double, so it stays in double, two lanes at a time.
//    Also the 4x4 transpose (0x423220), four-float not-equal (0x407F50: A in eax, B on the stack, al = any lane
//    differs) and the 4x4 (0x4144D0) and 3x4-affine (0x416100) multiplies, row by row in double as the originals.
//    Arithmetic is written in assembly to keep each original's operand order (it decides which NaN comes out when two
//    meet); tools/test_rendermath runs the game's own code against these on random input, NaN-heavy input and
//    in-place calls, bit for bit.
//  effects (FastEffects): the effects library's debug-build wrappers that set (0x984890), splat (0x9847C0,
//    0x984920 through the x87 stack) and subtract (0x984B60) four floats.
//
// Each table is checked in full before any of it is written.
bool rendermath_install(bool core, bool effects);
// RemoteControl `cull 0|1`: the view culling replacement off or on (diagnostic A/B).
bool rendermath_set_cull(bool on);
