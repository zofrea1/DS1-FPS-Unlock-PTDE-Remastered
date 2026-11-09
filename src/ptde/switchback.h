#pragma once

// Switchback (reversal) animation at any frame rate (INI FixSwitchback; see turnhistory.h).
//
// TurnAnim (vtable 0x118E810): slot 3 at 0x118E81C (0xE13160, thiscall, 6 stack arguments, returns its out
// pointer) writes the ring each update and gets a wrapper that records the new entry. The movement update
// (0xFE11E0) asks 0xE10DB0 (thiscall: threshold in degrees, out; ret 8; al) at 0xFE13A4 (sprint, 105 degrees at
// 0x12DEF30) and 0xFE1499 (run, 105 at 0x12DEF2C); both calls go to the native-grid check, which hands back to
// 0xE10DB0 at the native rate and below.
bool switchback_install();
