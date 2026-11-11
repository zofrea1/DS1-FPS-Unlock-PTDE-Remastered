// Watches the Steam client (steam.exe, 64-bit) from outside: steammon.exe [seconds]
// Every millisecond reads each steam.exe thread's CPU cycles (no suspension) to find bursts of work. A thread that
// works 5+ ms within 25 ms is a burst; the busiest bursting thread is then looked at (suspended for a moment, its
// instruction pointer and return addresses on its stack read) about every 1 ms while it bursts. Prints the bursts with
// their times and gaps, and where the bursting thread spent them (module+offset), to see what steam.exe does every
// few seconds that keeps it from answering games.
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

struct Mod {
    uint64_t lo, hi;
    std::string name;
};
std::vector<Mod> g_mods;

std::string where(uint64_t a) {
    for (const Mod& m : g_mods) {
        if (a >= m.lo && a < m.hi) {
            char b[64];
            std::snprintf(b, sizeof(b), "+%llX", static_cast<unsigned long long>(a - m.lo));
            return m.name + b;
        }
    }
    char b[32];
    std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(a));
    return b;
}

bool in_module(uint64_t a) {
    for (const Mod& m : g_mods) {
        if (a >= m.lo + 0x1000 && a < m.hi) return true;
    }
    return false;
}

double g_busy_ms = 0.5;
int g_min_burst = 4;

struct Thread {
    DWORD tid;
    HANDLE h;
    uint64_t start;
    ULONG64 last = 0;
    std::vector<double> ms;  // CPU ms per poll
};

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 20.0;
    if (argc > 2) g_busy_ms = std::atof(argv[2]);
    if (argc > 3) g_min_burst = std::atoi(argv[3]);
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) pid = pe.th32ProcessID;
    }
    CloseHandle(snap);
    if (!pid) { std::printf("steam.exe not running\n"); return 2; }
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { std::printf("OpenProcess failed %lu\n", GetLastError()); return 2; }
    HMODULE mods[1024];
    DWORD need = 0;
    if (EnumProcessModulesEx(proc, mods, sizeof(mods), &need, LIST_MODULES_ALL)) {
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
            MODULEINFO mi{};
            char name[MAX_PATH] = "";
            GetModuleInformation(proc, mods[i], &mi, sizeof(mi));
            GetModuleBaseNameA(proc, mods[i], name, MAX_PATH);
            g_mods.push_back({reinterpret_cast<uint64_t>(mi.lpBaseOfDll),
                              reinterpret_cast<uint64_t>(mi.lpBaseOfDll) + mi.SizeOfImage, name});
        }
    }
    using NtQIT = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    auto nqit = reinterpret_cast<NtQIT>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    std::vector<Thread> threads;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        Thread t{te.th32ThreadID, h, 0};
        PVOID start = nullptr;
        if (nqit) nqit(h, 9, &start, sizeof(start), nullptr);
        t.start = reinterpret_cast<uint64_t>(start);
        QueryThreadCycleTime(h, &t.last);
        threads.push_back(t);
    }
    CloseHandle(snap);
    std::printf("steam.exe pid %lu: %zu threads, %zu modules\n", pid, threads.size(), g_mods.size());
    // Cycles per ms, measured.
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    timeBeginPeriod(1);
    unsigned __int64 c0 = __rdtsc();
    QueryPerformanceCounter(&q0);
    Sleep(200);
    unsigned __int64 c1 = __rdtsc();
    QueryPerformanceCounter(&q1);
    const double cyc_per_ms = (c1 - c0) / (1000.0 * (q1.QuadPart - q0.QuadPart) / f.QuadPart);
    for (Thread& t : threads) QueryThreadCycleTime(t.h, &t.last);

    // Phase 1 and 2 together: poll cycles every ms; a thread that used > 0.5 ms in the last ms is looked at.
    std::map<std::string, int> self_hits, ret_hits;
    std::vector<std::pair<double, DWORD>> busy_ms;  // (time s, tid) for ms slots with > 0.5 ms of CPU
    QueryPerformanceCounter(&q0);
    std::vector<uint64_t> stack(1024);
    int looks = 0;
    for (;;) {
        Sleep(1);
        QueryPerformanceCounter(&q1);
        const double now = double(q1.QuadPart - q0.QuadPart) / f.QuadPart;
        if (now > seconds) break;
        for (Thread& t : threads) {
            ULONG64 c = 0;
            if (!QueryThreadCycleTime(t.h, &c)) continue;
            const double ms = (c - t.last) / cyc_per_ms;
            t.last = c;
            if (ms > g_busy_ms) {
                busy_ms.push_back({now, t.tid});
                // Look at it now.
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(t.h) == static_cast<DWORD>(-1)) continue;
                const bool got = GetThreadContext(t.h, &ctx) != 0;
                SIZE_T read = 0;
                if (got) ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(ctx.Rsp), stack.data(), stack.size() * 8, &read);
                ResumeThread(t.h);
                if (!got) continue;
                ++looks;
                char key[96];
                std::snprintf(key, sizeof(key), "%lu ", t.tid);
                self_hits[std::string(key) + where(ctx.Rip & ~0xFFull)]++;
                int n = 0;
                for (size_t w = 0; w < read / 8 && n < 12; ++w) {
                    if (in_module(stack[w])) {
                        ret_hits[std::string(key) + where(stack[w])]++;
                        ++n;
                    }
                }
            }
        }
    }
    // Bursts: per thread, busy ms slots grouped when within 30 ms.
    std::map<DWORD, std::vector<double>> per;
    for (auto& b : busy_ms) per[b.second].push_back(b.first);
    for (auto& [tid, times] : per) {
        std::vector<std::pair<double, int>> bursts;
        for (double tt : times) {
            if (bursts.empty() || tt - bursts.back().first - bursts.back().second * 0.001 > 0.03)
                bursts.push_back({tt, 1});
            else
                bursts.back().second++;
        }
        int big = 0;
        for (auto& b : bursts) big += b.second >= g_min_burst;
        if (big == 0) continue;
        uint64_t start = 0;
        for (Thread& t : threads) if (t.tid == tid) start = t.start;
        std::printf("thread %lu (starts at %s): %zu busy ms; bursts of 4+ ms:", tid, where(start).c_str(), times.size());
        double prev = 0;
        for (auto& b : bursts) {
            if (b.second < g_min_burst) continue;
            std::printf(" %.2fs(%dms,+%.2f)", b.first, b.second, prev ? b.first - prev : 0.0);
            prev = b.first;
        }
        std::printf("\n");
    }
    std::vector<std::pair<int, std::string>> top;
    for (auto& e : self_hits) top.push_back({e.second, e.first});
    std::sort(top.rbegin(), top.rend());
    std::printf("%d looks; where the busy threads were (thread, 256-byte block):\n", looks);
    for (size_t i = 0; i < top.size() && i < 25; ++i) std::printf("  %5d  %s\n", top[i].first, top[i].second.c_str());
    top.clear();
    for (auto& e : ret_hits) top.push_back({e.second, e.first});
    std::sort(top.rbegin(), top.rend());
    std::printf("return addresses on their stacks:\n");
    for (size_t i = 0; i < top.size() && i < 40; ++i) std::printf("  %5d  %s\n", top[i].first, top[i].second.c_str());
    return 0;
}
