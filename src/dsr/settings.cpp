#include "settings.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

std::string trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool parse_bool(const std::string& v) {
    const std::string s = lower(trim(v));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

// true / false, or auto (kAuto).
int parse_auto(const std::string& v) {
    return lower(trim(v)) == "auto" ? kAuto : (parse_bool(v) ? 1 : 0);
}

// Remastered's own effect archives (sfx\, the Steam build, unchanged since 2022) and their sizes. The Prepare to Die
// Edition effects mod ("Dark Souls Remastered: PTDE Effects Fully Restored") replaces all 29, and every release of it
// (1.0.0, 1.1.0, 1.2.0) changes the size of each one, so an archive of another size counts as replaced.
struct EffectArchive {
    const wchar_t* name;
    unsigned long long size;
};
const EffectArchive kEffectArchives[] = {
    {L"FRPG_SfxBnd_CommonEffects.ffxbnd.dcx", 67445412u}, {L"FRPG_SfxBnd_Patch.ffxbnd.dcx", 1753816u},
    {L"FRPG_SfxBnd_c3471.ffxbnd.dcx", 395800u},           {L"FRPG_SfxBnd_c4100.ffxbnd.dcx", 189253u},
    {L"FRPG_SfxBnd_c4500.ffxbnd.dcx", 305495u},           {L"FRPG_SfxBnd_c4510.ffxbnd.dcx", 59953u},
    {L"FRPG_SfxBnd_m10.ffxbnd.dcx", 222762u},             {L"FRPG_SfxBnd_m10_00.ffxbnd.dcx", 1398024u},
    {L"FRPG_SfxBnd_m10_01.ffxbnd.dcx", 1394407u},         {L"FRPG_SfxBnd_m10_02.ffxbnd.dcx", 143556u},
    {L"FRPG_SfxBnd_m11.ffxbnd.dcx", 954271u},             {L"FRPG_SfxBnd_m12.ffxbnd.dcx", 1308938u},
    {L"FRPG_SfxBnd_m12_00.ffxbnd.dcx", 772223u},          {L"FRPG_SfxBnd_m12_01.ffxbnd.dcx", 1467396u},
    {L"FRPG_SfxBnd_m13.ffxbnd.dcx", 364045u},             {L"FRPG_SfxBnd_m13_00.ffxbnd.dcx", 180062u},
    {L"FRPG_SfxBnd_m13_01.ffxbnd.dcx", 282837u},          {L"FRPG_SfxBnd_m13_02.ffxbnd.dcx", 504686u},
    {L"FRPG_SfxBnd_m14.ffxbnd.dcx", 1860829u},            {L"FRPG_SfxBnd_m14_00.ffxbnd.dcx", 210593u},
    {L"FRPG_SfxBnd_m14_01.ffxbnd.dcx", 838249u},          {L"FRPG_SfxBnd_m15.ffxbnd.dcx", 123437u},
    {L"FRPG_SfxBnd_m15_00.ffxbnd.dcx", 417178u},          {L"FRPG_SfxBnd_m15_01.ffxbnd.dcx", 677151u},
    {L"FRPG_SfxBnd_m16.ffxbnd.dcx", 1653641u},            {L"FRPG_SfxBnd_m17.ffxbnd.dcx", 1069320u},
    {L"FRPG_SfxBnd_m18.ffxbnd.dcx", 194263u},             {L"FRPG_SfxBnd_m18_00.ffxbnd.dcx", 242470u},
    {L"FRPG_SfxBnd_m18_01.ffxbnd.dcx", 125679u},
};
constexpr int kEffectArchiveCount = static_cast<int>(sizeof(kEffectArchives) / sizeof(kEffectArchives[0]));

// The switches set to auto take the original game's look (1) when more than half of the effect archives in the game
// folder's sfx\ are replaced (the effects mod replaces all of them; a mod that changes a few effects is not taken for
// it), else Remastered's (0). Logs what it found.
void settings_resolve_effect_looks(Settings& s, const wchar_t* dir) {
    int* const looks[] = {&s.sfx_distortion_ptde_speed, &s.sfx_sway_ptde_speed, &s.sfx_trails_ptde_speed};
    static const char* const kNames[] = {"SfxDistortionPtdeSpeed", "SfxSwayPtdeSpeed", "SfxTrailsPtdeSpeed"};
    bool any_auto = false;
    for (int* v : looks) any_auto = any_auto || *v == kAuto;
    if (!any_auto) return;
    int found = 0, replaced = 0;
    for (const EffectArchive& a : kEffectArchives) {
        wchar_t file[MAX_PATH];
        _snwprintf_s(file, _TRUNCATE, L"%ssfx\\%s", dir, a.name);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(file, GetFileExInfoStandard, &info)) continue;
        ++found;
        const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size != a.size) ++replaced;
    }
    const bool ptde = replaced * 2 > kEffectArchiveCount;
    std::string autos;
    for (int i = 0; i < 3; ++i) {
        if (*looks[i] != kAuto) continue;
        *looks[i] = ptde ? 1 : 0;
        autos += autos.empty() ? kNames[i] : std::string(", ") + kNames[i];
    }
    if (ptde) {
        LOG_INFO("Effects: %d of Remastered's %d effect archives are replaced: the PTDE effects mod. %s (auto): the "
                 "original game's look (30 FPS), as its effects were made", replaced, kEffectArchiveCount,
                 autos.c_str());
    } else {
        LOG_INFO("Effects: %d of Remastered's %d effect archives found, %d replaced%s. %s (auto): Remastered's own "
                 "look (60 FPS)", found, kEffectArchiveCount, replaced,
                 replaced > 0 ? " (not the PTDE effects mod, which replaces all of them)" : "", autos.c_str());
    }
}

// MaxFPS: a whole number, clamped to kMinMaxFps..kMaxMaxFps. Anything that is not a number keeps
// the default.
int parse_max_fps(const std::string& v, int fallback) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) {
        LOG_ERROR("Settings::read - MaxFPS \"%s\" is not a number, using %d", v.c_str(), fallback);
        return fallback;
    }
    const long clamped = n < kMinMaxFps ? kMinMaxFps : (n > kMaxMaxFps ? kMaxMaxFps : n);
    if (clamped != n) {
        LOG_INFO("Settings::read - MaxFPS %ld is outside %d to %d, using %ld", n, kMinMaxFps, kMaxMaxFps, clamped);
    }
    return static_cast<int>(clamped);
}

}  // namespace

Settings settings_read(const wchar_t* dll_path) {
    Settings s;
    wchar_t path[MAX_PATH];
    lstrcpynW(path, dll_path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) {
        slash[1] = 0;
    }
    wchar_t full[MAX_PATH];
    _snwprintf_s(full, _TRUNCATE, L"%sDSR-FPS-Unlock.ini", path);

    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, full, -1, narrow, MAX_PATH, nullptr, nullptr);
    LOG_INFO("Settings::read - reading INI from %s", narrow);

    std::ifstream in(full);
    if (!in) {
        LOG_ERROR("Settings::read - INI read failed, using defaults");
        return s;
    }

    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "fpsunlock") {
            s.fps_unlock = parse_bool(val);
        } else if (key == "settingsmenu") {
            s.settings_menu = parse_bool(val);
        } else if (key == "settingsmenukey") {
            // F1 to F12, or none.
            const std::string v = lower(trim(val));
            const int n = (v.size() >= 2 && v[0] == 'f') ? std::atoi(v.c_str() + 1) : 0;
            s.settings_menu_key = (n >= 1 && n <= 12) ? 0x70 + n - 1 : 0;
        } else if (key == "maxfps" || key == "targetfps") {
            s.target_fps = parse_max_fps(val, s.target_fps);
        } else if (key == "variableframetime") {
            s.variable_frame_time = parse_bool(val);
        } else if (key == "menuinputfilter") {
            s.menu_input_filter = parse_bool(val);
        } else if (key == "bonfirefix" || key == "bonfireunstick") {
            s.bonfire_unstick = parse_bool(val);
        } else if (key == "startoffline" || key == "disableonlinemode") {
            s.start_offline = parse_bool(val);
        } else if (key == "skipintro" || key == "introskip") {
            s.skip_intro = parse_bool(val);
        } else if (key == "trace") {
            s.trace = parse_bool(val);
        } else if (key == "fixstepup") {
            s.fix_step_up = parse_bool(val);
        } else if (key == "fixstepdown") {
            s.fix_step_down = parse_bool(val);
        } else if (key == "fixladderslide") {
            s.fix_ladder_slide = parse_bool(val);
        } else if (key == "lockonptdespeed" || key == "cameraptdespeed") {
            s.camera_ptde_speed = parse_bool(val);
        } else if (key == "slidefrictionptde") {
            s.slide_ptde = parse_bool(val);
        } else if (key == "grazeptde") {
            s.graze_ptde = parse_bool(val);
        } else if (key == "stepdownptde") {
            s.step_down_ptde = parse_bool(val);
        } else if (key == "stepheightptde") {
            s.step_height_ptde = parse_bool(val);
        } else if (key == "airdampingptde") {
            s.air_damping_ptde = parse_bool(val);
        } else if (key == "bulletptdespeed") {
            s.bullet_ptde_speed = parse_bool(val);
        } else if (key == "fixcamera") {
            s.fix_camera = parse_bool(val);
        } else if (key == "watch") {
            s.watch = parse_bool(val);
        } else if (key == "inputlog") {
            s.input_log = parse_bool(val);
        } else if (key == "fixdpadhold") {
            s.fix_dpad_hold = parse_bool(val);
        } else if (key == "fixslidecontact") {
            s.fix_slide_contact = parse_bool(val);
        } else if (key == "fixslidesteer") {
            s.fix_slide_steer = parse_bool(val);
        } else if (key == "slideptdespeed") {
            s.slide_ptde_speed = parse_bool(val);
        } else if (key == "fixstickflick") {
            s.fix_stick_flick = parse_bool(val);
        } else if (key == "stickinputptdespeed" || key == "stickflickptdespeed") {
            s.stick_input_ptde_speed = parse_bool(val);
        } else if (key == "fixswitchback") {
            s.fix_switchback = parse_bool(val);
        } else if (key == "fixrolldirection") {
            s.fix_roll_direction = parse_bool(val);
        } else if (key == "fixspeffecttiming") {
            s.fix_speffect_timing = parse_bool(val);
        } else if (key == "speffectptdespeed") {
            s.speffect_ptde_speed = parse_bool(val);
        } else if (key == "fixbullethittiming") {
            s.fix_bullet_hit_timing = parse_bool(val);
        } else if (key == "fixeventwaits") {
            s.fix_event_waits = parse_bool(val);
        } else if (key == "eventwaitptdespeed") {
            s.event_wait_ptde_speed = parse_bool(val);
        } else if (key == "fixsfxdistortion") {
            s.fix_sfx_distortion = parse_bool(val);
        } else if (key == "sfxdistortionptdespeed") {
            s.sfx_distortion_ptde_speed = parse_auto(val);
        } else if (key == "fixsfxsway") {
            s.fix_sfx_sway = parse_bool(val);
        } else if (key == "sfxswayptdespeed") {
            s.sfx_sway_ptde_speed = parse_auto(val);
        } else if (key == "fixsfxtrails") {
            s.fix_sfx_trails = parse_bool(val);
        } else if (key == "sfxtrailsptdespeed") {
            s.sfx_trails_ptde_speed = parse_auto(val);
        } else if (key == "fixsfxspawnrate") {
            s.fix_sfx_spawn_rate = parse_bool(val);
        } else if (key == "fixsoulcounter") {
            s.fix_soul_counter = parse_bool(val);
        } else if (key == "fixstaminatick") {
            s.fix_stamina_tick = parse_bool(val);
        } else if (key == "hudptdespeed") {
            s.hud_ptde_speed = parse_bool(val);
        } else if (key == "fixui") {
            s.fix_ui = parse_bool(val);
        } else if (key == "fixgraze") {
            s.fix_graze = parse_bool(val);
        } else if (key == "fixdamping") {
            s.fix_damping = parse_bool(val);
        } else if (key == "fixslide") {
            s.fix_slide = parse_bool(val);
        } else if (key == "fixjump") {
            s.fix_jump = parse_bool(val);
        } else if (key == "fixlockonturn") {
            s.fix_lock_on_turn = parse_bool(val);
        } else if (key == "fixfootik") {
            s.fix_foot_ik = parse_bool(val);
        } else if (key == "footikptdespeed") {
            s.foot_ik_ptde_speed = parse_bool(val);
        } else if (key == "fixghosts") {
            s.fix_ghosts = parse_bool(val);
        } else if (key == "fixtimers") {
            s.fix_timers = parse_bool(val);
        } else if (key == "fixsmoothing") {
            s.fix_smoothing = parse_bool(val);
        } else if (key == "fixvelocity") {
            s.fix_velocity = parse_bool(val);
        } else if (key == "fixscriptmemory") {
            s.fix_script_memory = parse_bool(val);
        } else if (key == "fixmovedt") {
            s.fix_move_dt = parse_bool(val);
        }
    }
    return s;
}

bool settings_effects_mod_installed(const wchar_t* dir) {
    int replaced = 0;
    for (const EffectArchive& a : kEffectArchives) {
        wchar_t file[MAX_PATH];
        _snwprintf_s(file, _TRUNCATE, L"%ssfx\\%s", dir, a.name);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(file, GetFileExInfoStandard, &info)) continue;
        const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size != a.size) ++replaced;
    }
    return replaced * 2 > kEffectArchiveCount;
}

Settings settings_load(const wchar_t* dll_path) {
    Settings s = settings_read(dll_path);
    // The game folder (the DLL sits in it).
    wchar_t dir[MAX_PATH];
    lstrcpynW(dir, dll_path, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;
    settings_resolve_effect_looks(s, dir);
    return s;
}
