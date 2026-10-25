#pragma once

// Steep-slide contact. Once a frame, 0xE29870 latches the physics contact bit into bit 0 of the byte at
// [action+0x43C] (the store at 0xE298BB; eax there is the block at [action+0x78]). Script condition 296,
// animation 7060, cancels into 7062 ("walking in the air") when that bit is clear for a frame. On a steep
// slide the contact can miss for a single short frame at the lip. Bit 0 of this byte is kept set for 1/30 s
// after the last real contact. The physics flags themselves are not written. At a 1/30 s step the hold has
// already expired, so 30 FPS and below match the game. A gap longer than a quarter second (a load, or a hitch
// where this block was not updated) drops the hold. The same store opcode also appears at 0xE29A71, inside a
// block copy; that write is left alone.
bool slidecontact_install();
