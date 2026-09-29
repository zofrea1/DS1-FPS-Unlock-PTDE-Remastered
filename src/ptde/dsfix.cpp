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

DsfixInfo dsfix_probe(const wchar_t* game_dir) {
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
            }
        }
    }
    info.present = info.ini_found || non_system_dinput8(game_dir);
    return info;
}
