#pragma once

// Start-up autosave notice and logos. Title step 0 (0xEE6AC0) opens the autosave notice (system message 45000,
// "This game uses an autosave function..."), which step 1 (0xEE62C0) keeps for 10 s or until a button; with
// SkipIntro it is never opened and step 1 goes straight on (see intro.cpp).
// The title screen (TitleStep_forWin32, its step functions in the table at 0x1378FE8) shows the
// autosave notice, checks the save data, then its step 37 (0xEE5900) sets MenuMan+0x10C to 1, which opens the
// logo dialog (FrpgMenuDlgLogo: logo_BNG, logo_from, logo_havok, each faded in, held and faded out); step 38
// (0xEE5870) waits until the dialog's last logo sets it back to 0, then the title screen follows. With SkipIntro
// the write is left out: the dialog never opens and step 38 goes straight on. The console title step
// (0xEE92A0 / 0xEE91D0) has the same pair and is patched too. Nothing depends on timing, unlike DSfix's
// skipIntro, which speeds up the game's clock while it sees the Bandai Namco logo.
bool intro_install(bool skip);
