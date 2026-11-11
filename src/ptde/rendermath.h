#pragma once

// FastRenderMath: two small math helpers the render thread calls tens of thousands of times a frame, replaced with
// SSE versions that give bit-identical results.
//
//  * 0x4078D0, a four-float add: each lane is widened to double, added and rounded back to float, then stored one
//    float at a time. A double sum of two floats rounds to the same float as a float add (double has more than
//    twice float's precision), so one addps gives the same bits.
//  * 0x421E70, a 3x4 matrix add (out in ecx, A in eax, B on the stack, callee pops it) built from three calls of the
//    above; its callers then read each row back as two 8-byte loads of four 4-byte stores, which the CPU cannot
//    forward and stalls on. Called only by the draw traversal (0xF6B8xx..0xF6D3xx).
bool rendermath_install();
