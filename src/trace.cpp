#include "trace.h"

#include "log.h"
#include "watch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>

namespace {

FILE* g_file = nullptr;
std::mutex g_mu;
uint64_t g_start_qpc = 0;
double g_qpc_to_ms = 1.0;
uint32_t g_rows = 0;
bool g_cap_logged = false;

constexpr uint32_t kMaxRows = 5000000;
constexpr size_t kCacheSize = 1024;

struct Entry {
    const void* chr = nullptr;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    uint32_t anim = 0xFFFFFFFFu;
    uint32_t action = 0xFFFFFFFFu;
    int pending = 0;
};

Entry g_cache[kCacheSize];

size_t slot_for(const void* chr) {
    auto h = static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(chr) >> 4);
    h ^= h >> 11;
    return h & (kCacheSize - 1);
}

Entry* find_entry(const void* chr) {
    const size_t start = slot_for(chr);
    for (size_t n = 0; n < 8; ++n) {
        Entry& entry = g_cache[(start + n) & (kCacheSize - 1)];
        if (entry.chr == nullptr) {
            entry.chr = chr;
            entry.pending = 1;
            return &entry;
        }
        if (entry.chr == chr) {
            return &entry;
        }
    }
    Entry& entry = g_cache[start];
    entry.chr = chr;
    entry.pending = 1;
    return &entry;
}

const char* kHeader =
    "seq,ms,site,phase,chr,dt,anim,action,idx,aobj_type,aobj_flags,"
    "mc,mstate,mstate2,mflags,mspeed,mthresh,"
    "physx,physy,physz,pposx,pposy,pposz,pflag,"
    "c140x,c140y,c140z,c2c0x,c2c0y,c2c0z,"
    "ctimer,cmode,cdt,rdt,"
    "velx,vely,velz,mvx,mvy,mvz,airt,"
    "m150x,m150y,m150z,m160x,m160y,m160z,cmdx,cmdy,cmdz,"
    "p130x,p130y,p130z,p140x,p140y,p140z,p150x,p150y,p150z,"
    "p1ax,p1ay,p1az,p1dx,p1dy,p1dz,pflags,"
    "c0x,c0y,c0z,p154,e4,e8,f228,f1e4,f1e0,fl\n";

}  // namespace

bool trace_active() {
    return g_file != nullptr;
}

bool trace_start(const wchar_t* dll_path) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) {
        return true;
    }
    // The game directory under Program Files is not writable unless the game
    // runs elevated, and one fixed name would be truncated by the next
    // launch. Write to %TEMP% with the process id so every run keeps its own
    // file.
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) {
        return false;
    }
    wchar_t full[MAX_PATH];
    _snwprintf_s(full, _TRUNCATE, L"%sDSR-FPS-Unlock-trace-%lu.csv", dir,
                 static_cast<unsigned long>(GetCurrentProcessId()));
    g_file = _wfsopen(full, L"w", _SH_DENYNO);
    if (!g_file) {
        return false;
    }
    std::setvbuf(g_file, nullptr, _IOFBF, 1 << 20);
    std::fprintf(g_file, "%s", kHeader);

    char narrow[MAX_PATH];
    if (WideCharToMultiByte(CP_UTF8, 0, full, -1, narrow, MAX_PATH, nullptr, nullptr) > 0) {
        LOG_INFO("Trace CSV: %s", narrow);
    }

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    g_start_qpc = now.QuadPart;
    g_qpc_to_ms = freq.QuadPart ? 1000.0 / static_cast<double>(freq.QuadPart) : 1.0;
    return true;
}

void trace_log(void* chr, float dt, uint64_t site, int phase) {
    if (!g_file || !chr) {
        return;
    }
    auto* base = static_cast<uint8_t*>(chr);
    auto* mc = *reinterpret_cast<uint8_t**>(base + 0x68);
    if (!mc) {
        return;
    }
    auto* phys = *reinterpret_cast<uint8_t**>(mc + 0x28);
    if (!phys) {
        return;
    }

    if (phase == 0) {
        watch_note(chr, phys);
    }
    const float* pos = reinterpret_cast<const float*>(phys + 0x10);
    const uint32_t anim = *reinterpret_cast<const uint32_t*>(base + 0xD0);
    const uint32_t action = *reinterpret_cast<const uint32_t*>(base + 0x354);

    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_file) {
        return;
    }
    if (g_rows >= kMaxRows) {
        if (!g_cap_logged) {
            g_cap_logged = true;
            LOG_ERROR("Trace row cap of %u reached; further rows are dropped", kMaxRows);
        }
        return;
    }
    Entry* entry = find_entry(chr);
    const bool moved = pos[0] != entry->pos[0] || pos[1] != entry->pos[1] || pos[2] != entry->pos[2] ||
                       anim != entry->anim || action != entry->action;
    if (moved) {
        entry->pending = 1;
    }
    const bool log_row = entry->pending != 0;
    if (phase) {
        entry->pos[0] = pos[0];
        entry->pos[1] = pos[1];
        entry->pos[2] = pos[2];
        entry->anim = anim;
        entry->action = action;
        entry->pending = 0;
    }
    if (!log_row) {
        return;
    }

    const int32_t idx = *reinterpret_cast<const int32_t*>(base + 0x164);
    uint8_t* action_obj = *reinterpret_cast<uint8_t**>(base + 0x18);
    const uint32_t action_type = action_obj ? *reinterpret_cast<const uint32_t*>(action_obj + 0x10) : 0xFFFFFFFFu;
    const uint8_t action_flags = action_obj ? *reinterpret_cast<const uint8_t*>(action_obj + 0x20) : 0xFF;
    const uint32_t mstate = *reinterpret_cast<const uint32_t*>(mc + 0x134);
    const uint32_t mstate2 = *reinterpret_cast<const uint32_t*>(mc + 0x138);
    const uint32_t mflags = *reinterpret_cast<const uint32_t*>(mc + 0x1B4);
    const float mspeed = *reinterpret_cast<const float*>(mc + 0x1B8);
    const float mthresh = *reinterpret_cast<const float*>(mc + 0x1BC);
    const float* prev = reinterpret_cast<const float*>(phys + 0x20);
    const uint8_t pflag = *reinterpret_cast<const uint8_t*>(phys + 0x32);
    const float* c140 = reinterpret_cast<const float*>(base + 0x140);
    const float* c2c0 = reinterpret_cast<const float*>(base + 0x2C0);
    const float ctimer = *reinterpret_cast<const float*>(base + 0x450);
    const uint8_t cmode = *reinterpret_cast<const uint8_t*>(base + 0x460);
    const float cdt = *reinterpret_cast<const float*>(base + 0x458);
    const float rdt = *reinterpret_cast<const float*>(base + 0x45C);

    // Probe: physics velocity (vec3 at phys+0x120), move-control input vector
    // (mc+0x140), the phys airborne accumulator (+0x1B0) and the byte flags
    // that gate fn 0x2bbe40's velocity damp paths. The extra vec3 probes cover
    // the move pipeline (mc+0xA0 move command, mc+0x150/0x160) and the phys
    // internal velocity/displacement slots (+0x130/+0x140/+0x150/+0x1A0/+0xD0)
    // so a per-frame displacement bug shows its carrier column directly.
    const float* vel = reinterpret_cast<const float*>(phys + 0x120);
    const float* mv = reinterpret_cast<const float*>(mc + 0x140);
    const float airt = *reinterpret_cast<const float*>(phys + 0x1B0);
    const float* m150 = reinterpret_cast<const float*>(mc + 0x150);
    const float* m160 = reinterpret_cast<const float*>(mc + 0x160);
    const float* mcmd = reinterpret_cast<const float*>(mc + 0xA0);
    const float* p130 = reinterpret_cast<const float*>(phys + 0x130);
    const float* p140 = reinterpret_cast<const float*>(phys + 0x140);
    const float* p150 = reinterpret_cast<const float*>(phys + 0x150);
    const float* p1a0 = reinterpret_cast<const float*>(phys + 0x1A0);
    const float* p0d0 = reinterpret_cast<const float*>(phys + 0xD0);
    uint8_t pflags = 0;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x33)) pflags |= 1u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0xEC)) pflags |= 2u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0xED)) pflags |= 4u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x1F2)) pflags |= 8u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x1F5)) pflags |= 16u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x1F0)) pflags |= 32u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x6D)) pflags |= 64u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x1F4)) pflags |= 128u;

    // Ground snap probe (fn 0x2BCB00 / 0x2BC650): the per-frame snap vector
    // (+0xC0), the extra offset (+0x154), step heights (+0xE4/+0xE8), reach
    // padding (+0x228), the 0x1E4/0x1E0 gates and the state bytes.
    const float* snap = reinterpret_cast<const float*>(phys + 0xC0);
    const float p154 = *reinterpret_cast<const float*>(phys + 0x154);
    const float e4 = *reinterpret_cast<const float*>(phys + 0xE4);
    const float e8 = *reinterpret_cast<const float*>(phys + 0xE8);
    const float f228 = *reinterpret_cast<const float*>(phys + 0x228);
    const float f1e4 = *reinterpret_cast<const float*>(phys + 0x1E4);
    const float f1e0 = *reinterpret_cast<const float*>(phys + 0x1E0);
    uint32_t fl = 0;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x1F1)) fl |= 1u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x1F3)) fl |= 2u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x34)) fl |= 4u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0xEE)) fl |= 8u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x6C)) fl |= 16u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x1F8)) fl |= 32u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x240)) fl |= 64u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x241)) fl |= 128u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x31)) fl |= 256u;
    if (*reinterpret_cast<const uint8_t*>(phys + 0x30)) fl |= 512u;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double ms = static_cast<double>(now.QuadPart - g_start_qpc) * g_qpc_to_ms;

    ++g_rows;
    std::fprintf(g_file,
                 "%u,%.3f,%llX,%d,%llX,%.6f,%u,%u,%d,%u,%u,"
                 "%llX,%u,%u,%u,%.6f,%.6f,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%u,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,"
                 "%.6f,%u,%.6f,%.6f,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.6f,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,"
                 "%.5f,%.5f,%.5f,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,"
                 "%.5f,%.5f,%.5f,"
                 "%.5f,%.5f,%.5f,"
                 "%.5f,%.5f,%.5f,%u,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%u\n",
                 g_rows, ms, static_cast<unsigned long long>(site), phase,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(chr)), dt, anim, action, idx,
                 action_type, action_flags, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mc)),
                 mstate, mstate2, mflags, mspeed, mthresh, pos[0], pos[1], pos[2], prev[0], prev[1], prev[2],
                 pflag, c140[0], c140[1], c140[2], c2c0[0], c2c0[1], c2c0[2], ctimer, cmode, cdt, rdt,
                 vel[0], vel[1], vel[2], mv[0], mv[1], mv[2], airt, m150[0], m150[1], m150[2], m160[0],
                 m160[1], m160[2], mcmd[0], mcmd[1], mcmd[2], p130[0], p130[1], p130[2], p140[0], p140[1],
                 p140[2], p150[0], p150[1], p150[2], p1a0[0], p1a0[1], p1a0[2], p0d0[0], p0d0[1], p0d0[2],
                 pflags, snap[0], snap[1], snap[2], p154, e4, e8, f228, f1e4, f1e0, fl);
    if ((g_rows & 0x3FF) == 0) {
        std::fflush(g_file);
    }
}

void trace_stop() {
    // Called from DllMain during process detach, by which point the other
    // threads are already gone, so the lock is not taken: flush the tail the
    // periodic fflush never reached.
    FILE* file = g_file;
    g_file = nullptr;
    if (file) {
        std::fflush(file);
        std::fclose(file);
    }
}
