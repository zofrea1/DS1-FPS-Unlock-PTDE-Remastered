#include "dsfix.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// True when the dinput8.dll the game loaded lives beside the game instead of in
// the system directory, which is how DSfix (or any other interceptor) shows up.
bool non_system_dinput8(const wchar_t* game_dir) {
    HMODULE module = GetModuleHandleW(L"dinput8.dll");
    if (!module) {
        return false;
    }
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(module, path, MAX_PATH)) {
        return false;
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    if (_wcsnicmp(path, sys, wcslen(sys)) == 0) {
        return false;
    }
    const size_t len = wcslen(game_dir);
    return _wcsnicmp(path, game_dir, len) == 0;
}

}  // namespace

namespace {
DsfixInfo g_last;
bool g_yield = false;
}  // namespace

void dsfix_set_yield(bool on) { g_yield = on; }
bool dsfix_yields() { return g_yield && dsfix_loaded(); }

bool dsfix_filtering_override() { return g_last.dll_found && g_last.ini_found && g_last.filtering_override != 0; }
bool dsfix_loaded() { return g_last.dll_found && g_last.ini_found; }

bool dsfix_changes_resolution() {
    return g_last.dll_found && g_last.ini_found && (g_last.render_width != 1024 || g_last.render_height != 720);
}

void dsfix_render_size(int* width, int* height) {
    *width = g_last.render_width;
    *height = g_last.render_height;
}

DsfixInfo dsfix_probe_impl(const wchar_t* game_dir);

DsfixInfo dsfix_probe(const wchar_t* game_dir) {
    g_last = dsfix_probe_impl(game_dir);
    return g_last;
}

DsfixInfo dsfix_probe_impl(const wchar_t* game_dir) {
    DsfixInfo info;
    wchar_t ini[MAX_PATH];
    _snwprintf_s(ini, _TRUNCATE, L"%sDSfix.ini", game_dir);
    std::ifstream in(ini);
    if (in) {
        info.ini_found = true;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream words(line);  // DSfix.ini is "name value", no '='
            std::string key;
            std::string value;
            words >> key >> value;
            if (key == "unlockFPS") {
                info.unlock_fps = std::atoi(value.c_str()) != 0;
            } else if (key == "FPSlimit") {
                info.fps_limit = std::atoi(value.c_str());
            } else if (key == "borderlessFullscreen") {
                info.borderless = std::atoi(value.c_str()) != 0;
            } else if (key == "filteringOverride") {
                info.filtering_override = std::atoi(value.c_str());
            } else if (key == "renderWidth") {
                info.render_width = std::atoi(value.c_str());
            } else if (key == "renderHeight") {
                info.render_height = std::atoi(value.c_str());
            } else if (key == "skipIntro") {
                info.skip_intro = std::atoi(value.c_str()) != 0;
            }
        }
    }
    {
        wchar_t dll[MAX_PATH];
        lstrcpynW(dll, game_dir, MAX_PATH);
        lstrcatW(dll, L"DINPUT8.dll");
        info.dll_found = GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES;
    }
    info.present = info.ini_found || info.dll_found || non_system_dinput8(game_dir);
    return info;
}
