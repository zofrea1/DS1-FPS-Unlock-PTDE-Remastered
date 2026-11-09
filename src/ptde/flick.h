#pragma once

// Kick (forward + R1) and jumping attack (forward + R2) at any frame rate (INI FixStickFlick).
//
// Prepare to Die Edition's stick flick is fn 0xE16F80, called once from PadManipulator::Update (0xE1C380,
// vtable slot 8 at 0x118E25C) at 0xE1D6B0 with ecx = manipulator, eax = 16-byte result, xmm3 = forward,
// [esp] = frame time, [esp+4] = side (ret 8, eax = result). Its state: +0x239 neutral byte, +0x2F4..+0x300 the
// four direction timers; its values: 0.06 s at 0x12DEF68, 0.3 at 0x12DEF6C, 0.2 at 0x12DEF70. The call goes to
// the real-time version (src/common/stickflick.h) and the update gets a wrapper that holds a fresh R1/R2 press
// while it could still become a kick or a jumping attack. The original game's rate is 30 FPS.
bool flick_install();
