#include "intro.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

// mov dword ptr [eax+10Ch], edx (edx = 1, MenuMan in eax): "show the logos". The add [ecx+8], edx after it
// moves the title step on and stays.
constexpr uint32_t kLogoOpenSites[2] = {0x00EE5919, 0x00EE92B9};
constexpr uint8_t kLogoOpen[6] = {0x89, 0x90, 0x0C, 0x01, 0x00, 0x00};
constexpr uint8_t kNops[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};  // nop word ptr [eax+eax+0]

// Title step 0 (0xEE6AC0) opens the autosave notice (system message 45000) from 0xEE6B2D on: mov eax,[MenuMan],
// then MenuMan+0xB4 = 1, the dialog record at [MenuMan+0x9E8] = {45000, 1, 0} and the dialog call 0xC7EE00, with
// edi = 1 for the step's add at 0xEE6B68. Step 1 (0xEE62C0) waits while a menu is open, closes the notice after
// 10 s, and moves on once no menu is open and the notice was answered or its time ran out (step+0x5C). With the
// notice skipped the step jumps over that block, marks the time as run out and moves on with edi = 1; step 1 then
// goes straight to the save data check. The console title step (0xEEA4C0) is laid out differently and not used
// on PC.
constexpr uint32_t kNoticeOpenSite = 0x00EE6B2D;
constexpr uint8_t kNoticeOpen[5] = {0xA1, 0xD0, 0x86, 0x37, 0x01};  // mov eax, [013786D0h]
uint32_t g_notice_resume = 0x00EE6B68;                              // add [esi+8], edi

__declspec(naked) void skip_notice() {
    __asm {
        mov byte ptr [esi + 0x5C], 1  // step 1: the notice's time has run out
        mov edi, 1                    // the step's increment
        jmp dword ptr [g_notice_resume]
    }
}

bool patch(uint32_t va, const uint8_t* expect, const uint8_t* repl, size_t n) {
    auto* p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(va));
    __try {
        if (std::memcmp(p, expect, n) != 0) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, repl, n);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    DWORD ignored = 0;
    VirtualProtect(p, n, old, &ignored);
    return true;
}

bool patch_jump(uint32_t va, const uint8_t* expect, size_t n, const void* target) {
    uint8_t code[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (va + 5));
    std::memcpy(code + 1, &rel, 4);
    return n == sizeof(code) && patch(va, expect, code, n);
}

}  // namespace

bool intro_install(bool skip) {
    if (!skip) {
        LOG_INFO("SkipIntro: false (the autosave notice and the start-up logos are shown)");
        return true;
    }
    int done = 0;
    for (uint32_t site : kLogoOpenSites) {
        if (patch(site, kLogoOpen, kNops, sizeof(kLogoOpen))) ++done;
    }
    const bool notice = patch_jump(kNoticeOpenSite, kNoticeOpen, sizeof(kNoticeOpen), reinterpret_cast<void*>(&skip_notice));
    if (done == 0 && !notice) {
        LOG_ERROR("SkipIntro: the title screen's steps did not match this build; the notice and the logos are shown");
        return false;
    }
    LOG_INFO("SkipIntro: the autosave notice is %s; the start-up logos are skipped (%d of 2 title steps patched)",
             notice ? "skipped" : "shown (its step did not match this build)", done);
    return true;
}
