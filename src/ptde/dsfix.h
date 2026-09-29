#pragma once

// What we can learn about DSfix (Durante's DINPUT8.dll interceptor) without
// touching it. DSfix has its own FPS unlock that rewrites the same 1/30 step
// constant this mod does, so the two must not both drive the frame rate.
struct DsfixInfo {
    bool present = false;      // a DSfix.ini or a non-system dinput8.dll beside the game
    bool ini_found = false;
    bool unlock_fps = false;   // DSfix.ini has "unlockFPS 1"
    int fps_limit = 0;         // DSfix.ini "FPSlimit", 0 if not set
};

DsfixInfo dsfix_probe(const wchar_t* game_dir);
