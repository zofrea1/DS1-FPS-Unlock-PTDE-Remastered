#include "dxvk.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using SignalHandler = void(__cdecl*)(int);
using SignalFn = SignalHandler(__cdecl*)(int, SignalHandler);

// The system Direct3DCreate9 opens with mov edi,edi / push ebp / mov ebp,esp. Just past it go
// pop ebp (undoing the push) and a jump to routed_create, which then sees the caller's frame.
constexpr uint8_t kPrologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
constexpr size_t kPatchAt = sizeof(kPrologue);
constexpr size_t kPatchLen = 6;

constexpr int kSigAbrt = 22;  // SIGABRT in the Microsoft C runtimes
constexpr DWORD kProbeTimeoutMs = 20000;

enum Probe : DWORD { kProbeOk = 0, kProbeNull, kProbeAborted, kProbeFault, kProbeTimeout, kProbeNoThread };

CreateFn g_dxvk_create = nullptr;
IDirect3D9* g_probe_d3d = nullptr;
DWORD g_probe_fault = 0;
volatile DWORD g_probe_thread_id = 0;
// The object the probe created, handed to the first routed call so DXVK starts up only once.
PVOID volatile g_first = nullptr;

IDirect3D9* WINAPI routed_create(UINT sdk_version) {
    if (void* first = InterlockedExchangePointer(&g_first, nullptr)) {
        return static_cast<IDirect3D9*>(first);
    }
    return g_dxvk_create(sdk_version);
}

// An exception that escapes DXVK ends in abort(), which first raises SIGABRT in the C runtime
// DXVK shares with the process (ucrtbase). On the probe thread, that ends just the thread.
void __cdecl on_abort(int) {
    if (GetCurrentThreadId() == g_probe_thread_id) {
        ExitThread(kProbeAborted);
    }
}

const wchar_t* const kRuntimes[] = {L"ucrtbase.dll", L"msvcrt.dll"};
constexpr int kRuntimeCount = sizeof(kRuntimes) / sizeof(kRuntimes[0]);
SignalFn g_signal[kRuntimeCount] = {};
SignalHandler g_previous[kRuntimeCount] = {};

void guard_abort(bool on) {
    for (int i = 0; i < kRuntimeCount; ++i) {
        if (on) {
            HMODULE crt = GetModuleHandleW(kRuntimes[i]);
            g_signal[i] = crt ? reinterpret_cast<SignalFn>(GetProcAddress(crt, "signal")) : nullptr;
            if (g_signal[i]) {
                g_previous[i] = g_signal[i](kSigAbrt, &on_abort);
                if (g_previous[i] == reinterpret_cast<SignalHandler>(-1)) {  // SIG_ERR
                    g_signal[i] = nullptr;
                }
            }
        } else if (g_signal[i]) {
            g_signal[i](kSigAbrt, g_previous[i]);
            g_signal[i] = nullptr;
        }
    }
}

// Faults in DXVK or the Vulkan driver. Informational exceptions (thread names, debug output)
// are left to whoever raised them.
int fault_filter(DWORD code) {
    if ((code & 0xC0000000u) != 0xC0000000u) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_probe_fault = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

DWORD create_guarded() {
    __try {
        g_probe_d3d = g_dxvk_create(D3D_SDK_VERSION);
    } __except (fault_filter(GetExceptionCode())) {
        return kProbeFault;
    }
    return g_probe_d3d ? kProbeOk : kProbeNull;
}

DWORD WINAPI probe_thread(void*) {
    return create_guarded();
}

DWORD run_probe() {
    guard_abort(true);
    DWORD id = 0;
    HANDLE thread = CreateThread(nullptr, 0, probe_thread, nullptr, CREATE_SUSPENDED, &id);
    if (!thread) {
        guard_abort(false);
        return kProbeNoThread;
    }
    g_probe_thread_id = id;
    ResumeThread(thread);
    DWORD code = kProbeTimeout;
    if (WaitForSingleObject(thread, kProbeTimeoutMs) == WAIT_OBJECT_0) {
        GetExitCodeThread(thread, &code);
        guard_abort(false);
    }
    // After a timeout the guard stays in place; it only acts on the probe thread.
    CloseHandle(thread);
    return code;
}

void narrow(const wchar_t* in, char (&out)[MAX_PATH]) {
    if (!WideCharToMultiByte(CP_UTF8, 0, in, -1, out, MAX_PATH, nullptr, nullptr)) {
        out[0] = 0;
    }
}

bool under_windows_dir(const wchar_t* path) {
    wchar_t windows[MAX_PATH];
    const UINT len = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    return len > 0 && len < MAX_PATH && _wcsnicmp(path, windows, len) == 0 && path[len] == L'\\';
}

// File offset of `n` bytes at `rva`, from the section table in `headers`.
bool rva_to_offset(const uint8_t* headers, DWORD size, uintptr_t rva, size_t n, DWORD* offset) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(headers);
    if (size < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<DWORD>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > size) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(headers + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    const WORD count = nt->FileHeader.NumberOfSections;
    if (reinterpret_cast<const uint8_t*>(section + count) > headers + size) {
        return false;
    }
    for (WORD i = 0; i < count; ++i) {
        const IMAGE_SECTION_HEADER& s = section[i];
        if (rva >= s.VirtualAddress && rva + n <= s.VirtualAddress + s.SizeOfRawData) {
            *offset = static_cast<DWORD>(rva - s.VirtualAddress + s.PointerToRawData);
            return true;
        }
    }
    return false;
}

// Bytes of the DLL file at `rva`, as the file has them, whatever has been changed in memory since.
// (LoadLibraryEx as a data file would hand back the loaded module when it is the same file.)
bool read_file_bytes(const wchar_t* path, uintptr_t rva, uint8_t* out, size_t n) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint8_t headers[4096];
    DWORD got = 0;
    DWORD offset = 0;
    bool ok = ReadFile(file, headers, sizeof(headers), &got, nullptr) && rva_to_offset(headers, got, rva, n, &offset) &&
              SetFilePointer(file, static_cast<LONG>(offset), nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
              ReadFile(file, out, static_cast<DWORD>(n), &got, nullptr) && got == n;
    CloseHandle(file);
    return ok;
}

// The function a DLL exports under `name`, from the DLL's own export table. GetProcAddress can
// return a stub of the Windows compatibility shim engine (apphelp.dll) instead, as it does in a
// game with a compatibility mode set; the stub ends up calling this function.
uint8_t* export_address(HMODULE module, const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) {
        return nullptr;
    }
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name) != 0) {
            continue;
        }
        const DWORD rva = functions[ordinals[i]];
        // A forwarder (an RVA inside the export directory) or an RVA past the image is not code here.
        const bool forwarder = rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size;
        return forwarder || rva >= nt->OptionalHeader.SizeOfImage ? nullptr : base + rva;
    }
    return nullptr;
}

// Who, if anyone, has hooked the entry: the module a 5-byte jump (or a hotpatch short jump to
// one) leads to.
void describe_entry(const uint8_t* entry, const uint8_t* file, char (&out)[MAX_PATH]) {
    if (std::memcmp(entry, file, kPatchAt) == 0) {
        std::snprintf(out, MAX_PATH, "not hooked");
        return;
    }
    const uint8_t* jump = entry;
    if (jump[0] == 0xEB) {
        jump = jump + 2 + static_cast<int8_t>(jump[1]);
    }
    const uint8_t* target = nullptr;
    if (jump[0] == 0xE9) {
        int32_t rel = 0;
        std::memcpy(&rel, jump + 1, sizeof(rel));
        target = jump + 5 + rel;
    }
    HMODULE owner = nullptr;
    wchar_t name[MAX_PATH] = {};
    if (target && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(target), &owner) &&
        GetModuleFileNameW(owner, name, MAX_PATH)) {
        const wchar_t* slash = wcsrchr(name, L'\\');
        char file_name[MAX_PATH];
        narrow(slash ? slash + 1 : name, file_name);
        std::snprintf(out, MAX_PATH, "hooked by %s", file_name);
        return;
    }
    std::snprintf(out, MAX_PATH, "hooked");
}

bool write_code(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(at, bytes, n);
    DWORD ignored = 0;
    VirtualProtect(at, n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

}  // namespace

bool dxvk_route(const wchar_t* dxvk_path) {
    char dxvk_name[MAX_PATH];
    narrow(dxvk_path, dxvk_name);
    if (GetFileAttributesW(dxvk_path) == INVALID_FILE_ATTRIBUTES) {
        LOG_ERROR("DXVK: %s was not found. Running on the system Direct3D 9.", dxvk_name);
        return false;
    }

    HMODULE sys = GetModuleHandleW(L"d3d9.dll");
    wchar_t sys_path[MAX_PATH] = {};
    if (!sys || !GetModuleFileNameW(sys, sys_path, MAX_PATH)) {
        LOG_ERROR("DXVK: d3d9.dll is not loaded. Running on the system Direct3D 9.");
        return false;
    }
    char sys_name[MAX_PATH];
    narrow(sys_path, sys_name);
    if (!under_windows_dir(sys_path)) {
        LOG_ERROR("DXVK: the game loaded d3d9.dll from %s, not from Windows, so another Direct3D 9 replacement "
                  "(DXVK, ReShade, ...) is already installed. Leaving that one in charge.", sys_name);
        return false;
    }
    uint8_t* entry = export_address(sys, "Direct3DCreate9");
    const void* lookup = reinterpret_cast<const void*>(GetProcAddress(sys, "Direct3DCreate9"));
    if (lookup != entry) {
        LOG_INFO("DXVK: GetProcAddress gives Direct3DCreate9 as %p, a Windows compatibility shim stub; the function "
                 "itself is at %p", lookup, entry);
    }
    uint8_t file[kPatchAt + kPatchLen] = {};
    if (!entry || !read_file_bytes(sys_path, entry - reinterpret_cast<uint8_t*>(sys), file, sizeof(file))) {
        LOG_ERROR("DXVK: could not read Direct3DCreate9 from %s. Running on the system Direct3D 9.", sys_name);
        return false;
    }
    if (std::memcmp(file, kPrologue, sizeof(kPrologue)) != 0) {
        LOG_ERROR("DXVK: Direct3DCreate9 in %s starts %02X %02X %02X %02X %02X, not the expected prologue. "
                  "Running on the system Direct3D 9.", sys_name, file[0], file[1], file[2], file[3], file[4]);
        return false;
    }
    if (std::memcmp(entry + kPatchAt, file + kPatchAt, kPatchLen) != 0) {
        LOG_ERROR("DXVK: something has already rewritten Direct3DCreate9 past its first %u bytes. "
                  "Running on the system Direct3D 9.", static_cast<unsigned>(kPatchAt));
        return false;
    }
    if (std::memcmp(entry, file, kPatchAt) != 0 && entry[0] != 0xE9 && entry[0] != 0xEB) {
        LOG_ERROR("DXVK: Direct3DCreate9 was changed in an unrecognized way (%02X %02X %02X %02X %02X). "
                  "Running on the system Direct3D 9.", entry[0], entry[1], entry[2], entry[3], entry[4]);
        return false;
    }
    char entry_state[MAX_PATH];
    describe_entry(entry, file, entry_state);

    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!vulkan) {
        LOG_ERROR("DXVK: this PC has no Vulkan runtime (vulkan-1.dll). DXVK needs a graphics driver with Vulkan 1.3. "
                  "Running on the system Direct3D 9.");
        return false;
    }
    FreeLibrary(vulkan);

    HMODULE dxvk = LoadLibraryW(dxvk_path);
    if (!dxvk) {
        LOG_ERROR("DXVK: could not load %s (Win32 error %lu). Running on the system Direct3D 9.", dxvk_name,
                  GetLastError());
        return false;
    }
    g_dxvk_create = reinterpret_cast<CreateFn>(GetProcAddress(dxvk, "Direct3DCreate9"));
    if (!g_dxvk_create) {
        LOG_ERROR("DXVK: %s has no Direct3DCreate9; is it DXVK's 32-bit d3d9.dll? Running on the system Direct3D 9.",
                  dxvk_name);
        return false;
    }

    const DWORD probe = run_probe();
    if (probe != kProbeOk) {
        switch (probe) {
        case kProbeAborted:
            LOG_ERROR("DXVK could not start: no Vulkan 1.3 graphics card with the features it needs, or an outdated "
                      "driver (DARKSOULS_d3d9.log in the game folder says which). Running on the system Direct3D 9.");
            break;
        case kProbeFault:
            LOG_ERROR("DXVK crashed while starting (exception %08lX), probably in the Vulkan driver or a Vulkan "
                      "overlay layer. Running on the system Direct3D 9.", g_probe_fault);
            break;
        case kProbeTimeout:
            LOG_ERROR("DXVK did not finish starting within %lu s. Running on the system Direct3D 9.",
                      kProbeTimeoutMs / 1000);
            break;
        default:
            LOG_ERROR("DXVK could not create its Direct3D 9 object (result %lu). Running on the system Direct3D 9.",
                      probe);
            break;
        }
        return false;
    }
    const UINT adapters = g_probe_d3d->GetAdapterCount();
    if (adapters == 0) {
        g_probe_d3d->Release();
        LOG_ERROR("DXVK found no usable graphics adapter. Running on the system Direct3D 9.");
        return false;
    }
    D3DADAPTER_IDENTIFIER9 id{};
    g_probe_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);

    uint8_t patch[kPatchLen] = {0x5D, 0xE9};  // pop ebp; jmp routed_create
    const int32_t rel =
        static_cast<int32_t>(reinterpret_cast<uint8_t*>(&routed_create) - (entry + kPatchAt + kPatchLen));
    std::memcpy(patch + 2, &rel, sizeof(rel));
    g_first = g_probe_d3d;
    if (!write_code(entry + kPatchAt, patch, kPatchLen)) {
        const DWORD error = GetLastError();
        g_first = nullptr;
        g_probe_d3d->Release();
        LOG_ERROR("DXVK: could not unprotect Direct3DCreate9 (Win32 error %lu). Running on the system Direct3D 9.",
                  error);
        return false;
    }
    LOG_INFO("DXVK: Direct3D 9 runs on Vulkan through %s on \"%s\" (system Direct3DCreate9 %s, now leads to DXVK)",
             dxvk_name, id.Description, entry_state);
    return true;
}
