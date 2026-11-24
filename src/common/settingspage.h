#pragma once

#include <cstdint>

// FPS Unlock Settings, the part both games share (settingspage.cpp): the pages and their rows, the INI they are written
// to, the panel drawn with GDI into a 32-bit picture, and the controls. Each game draws that picture over its frame
// (Prepare to Die Edition: Direct3D 9, src/ptde/settingsmenu.cpp; Remastered: Direct3D 11, src/dsr/settingsmenu.cpp)
// and hands in its controller and keyboard.

// A row: an INI key and its values. A bundled mod (Prepare to Co-op Edition) adds a page of its own.
struct SettingsMenuChoice {
    const char* ini;       // the value written to the INI
    const wchar_t* label;  // shown
};
struct SettingsMenuRow {
    const char* key;  // INI key
    const wchar_t* label;
    const wchar_t* help;
    const SettingsMenuChoice* choices;
    int choice_count;
    // Applied at once (live), else "after a restart". Called on the render thread with the INI value.
    void (*apply)(const char* ini_value);
    bool text;  // free text typed on the keyboard (choices unused), e.g. a password
    const wchar_t* when;  // when it applies, if not the usual ("Applies at once" / "after a restart")
};

// A picture of the page: 32-bit pixels (B, G, R, A in memory, alpha set), top row first. version changes whenever the
// pixels do.
struct SettingsPageImage {
    int w = 0, h = 0;
    const uint32_t* bits = nullptr;
    unsigned version = 0;
};

// What the game hands in each frame.
struct SettingsPageInput {
    uint16_t pad = 0;           // XInput buttons of every controller together
    int lx = 0, ly = 0;         // the left stick furthest from the middle
    bool keyboard = false;      // the game window has the focus (its keyboard counts)
    bool (*key)(int vk) = nullptr;  // a key down, as the game reads its keyboard (virtual key codes)
    bool pad_opens = false;     // Y opens the page now (the game's PC settings screen is up)
};

// After the settings are read. key: the virtual key that opens the page (0 = none). blank(on): the game gets no
// controller, keyboard or mouse input while on (from opening until every button is let go after closing).
void settingspage_init(const wchar_t* ini_path, int key, void (*blank)(bool on));
void settingspage_add_page(const wchar_t* title, const SettingsMenuRow* rows, int count);
bool settingspage_enabled();
int settingspage_key();
bool settingspage_is_open();
void settingspage_set_open(bool open);
// Every frame (the render thread): INI writes that are still owed, then the controls.
void settingspage_frame(const SettingsPageInput& in);
// The panel for a screen SCREEN_W x SCREEN_H (drawn again when something changed), and the hint line LINE.
const SettingsPageImage* settingspage_panel(int screen_w, int screen_h);
const SettingsPageImage* settingspage_hint(int screen_w, int screen_h, const wchar_t* line);
