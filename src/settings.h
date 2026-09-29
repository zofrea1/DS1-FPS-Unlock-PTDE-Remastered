#pragma once

struct Settings {
    bool fps_unlock = true;
    int target_fps = 120;
    // Writes DSR-FPS-Unlock-trace.csv with per-character update samples. Off by
    // default: the file is large and the hook costs a little per update.
    bool trace = false;
};

// Reads DSR-FPS-Unlock.ini from the same directory as the DLL.
Settings settings_load(const wchar_t* dll_path);
