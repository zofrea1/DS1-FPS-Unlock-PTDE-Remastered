// FastReadLocks against the game's own reader/writer lock (DLKR::DLPlainReadWriteLock), with the game's image mapped
// at 0x400000 in a suspended child (as test_rmath):
//  1. the same random single-thread sequences of lock calls on an original lock and on one driven by the
//     replacements: every return value and the reader count must match;
//  2. a stress run: reader threads on the replacements, writer threads on the game's own WriteLock/WriteUnlock, with
//     a shared check that a writer never overlaps a reader, and that nothing deadlocks.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#pragma comment(lib, "winmm.lib")
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);

#include "rwlock.cpp"

static int g_fail = 0;

struct GameLock {
    void* vtable;
    HANDLE event, writer, reader_mutex;
    volatile LONG count;
};

// thiscall callers: ecx = this, args pushed, the callee pops them.
__declspec(naked) int __cdecl this0(void* fn, void* self) {
    __asm {
        mov ecx, [esp + 8]
        jmp dword ptr [esp + 4]
    }
}
__declspec(naked) int __cdecl this1(void* fn, void* self, uint32_t a) {
    __asm {
        push ebp
        mov ebp, esp
        mov ecx, [ebp + 12]
        push dword ptr [ebp + 16]
        call dword ptr [ebp + 8]
        pop ebp
        ret
    }
}

static void make_lock(GameLock* l) {
    std::memset(l, 0, sizeof(*l));
    l->vtable = reinterpret_cast<void*>(0x10DD0E4);  // DLKR::DLPlainReadWriteLock
    this0(reinterpret_cast<void*>(0x60CEB0), l);      // Create: the event, both mutexes, count -1
}

// Original methods (the game's code) and the replacements, through the same thiscall shapes.
static int o_read(GameLock* l, uint32_t t) { return this1(reinterpret_cast<void*>(0x60D0F0), l, t); }
static int o_try_read(GameLock* l) { return this0(reinterpret_cast<void*>(0x60D1B0), l); }
static int o_read_unlock(GameLock* l) { return this0(reinterpret_cast<void*>(0x60D220), l); }
static int r_read(GameLock* l, uint32_t t) { return this1(reinterpret_cast<void*>(&read_lock), l, t); }
static int r_try_read(GameLock* l) { return this0(reinterpret_cast<void*>(&try_read_lock), l); }
static int r_read_unlock(GameLock* l) { return this0(reinterpret_cast<void*>(&read_unlock), l); }
static int write_lock(GameLock* l, uint32_t t) { return this1(reinterpret_cast<void*>(0x60D300), l, t); }
static int try_write_lock(GameLock* l) { return this0(reinterpret_cast<void*>(0x60D310), l); }
static int write_unlock(GameLock* l) { return this0(reinterpret_cast<void*>(0x60D320), l); }

static volatile int g_run = -1, g_step = -1, g_op = -1, g_side = -1;
static DWORD WINAPI watchdog(void*) {
    Sleep(120000);
    std::printf("WATCHDOG: stuck at run %d step %d op %d side %d\n", g_run, g_step, g_op, g_side);
    ExitProcess(3);
}

static void sequences() {
    std::mt19937 rng(77);
    int ops = 0;
    for (int run = 0; run < 600; ++run) {
        GameLock a, b;
        make_lock(&a);
        make_lock(&b);
        int readers = 0, writer = 0;  // what the test holds (same for both locks)
        for (int step = 0; step < 40; ++step) {
            const int op = static_cast<int>(rng() % 7);
            g_run = run;
            g_step = step;
            g_op = op;
            g_side = 0;
            int ra = 0, rb = 0;
            const char* name = "";
            switch (op) {
            case 0:  // read lock, infinite: only when no writer (it would block forever)
                if (writer) continue;
                name = "read lock (infinite)";
                ra = o_read(&a, INFINITE);
                g_side = 1;
                rb = r_read(&b, INFINITE);
                ++readers;
                break;
            case 1:  // read lock, finite: also against a held writer (times out)
                name = "read lock (finite)";
                ra = o_read(&a, 1);
                rb = r_read(&b, 1);
                if (ra != -3 || a.count >= 0) {
                    // counted (even when -3 for a joining reader)
                }
                break;
            case 2:
                name = "try read lock";
                ra = o_try_read(&a);
                rb = r_try_read(&b);
                break;
            case 3:  // read unlock: only while readers are counted
                if (a.count < 0) continue;
                name = "read unlock";
                ra = o_read_unlock(&a);
                rb = r_read_unlock(&b);
                break;
            case 4:  // write lock, finite (against readers it times out)
                if (writer) continue;
                name = "write lock (finite)";
                ra = write_lock(&a, 1);
                rb = write_lock(&b, 1);
                if (ra == 0 && rb == 0) writer = 1;
                break;
            case 5:
                if (writer) continue;
                name = "try write lock";
                ra = try_write_lock(&a);
                rb = try_write_lock(&b);
                if (ra == 0 && rb == 0) writer = 1;
                break;
            case 6:
                if (!writer) continue;
                name = "write unlock";
                ra = write_unlock(&a);
                rb = write_unlock(&b);
                writer = 0;
                break;
            }
            ++ops;
            if (ra != rb || a.count != b.count) {
                if (g_fail < 20)
                    std::printf("MISMATCH run %d step %d %s: original %d (count %ld), replacement %d (count %ld)\n", run,
                                step, name, ra, a.count, rb, b.count);
                ++g_fail;
                break;
            }
        }
        (void)readers;
    }
    std::printf("sequences: %d calls compared, %d mismatches\n", ops, g_fail);
}

static GameLock g_shared;
static volatile LONG g_readers_in = 0, g_writer_in = 0, g_stop = 0, g_violations = 0;
static volatile LONG g_reads = 0, g_writes = 0, g_tries = 0;

static DWORD WINAPI reader(void* arg) {
    std::mt19937 rng(static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg)));
    // Infinite read locks only: all 29 of the game's calls pass INFINITE, and nothing calls the try lock directly. A
    // finite or try read lock cannot tell a caller whether it was counted (the original's quirk), so those are checked
    // in the single-thread sequences instead.
    while (!g_stop) {
        r_read(&g_shared, INFINITE);
        InterlockedIncrement(&g_readers_in);
        if (g_writer_in) InterlockedIncrement(&g_violations);
        for (volatile int i = 0; i < static_cast<int>(rng() % 200); ++i) {}
        if (g_writer_in) InterlockedIncrement(&g_violations);
        InterlockedDecrement(&g_readers_in);
        r_read_unlock(&g_shared);
        InterlockedIncrement(&g_reads);
    }
    return 0;
}

static DWORD WINAPI writer(void*) {
    while (!g_stop) {
        write_lock(&g_shared, INFINITE);
        InterlockedExchange(&g_writer_in, 1);
        if (g_readers_in) InterlockedIncrement(&g_violations);
        Sleep(0);
        if (g_readers_in) InterlockedIncrement(&g_violations);
        InterlockedExchange(&g_writer_in, 0);
        write_unlock(&g_shared);
        InterlockedIncrement(&g_writes);
        Sleep(0);
    }
    return 0;
}

static void stress() {
    make_lock(&g_shared);
    HANDLE t[8];
    for (int i = 0; i < 6; ++i) t[i] = CreateThread(nullptr, 0, reader, reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)), 0, nullptr);
    for (int i = 6; i < 8; ++i) t[i] = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
    Sleep(4000);
    g_stop = 1;
    const DWORD w = WaitForMultipleObjects(8, t, TRUE, 10000);
    if (w == WAIT_TIMEOUT) {
        std::printf("STRESS: threads did not finish (deadlock?) count %ld\n", g_shared.count);
        ++g_fail;
        return;
    }
    if (g_violations) {
        std::printf("STRESS: %ld overlaps of a writer and a reader\n", g_violations);
        ++g_fail;
    }
    if (g_shared.count != -1) {
        std::printf("STRESS: reader count %ld at the end (expected -1)\n", g_shared.count);
        ++g_fail;
    }
    std::printf("stress: %ld read sections (%ld try), %ld write sections, count %ld, %ld overlaps\n", g_reads, g_tries,
                g_writes, g_shared.count, g_violations);
}

static uint8_t g_image[0x11C2000];
static int run_parent(const char* self) {
    FILE* f = std::fopen("ptde_image.bin", "rb");
    if (!f) { std::printf("no image\n"); return 2; }
    std::fread(g_image, 1, sizeof(g_image), f);
    std::fclose(f);
    char cmd[MAX_PATH + 16];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" child", self);
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) return 2;
    void* at = VirtualAllocEx(pi.hProcess, reinterpret_cast<void*>(0x400000), sizeof(g_image), MEM_RESERVE | MEM_COMMIT,
                              PAGE_EXECUTE_READWRITE);
    SIZE_T wrote = 0;
    if (at != reinterpret_cast<void*>(0x400000) || !WriteProcessMemory(pi.hProcess, at, g_image, sizeof(g_image), &wrote)) {
        TerminateProcess(pi.hProcess, 2);
        return 2;
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    return static_cast<int>(code);
}

// The lock code calls Windows through the game's import table, which nobody bound in this process: fill in its
// KERNEL32 entries.
static int bind_kernel32_imports() {
    auto* base = reinterpret_cast<uint8_t*>(0x400000);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int bound = 0;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        const char* dll = reinterpret_cast<const char*>(base + d->Name);
        if (_stricmp(dll, "KERNEL32.dll") != 0) continue;
        HMODULE k = GetModuleHandleA("kernel32.dll");
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (FARPROC f = GetProcAddress(k, reinterpret_cast<const char*>(ibn->Name))) {
                iat->u1.Function = reinterpret_cast<DWORD>(f);
                ++bound;
            }
        }
    }
    return bound;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "child") != 0) return run_parent(argv[0]);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("KERNEL32 imports bound: %d\n", bind_kernel32_imports());
    CloseHandle(CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr));
    timeBeginPeriod(1);
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    g_wait_on_address = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(kb, "WaitOnAddress"));
    g_wake_all = reinterpret_cast<WakeByAddressAllFn>(GetProcAddress(kb, "WakeByAddressAll"));
    sequences();
    stress();
    std::printf("%d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
