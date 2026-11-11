#pragma once

// What we can learn about DSfix (Durante's DINPUT8.dll interceptor) without
// touching it. DSfix has its own FPS unlock that rewrites the same 1/30 step
// constant this mod does, so the two must not both drive the frame rate.
struct DsfixInfo {
    bool present = false;      // a DSfix.ini or a non-system dinput8.dll beside the game
    bool ini_found = false;
    bool unlock_fps = false;   // DSfix.ini has "unlockFPS 1"
    int fps_limit = 0;         // DSfix.ini "FPSlimit", 0 if not set
    bool borderless = false;   // DSfix.ini "borderlessFullscreen 1"
    bool skip_intro = false;   // DSfix.ini "skipIntro 1"
    bool dll_found = false;      // a DINPUT8.dll file beside the game (DSfix only acts when it loads)
    int filtering_override = 0;  // DSfix.ini "filteringOverride" (0 off, 1 trilinear, 2 anisotropic)
    int render_width = 1024;     // DSfix.ini "renderWidth" / "renderHeight" (1024x720 = the game's own)
    int render_height = 720;
};

DsfixInfo dsfix_probe(const wchar_t* game_dir);

// From the last dsfix_probe: DSfix is there and sets its own texture filtering / render resolution, so ours steps aside.
bool dsfix_filtering_override();
bool dsfix_changes_resolution();
// DSfix's render size from the last probe (1024x720 when it does not change it).
void dsfix_render_size(int* width, int* height);
