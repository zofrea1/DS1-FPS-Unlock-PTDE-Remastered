#pragma once

// FPS Unlock Settings: an in-game page with the settings a player would change while playing (see settingsmenu.cpp).

struct IDirect3DDevice9;

// After the settings are read (start-up thread). key: the virtual key that opens the page (0 = none); the
// PC settings screen opens it with Y on a controller either way.
void settingsmenu_install(const wchar_t* ini_path, int key);
bool settingsmenu_enabled();
// The device's Present, before the frame goes out (render thread): reads the keys, draws the page.
void settingsmenu_present(IDirect3DDevice9* device);
// Before the device's Reset.
void settingsmenu_reset();
// Every XInput read (xinput_proxy.cpp): takes the buttons for the page, and gives the game none while it is open.
void settingsmenu_pad(unsigned long index, void* state, unsigned long result);
bool settingsmenu_is_open();
void settingsmenu_set_open(bool open);  // RemoteControl `settings 0|1`

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
void settingsmenu_add_page(const wchar_t* title, const SettingsMenuRow* rows, int count);
