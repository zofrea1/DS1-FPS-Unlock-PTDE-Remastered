#pragma once

#include <functional>
#include <string>

// MaxFPS range. A value outside it is clamped to the nearest end.
constexpr int kMinMaxFps = 10;
constexpr int kMaxMaxFps = 1000;

struct Settings {
    // Master switch: false leaves the game unchanged (every fix and diagnostic off), except skip_intro.
    bool fps_unlock = true;
    // Frames per second the game should aim for (a cap), kMinMaxFps..kMaxMaxFps. PTDE runs at
    // 30 FPS natively.
    int target_fps = 120;
    // true: wait at the start of the frame, before input is read. false: wait just before Present.
    bool fps_limit_early = true;
    // Overwrite the engine's fixed 1/30 step with the measured frame time.
    bool driver = true;
    // Change the game's swap-interval bookkeeping the way DSfix does (2 -> 5) so the
    // render thread stops waiting for two vertical blanks per frame.
    bool vblank_patch = true;
    // Exclusive fullscreen refresh rate: -1 leaves the game's choice alone, 0 uses the
    // highest rate the display offers at the game's resolution, N asks for N Hz.
    int fullscreen_refresh = -1;
    // Diagnostic: a [perf] line every 5 s (frame, game update and render times, thread CPU use, Direct3D calls per
    // frame); F10 samples every busy thread for 10 s and logs where the time goes (see profile.h).
    bool profile = false;
    // Development remote control (remote.h): commands from fpsu-cmd.txt beside the game. Diagnostics builds only.
    bool remote_control = false;
    // Frame-rate independence fixes for constants tuned for 30 FPS (see fixes.h).
    bool fix_slide = true;
    bool fix_damping = true;
    bool fix_timers = true;
    bool fix_smoothing = true;
    // Velocities the game derives from one frame's movement times 30 (powered ragdoll, 3D sound Doppler).
    bool fix_velocity = true;
    // Script garbage collection at least every 15 (AI) / 30 frames, as at 30 FPS (see script.h). Without it the
    // script heap can fill at a high frame rate in a crowded area and the game closes itself.
    bool fix_script_memory = true;
    bool fix_graze = true;
    // Ground snap-down reach and the ladder-exit cap (INI FixStepDown; FixLadder is the old name).
    bool fix_step_down = true;
    bool fix_camera = true;
    // Lock-on body turn: the torso, head and root bones turn toward the target at the original speed.
    bool fix_lock_on_turn = true;
    // Foot IK (feet on stairs and slopes, the pelvis lowered to reach them) at the original speed (see footik.h).
    bool fix_foot_ik = true;
    // Bloodstain and wandering ghost replays play, and the player's replay data is recorded, in real time.
    bool fix_ghosts = true;
    // Effect particle spawn intervals below 1/30 s are raised to 1/30 s as effects load (Remastered's rule),
    // so emitters spawn as densely as at 30 FPS.
    bool fix_sfx_spawn_rate = true;
    bool fix_sfx_actions = true;
    // Heat haze (distortion effects) scrolls on the 30 FPS frame grid, as it was authored (see sfx.h).
    bool fix_sfx_distortion = true;
    // Internal render resolution of the 3D scene and the HUD (resolution.h): -1 = auto (the monitor's resolution),
    // 0 = the game's own 1024x720 (HUD 1280x720), else width x height.
    int render_width = -1;
    int render_height = -1;
    // Renders this many times the resolution above in each direction and scales the picture down (anti-aliasing).
    float supersampling = 1.0f;
    bool render_scale_post = false;
    // Anisotropic filtering level for surface textures (0/1 = the game's own filtering).
    int anisotropic_filtering = 16;
    // The mouse pointer hides over the game window after two seconds without moving (cursor.h).
    bool hide_mouse_cursor = true;
    // With DSfix's render resolution: the sun and moon no longer stay hidden after something passes in front.
    bool fix_sun_with_dsfix = true;  // experiment: scale the post-processing targets too
    // HUD gauge fill speed (HP, stamina, boss bars) and the loading screen swirl.
    bool fix_ui = true;
    // Sprint stamina drain takes one point every 0.1 s on average at any frame rate (the game rounded each
    // tick up to a whole frame and dropped the remainder).
    bool fix_stamina_tick = true;
    // Steep slides: keep the script's contact bit for 1/30 s after the last real contact (see slidecontact.h).
    bool fix_slide_contact = true;
    // Slide steering: scale Havok's root-motion threshold for a one-frame-old turn layer (see slidesteer.h).
    bool fix_slide_steer = true;
    // In-engine cutscene cameras blend between their 30 FPS keys (see cutscenecam.h).
    bool fix_cutscene_camera = true;
    // HUD soul counter steps with the frame time, at most four of the game's steps per update (see soulcounter.h).
    bool fix_soul_counter = true;
    // Kick (forward + R1) and jumping attack (forward + R2): the stick flick keeps the original game's 30 FPS
    // timing in real time (see flick.h and stickflick.h).
    bool fix_stick_flick = true;
    // Roll direction: the game's short memory of the stick (5 entries of 1/30 s) covers the same time at any frame
    // rate (see roll.h and rollhistory.h).
    bool fix_roll_direction = true;
    // Switchback (reversal) animation: the turn it looks for is measured over the original game's frames
    // (see switchback.h and turnhistory.h).
    bool fix_switchback = true;
    // SpEffect lifetimes expire on the original game's 30 FPS frames (see speffect.h and effectlife.h): status
    // build-up from effects that are applied again and again (Blighttown's swamp) at the original rate.
    bool fix_speffect_timing = true;
    // Bullet hit records run out on the original game's 30 FPS frames (see hitrecord.h and effectlife.h): Poison
    // and Toxic Mist clouds and other lingering attacks hit a target standing in them at the original rate.
    bool fix_bullet_hit_timing = true;
    // Stand the character up if it stays seated at a bonfire with no menu open (reverse hollowing bug).
    bool bonfire_unstick = true;
    // Diagnostic: log the HUD shortcut (D-pad) actions with timestamps.
    bool input_log = false;
    // Diagnostic: F9 records the player's state to a CSV; F4/F5/F6/F7 switch the frame
    // rate live to 30/60/120/240 FPS.
    bool trace = false;
    // Diagnostic: log the dragons' acid and the pool it leaves ([bullet] lines, see bullettrace.h).
    bool bullet_trace = false;
    // Diagnostic: an effect id whose first emitter timing block gets hardware watchpoints (see sfxwatch.h); 0 = off.
    int sfx_watch = 0;
    // Diagnostic: hash every vertex shader constant upload to count presents whose content did
    // not change. Costs noticeable CPU per frame; leave off for normal play.
    bool content_probe = false;
    // Diagnostic: hardware write watchpoints on player fields, e.g. "c:1B8,p:180". F8 arms/reports.
    char watch[96] = {};
    // Disable the game's catch-up loop that re-presents a frame until the display's
    // vblank counter reaches its 30 FPS schedule.
    bool skip_present_repeat = false;
    // Run Direct3D 9 on Vulkan through DXVK (dxvk_d3d9.dll beside this DLL). Off by default.
    bool dxvk = false;
    // DXVK may take exclusive fullscreen (dxvk.allowFse), which G-SYNC / variable refresh needs under DXVK (dxvk.h):
    // 0 auto (only while Windows HDR is off for the display), 1 always, 2 never.
    int dxvk_exclusive_fullscreen = 0;
    // DXVK's frames go out through a DXGI flip-model swap chain instead of a Vulkan one (dxgipresent.h): HDR stays on
    // and variable refresh works without exclusive fullscreen. DxvkExclusiveFullscreen then has nothing to do.
    bool dxvk_dxgi_present = true;
    // The game's brightness gamma ramp (gamma.h): 0 auto and 1 passed on (as the system Direct3D 9 does), 2 never.
    int gamma_ramp = 0;
    // Render thread CPU (see d3dperf.h): the render target the game reads back every frame is copied two frames late
    // instead of waiting for the GPU, and sampler state calls that repeat the value in place skip Direct3D.
    bool async_readback = true;
    bool filter_sampler_states = true;
    // Vertex shader constants that repeat what the device holds skip Direct3D (see d3dperf.h). Off by default: in the
    // game under 1% of the registers repeat, so the comparison cost more than it saved.
    bool filter_shader_constants = false;
    // Depth-stencil and offscreen surfaces made and released every frame are kept and handed out again (surfpool.h).
    bool reuse_surfaces = true;
    // The game's read locks count without kernel calls (rwlock.h).
    bool fast_read_locks = true;
    // Textures go to the graphics card on the loading thread as they are made, not at their first draw (texpreload.h).
    bool preload_textures = true;
    // The effects library's debug checks (stack fills and guard checks, see ffx.h) are skipped.
    bool fast_effects = true;
    // Two hot render-thread math helpers run as SSE with identical results (see rendermath.h).
    bool fast_render_math = true;
    // Make the game window borderless and cover its monitor. Needs the game set to
    // windowed mode. Skipped if DSfix's own borderlessFullscreen is on.
    bool borderless = false;
    // Per-monitor DPI aware (set in DllMain before the game makes its window): real pixels on scaled displays.
    bool dpi_aware = true;
    // Keep a real XInput gamepad when the game's four Windows 7 device ids do not match.
    bool fix_controller = true;
    // Keep the game thread's Steam calls out of the Steam client's regular pause (steamguard.h).
    bool steam_pause_guard = true;
    // Fill a display taller than 16:9 (16:10, the Steam Deck's 1280x800). The horizontal
    // view stays the same and the extra height shows more of the world. A 16:9 display
    // is left alone. Missing key keeps this on.
    bool fix_aspect = true;
    // Skip the autosave notice and the Bandai Namco, FromSoftware and Havok logos at start-up (see intro.h).
    // Independent of FPSUnlock.
    bool skip_intro = true;
    // Log a per-command summary for this many seconds after start (0 = off).
    int survey_seconds = 0;
};

// Reads PTDE-FPS-Unlock.ini from the same directory as the DLL.
Settings settings_load(const wchar_t* dll_path);

// Every key of PTDE-FPS-Unlock.ini in file order (key in lower case, value trimmed, # comments cut; section
// headers carry no meaning). A mod built into this DLL keeps its own keys in the same file (see extension.h).
// false when the file cannot be read.
bool settings_read_keys(const wchar_t* dll_path,
                        const std::function<void(const std::string& key, const std::string& value)>& fn);
// true for 1, true, yes or on (any case).
bool settings_parse_bool(const std::string& value);
