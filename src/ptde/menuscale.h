#pragma once

// The game's menus and HUD at their source (menuscale.cpp): hooks on the menu device context (NS_FRPG::FrpgMenuDc)
// and the dialogs' draw (EzMenuDialog::Draw). False when this build's code is not as expected.
bool menuscale_install();
bool menuscale_installed();
// UIScale and UIScaleWorld (percent, 100 = the game's size, 25..200 taken; a negative value keeps that one), and
// UIScaleText: the letters of the texts in those dialogs (0 = at their dialog's size, else percent of the game's).
void menuscale_set(int ui_scale, int ui_scale_world, int ui_scale_text = -1);
// RemoteControl `menuprobe N`: the next N calls of the context's transform and clipping methods in the log.
void menuscale_probe(int n);
// RemoteControl `menudlg N`: the next N dialog draws in the log (class, current and file rectangles), and the next N
// texts begun (font size, the dialog, the letters' scale).
void menuscale_probe_dialogs(int n);
