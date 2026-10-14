#pragma once

// Another mod built into the same xinput1_3.dll on top of this one (Prepare to Co-op Edition is). Such a build
// compiles this mod's sources and its own definition of ptde_extension(); the FPS Unlock on its own links
// extension_none.cpp, which has none. The calls come at fixed points of this mod's own start-up, so the
// bundled mod needs no other hooks into it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct IDirect3DDevice9;

struct PtdeExtension {
    // For the log, e.g. "Prepare to Co-op Edition v1.0.0".
    const char* name;
    // DllMain (process attach), after this mod's own early hooks and before the game runs any code.
    void (*attach)(HMODULE dll);
    // This mod's start-up thread, after its settings are logged and before it patches the game.
    void (*start)(HMODULE dll);
    // True: the Direct3D 9 device hooks go in even with FPSUnlock off (the extension draws or needs the device).
    bool (*wants_device)();
    // Each device the game creates, before this mod hooks the device itself.
    void (*device_created)(IDirect3DDevice9* device);
};

// nullptr when nothing is bundled.
const PtdeExtension* ptde_extension();

// This mod's version, for a bundled mod's log and title.
const char* ptde_fps_unlock_version();
