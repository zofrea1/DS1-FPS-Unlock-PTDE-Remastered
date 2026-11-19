#include "drawdist.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

// DrawMapPieces. A map piece is drawn when one of its draw groups (MSB part +0x38, 128 bits) is among the display
// groups of the frame, which the game takes from the collision the player stands on (its MSB part's +0x48, kept in
// the collision's record at +0x20 and read by 0x4569D0: ecx = record + 8, eax = out): when a map loads (0xDE85D0
// writes them into the draw state's +0x48 at 0xDE862A), when the player steps onto a floor of another block (the
// in-game step 0xEFD2F0 at 0xEFDEE3, into [step+0x38]+0x48), and into the per-block masks rebuilt from the floor
// (0xDE2AE0, 0xDE2C60 -> 0x4791C0 ORs them in), which the pieces' visibility takes too. Each collision is one room:
// it displays its own group, and its draw groups (record +0x10) list the rooms it is shown from, its neighbours.
// Firelink's ledge grass (m5060B2..m5064B2, groups 0-2) showed only from inside the shrine, not from the overlook
// above it (h0027B2: displays group 27, neighbours 0, 1, 2, 11 and 28; 2026-11-18, tested by giving that collision
// groups 0-2: the grass came in a few seconds later).
//  adjacent: those four reads answer the floor's display groups and its draw groups: the pieces seen from this room
//            and from the rooms next to it (the ledge grass from the overlook). Rooms farther away stay culled.
//  all:      every group: every piece of the loaded maps wherever the player stands (the view's own culling still
//            applies). Measured 2026-11-18 at 4K: Demon Ruins +3.5 ms game and +4.2 ms render thread a frame (about
//            300 -> 134 FPS: it draws Lost Izalith behind the walls, same map file), Blighttown +2.4 / +0.6 ms,
//            Firelink +0.5 / +0.6 ms.
// LodDistance. 0xF61CD0 (ecx = the model, esi = its LodBank row, al = its current level, xmm1 = its distance from the
// camera) picks level 0, 1 or 2 from the row's lv01/lv12 border distances and margins (default row 0: 15/30 m in
// Prepare to Die Edition, 25/50 m in Remastered). The distance is scaled by 100 / LodDistance on entry, so every
// border moves out by LodDistance percent.
namespace {

constexpr uint32_t kGetDisp = 0x004569D0;
constexpr uint32_t kLoadSite = 0x00DE8625;  // call 0x4569D0 (a map's load: the draw state's display groups)
constexpr uint32_t kStepSite = 0x00EFDEDE;  // call 0x4569D0 (a floor of another block underfoot)
// The per-block masks rebuilt from every floor of the block (0xDE2AE0, 0xDE2C60 -> 0x4791C0 ORs them in), which
// the pieces' visibility takes too: a floor's own groups alone in the draw state changed nothing (2026-11-18).
constexpr uint32_t kBlockSites[] = {0x00DE2AFA, 0x00DE2C9F};
constexpr uint32_t kLodFn = 0x00F61CD0;
constexpr uint8_t kLodHead[9] = {0x51, 0xF3, 0x0F, 0x10, 0x81, 0x50, 0x02, 0x00, 0x00};  // push ecx; movss xmm0,[ecx+250h]

volatile LONG g_mode = 0;  // DrawMapPieces: 0 game, 1 adjacent, 2 all
uint32_t g_get = kGetDisp;
// 0x4569D0 leaves ecx (the floor record + 8) as it was: its draw groups (the neighbouring rooms) are at ecx + 8.
uint32_t g_state = 0;  // the map's draw state ([ebx+0x10] at kLoadSite, [ebx+0x38] at kStepSite): +0x48 its groups
float g_lod_inv = 1.0f;
uint32_t g_lod_back = kLodFn + 9;
bool g_lod_on = false;

__declspec(naked) void get_disp_load() {
    __asm {
        push ecx
        mov ecx, dword ptr [ebx + 0x10]
        mov g_state, ecx
        pop ecx
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        cmp g_mode, 2
        je all
        push edx
        mov edx, dword ptr [ecx + 8]
        or dword ptr [eax], edx
        mov edx, dword ptr [ecx + 12]
        or dword ptr [eax + 4], edx
        mov edx, dword ptr [ecx + 16]
        or dword ptr [eax + 8], edx
        mov edx, dword ptr [ecx + 20]
        or dword ptr [eax + 12], edx
        pop edx
        jmp done
    all:
        mov dword ptr [eax], -1
        mov dword ptr [eax + 4], -1
        mov dword ptr [eax + 8], -1
        mov dword ptr [eax + 12], -1
    done:
        ret
    }
}

__declspec(naked) void get_disp_step() {
    __asm {
        push ecx
        mov ecx, dword ptr [ebx + 0x38]
        mov g_state, ecx
        pop ecx
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        cmp g_mode, 2
        je all
        push edx
        mov edx, dword ptr [ecx + 8]
        or dword ptr [eax], edx
        mov edx, dword ptr [ecx + 12]
        or dword ptr [eax + 4], edx
        mov edx, dword ptr [ecx + 16]
        or dword ptr [eax + 8], edx
        mov edx, dword ptr [ecx + 20]
        or dword ptr [eax + 12], edx
        pop edx
        jmp done
    all:
        mov dword ptr [eax], -1
        mov dword ptr [eax + 4], -1
        mov dword ptr [eax + 8], -1
        mov dword ptr [eax + 12], -1
    done:
        ret
    }
}

__declspec(naked) void get_disp_any() {
    __asm {
        call dword ptr [g_get]
        cmp g_mode, 0
        je done
        cmp g_mode, 2
        je all
        push edx
        mov edx, dword ptr [ecx + 8]
        or dword ptr [eax], edx
        mov edx, dword ptr [ecx + 12]
        or dword ptr [eax + 4], edx
        mov edx, dword ptr [ecx + 16]
        or dword ptr [eax + 8], edx
        mov edx, dword ptr [ecx + 20]
        or dword ptr [eax + 12], edx
        pop edx
        jmp done
    all:
        mov dword ptr [eax], -1
        mov dword ptr [eax + 4], -1
        mov dword ptr [eax + 8], -1
        mov dword ptr [eax + 12], -1
    done:
        ret
    }
}

__declspec(naked) void lod_entry() {
    __asm {
        mulss xmm1, g_lod_inv
        push ecx
        movss xmm0, dword ptr [ecx + 0x250]
        jmp dword ptr [g_lod_back]
    }
}

bool write_bytes(uint32_t at, const void* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, n);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

// A call rel32 at `site` to `from` made to call `to`.
bool redirect_call(uint32_t site, uint32_t from, const void* to) {
    const auto* p = reinterpret_cast<const uint8_t*>(site);
    int32_t rel = 0;
    std::memcpy(&rel, p + 1, 4);
    if (p[0] != 0xE8 || site + 5 + rel != from) return false;
    uint8_t code[5] = {0xE8};
    const int32_t nrel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (site + 5));
    std::memcpy(code + 1, &nrel, 4);
    return write_bytes(site, code, 5);
}

bool install_lod() {
    if (std::memcmp(reinterpret_cast<const void*>(kLodFn), kLodHead, sizeof(kLodHead)) != 0) return false;
    uint8_t code[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&lod_entry) - (kLodFn + 5));
    std::memcpy(code + 1, &rel, 4);
    return write_bytes(kLodFn, code, sizeof(code));
}

}  // namespace

void drawdist_set_mode(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    InterlockedExchange(&g_mode, mode);
    // At once when the draw state is known (the player's floor record: draw groups +0x10, display groups +0x20).
    const uint32_t state = g_state;
    __try {
        const uint32_t wcm = *reinterpret_cast<uint32_t*>(0x0137D644);
        const uint32_t chr = wcm ? *reinterpret_cast<uint32_t*>(wcm + 0x3C) : 0;
        const uint32_t floor = chr ? *reinterpret_cast<uint32_t*>(chr + 0x29C) : 0;
        if (state) {
            auto* groups = reinterpret_cast<uint32_t*>(state + 0x48);
            if (mode == 2) {
                for (int i = 0; i < 4; ++i) groups[i] = 0xFFFFFFFFu;
            } else if (floor) {
                const auto* disp = reinterpret_cast<const uint32_t*>(floor + 0x20);
                const auto* nbr = reinterpret_cast<const uint32_t*>(floor + 0x10);
                for (int i = 0; i < 4; ++i) groups[i] = disp[i] | (mode == 1 ? nbr[i] : 0u);
            }
        }
        // And the floor applied again on the next frame (SetFloor 0xE6E580 sees no floor yet), which rebuilds the
        // per-block masks through the sites above.
        if (chr) *reinterpret_cast<uint32_t*>(chr + 0x29C) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void drawdist_set_lod(int percent) {
    if (percent < 25) percent = 25;
    if (percent > 1000) percent = 1000;
    g_lod_inv = 100.0f / static_cast<float>(percent);
}

bool drawdist_install(int mode, int lod_percent) {
    drawdist_set_lod(lod_percent);
    InterlockedExchange(&g_mode, (mode < 0 || mode > 2) ? 0 : mode);
    const bool load = redirect_call(kLoadSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_load));
    bool step = redirect_call(kStepSite, kGetDisp, reinterpret_cast<const void*>(&get_disp_step));
    for (const uint32_t site : kBlockSites) {
        step = redirect_call(site, kGetDisp, reinterpret_cast<const void*>(&get_disp_any)) && step;
    }
    g_lod_on = install_lod();
    if (!load || !step) {
        LOG_ERROR("DrawMapPieces: the game's code at %08X/%08X does not match this build; not used",
                  static_cast<unsigned>(kLoadSite), static_cast<unsigned>(kStepSite));
    } else if (mode == 1) {
        LOG_INFO("DrawMapPieces = adjacent: the map pieces seen from the player's room and from the rooms next to it "
                 "are drawn (the game: this room's only)");
    } else if (mode == 2) {
        LOG_INFO("DrawMapPieces = all: every map piece of the loaded maps is drawn wherever the player stands");
    }
    if (!g_lod_on) {
        LOG_ERROR("LodDistance: the game's code at %08X does not match this build; not used",
                  static_cast<unsigned>(kLodFn));
    } else if (lod_percent != 100) {
        LOG_INFO("LodDistance: models switch to their lower levels of detail %d%% as far away as the game's", lod_percent);
    }
    return load && step;
}
