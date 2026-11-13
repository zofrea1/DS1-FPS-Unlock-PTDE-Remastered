# ffxloop

How the generated per-pass code for the effects library's particle update loop (0xA4D420, `a4_iter_full` /
`a4_iter_skip` in `src/ptde/rendermath.cpp`) was made.

1. Disassemble the loop body into `ADDR:instruction` lines (one per line, as `scratchpad/sweep/pdis.py` prints them with
   the byte column removed): `a4_fall.txt` = 0xA4DF22..0xA4E64E and 0xA4E658..0xA4FB8D (`[ebx+0x4C]` set),
   `a4_taken.txt` = 0xA4DF22..0xA4E64E and 0xA4F060..0xA4FB8D (not set).
2. `symloop.py LISTING 0 FFFFFFFF` traces the straight-line body symbolically: frame cells (ebp-X), argument fields
   (ebx+X), each helper call's pointer arguments resolved to earlier call results or frame inputs, and the frame cells
   read before being written (the loop's inputs). Only the counter at ebp-0x730 is both an input and written.
3. `gen_a4.py` turns both traces into C++ (`a4_gen.cpp`): the same helper calls at the same addresses with the same
   inputs, the array copy helpers 0x5538B0 / 0x553880 as 16-byte copies.

The same trace of 0x99C540's loop (0x99E9DA..0x99F1EA) gave `ffx_particle_loop`, written by hand from it. Both are
tested in `test_rmath.cpp` (`test_particle_loop`, `test_update_loop`) against the game's loops on fake frames.
