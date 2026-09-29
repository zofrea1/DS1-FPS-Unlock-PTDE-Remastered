#include "patches.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

// Dark Souls: Prepare to Die Edition, Steam build of 2015-06-15 (the Steamworks
// release, no GFWL). 32-bit, image base 0x400000.
constexpr uint32_t kTimestamp = 0x557F2FD0;
constexpr uint32_t kImageSize = 0x011C2000;

// The engine's fixed 30 FPS frame step is one float in .rdata. The compiler shares
// it among every use of the literal 1/30, so it is loaded directly by code all over
// the game (37 references in this build) instead of coming from a single variable.
constexpr uint32_t kTimestepRva = 0x00DE7E90;
constexpr int kTimestepRefs = 37;

}  // namespace

bool patches_probe(const Settings& settings) {
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LOG_ERROR("Could not read the game image");
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        LOG_ERROR("DARKSOULS.exe is not the expected 32-bit image");
        return false;
    }
    LOG_INFO("EXE timestamp=%08lX size=%08lX", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    if (nt->FileHeader.TimeDateStamp != kTimestamp || nt->OptionalHeader.SizeOfImage != kImageSize) {
        LOG_ERROR("This DARKSOULS.exe is not the supported 2015 Steam build. Nothing will be changed.");
        return false;
    }
    LOG_INFO("Recognized Dark Souls: Prepare to Die Edition (Steam, 2015-06-15)");

    const auto* step = reinterpret_cast<const float*>(base + kTimestepRva);
    LOG_INFO("Timestep constant at %p = %.9f (expected 1/30 = %.9f)", step, *step, 1.0f / 30.0f);
    if (std::fabs(*step - 1.0f / 30.0f) > 1e-7f) {
        LOG_ERROR("The timestep constant does not hold 1/30. Another tool may already have changed it "
                  "(DSfix with unlockFPS does exactly that).");
    }

    // Count 4-byte absolute references to that constant in .text.
    const auto* section = IMAGE_FIRST_SECTION(nt);
    int refs = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(section[i].Name, ".text", 5) != 0) {
            continue;
        }
        const uint8_t* code = base + section[i].VirtualAddress;
        const uint32_t size = section[i].Misc.VirtualSize;
        const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base)) + kTimestepRva;
        for (uint32_t off = 0; off + 4 <= size; ++off) {
            uint32_t value = 0;
            std::memcpy(&value, code + off, sizeof(value));
            if (value == address) {
                ++refs;
            }
        }
    }
    LOG_INFO("Code references to the timestep constant: %d (expected %d)", refs, kTimestepRefs);

    LOG_INFO("Target %d FPS requested (FPSUnlock=%s). Phase 1 build: observing only, the game is unchanged.",
             settings.target_fps, settings.fps_unlock ? "true" : "false");
    return true;
}
