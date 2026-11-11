// Follows one steam.exe thread every millisecond, busy or waiting: steamthread.exe <tid> [seconds]
// Prints each time the thread's state changes (instruction pointer's 256-byte block plus the first return addresses
// on its stack into a module, checked for a call before them), with the time and how long the previous state lasted.
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

struct Mod {
    uint64_t lo, hi;
    std::string name;
};
std::vector<Mod> g_mods;
HANDLE g_proc;

const Mod* mod_of(uint64_t a) {
    for (const Mod& m : g_mods)
        if (a >= m.lo && a < m.hi) return &m;
    return nullptr;
}

std::string where(uint64_t a) {
    if (const Mod* m = mod_of(a)) {
        char b[64];
        std::snprintf(b, sizeof(b), "+%llX", static_cast<unsigned long long>(a - m->lo));
        return m->name + b;
    }
    char b[32];
    std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(a));
    return b;
}

// A return address: inside a module's image and the bytes before it are a call.
bool is_return(uint64_t a) {
    const Mod* m = mod_of(a);
    if (!m || a < m->lo + 0x1000) return false;
    uint8_t p[8];
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(a - 8), p, 8, &got) || got != 8) return false;
    // p[8 - k] is the byte k before the address.
    if (p[3] == 0xE8) return true;                                  // call rel32
    if (p[2] == 0xFF && p[3] == 0x15) return true;                  // call [rip+d32]
    if (p[6] == 0xFF && (p[7] & 0xF8) == 0xD0) return true;         // call reg
    if (p[5] == 0x41 && p[6] == 0xFF && (p[7] & 0xF8) == 0xD0) return true;
    if (p[5] == 0xFF && (p[6] & 0xF8) == 0x50) return true;         // call [reg+d8]
    if (p[2] == 0xFF && (p[3] & 0xF8) == 0x90) return true;         // call [reg+d32]
    if (p[4] == 0xFF && p[5] == 0x54) return true;                  // call [r+r*s+d8]
    if (p[1] == 0xFF && p[2] == 0x94) return true;
    if (p[4] == 0x41 && p[5] == 0xFF) return true;
    if (p[1] == 0x48 && p[2] == 0xFF) return true;
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const DWORD tid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
    const double seconds = argc > 2 ? std::atof(argv[2]) : 12.0;
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!h) { std::printf("OpenThread failed %lu\n", GetLastError()); return 2; }
    const DWORD pid = GetProcessIdOfThread(h);
    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    HMODULE mods[1024];
    DWORD need = 0;
    EnumProcessModulesEx(g_proc, mods, sizeof(mods), &need, LIST_MODULES_ALL);
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
        MODULEINFO mi{};
        char name[MAX_PATH] = "";
        GetModuleInformation(g_proc, mods[i], &mi, sizeof(mi));
        GetModuleBaseNameA(g_proc, mods[i], name, MAX_PATH);
        g_mods.push_back({reinterpret_cast<uint64_t>(mi.lpBaseOfDll),
                          reinterpret_cast<uint64_t>(mi.lpBaseOfDll) + mi.SizeOfImage, name});
    }
    timeBeginPeriod(1);
    LARGE_INTEGER f, q0, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    std::string prev;
    double prev_t = 0;
    std::vector<uint64_t> stack(2048);
    for (;;) {
        QueryPerformanceCounter(&q);
        const double now = double(q.QuadPart - q0.QuadPart) / f.QuadPart;
        if (now > seconds) break;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(h) == static_cast<DWORD>(-1)) break;
        const bool got = GetThreadContext(h, &ctx) != 0;
        SIZE_T read = 0;
        if (got) ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(ctx.Rsp), stack.data(), stack.size() * 8, &read);
        ResumeThread(h);
        if (!got) break;
        std::string sig = where(ctx.Rip & ~0xFFull);
        int n = 0;
        for (size_t w = 0; w < read / 8 && n < 28; ++w) {
            if (is_return(stack[w])) {
                sig += " < " + where(stack[w]);
                ++n;
            }
        }
        if (sig != prev) {
            if (!prev.empty()) std::printf("%8.3f s (%.1f ms): %s\n", prev_t, (now - prev_t) * 1000.0, prev.c_str());
            prev = sig;
            prev_t = now;
        }
        Sleep(1);
    }
    std::printf("%8.3f s: %s\n", prev_t, prev.c_str());
    return 0;
}
