#pragma once

struct Settings {
    bool fps_unlock = true;
    // Frames per second the game should aim for. PTDE runs at 30 FPS natively.
    int target_fps = 60;
};

// Reads PTDE-FPS-Unlock.ini from the same directory as the DLL.
Settings settings_load(const wchar_t* dll_path);
