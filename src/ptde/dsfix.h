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
    int dof_override = 0;        // DSfix.ini "dofOverrideResolution" (0: its 512x360 targets keep their size)
    int aa_quality = 1;          // DSfix.ini "aaQuality" (0 off); when not listed, taken as on
    bool aa_smaa = true;         // DSfix.ini "aaType SMAA" (else FXAA); when not listed, taken as SMAA
};

DsfixInfo dsfix_probe(const wchar_t* game_dir);

// From the last dsfix_probe: DSfix is there and sets its own texture filtering / render resolution, so ours steps aside.
bool dsfix_filtering_override();
bool dsfix_changes_resolution();
// DSfix's DLL is beside the game (it wraps the game's Direct3D device when it loads).
bool dsfix_loaded();
// DSfix's render size from the last probe (1024x720 when it does not change it).
void dsfix_render_size(int* width, int* height);
// DSfix's dofOverrideResolution from the last probe (0 when unset or DSfix is not loaded).
int dsfix_dof_override();
// DSfix's SMAA runs on a scene of `width` x `height`: DSfix is loaded with aaType SMAA (aaQuality > 0), and its
// anti-aliasing finds the scene, which it only does at its own renderWidth x renderHeight (and at 1024x720 and 1280x720,
// which it enlarges). Its last pass reads the scene with sRGB decoding, which only 8-bit targets have.
bool dsfix_smaa_on_scene(int width, int height);
// YieldToDSFix: with DSfix loaded, this mod's picture settings that DSfix has its own of step back (the resolution with
// what rides on it, anisotropic filtering, the HUD's size). dsfix_yields() is true only with both. Set from DllMain.
void dsfix_set_yield(bool on);
bool dsfix_yields();
