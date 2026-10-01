#include "inputdump.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kObjectRva = 0x1CB5420;  // global pointer read by the HUD shortcut handler
constexpr uint32_t kWords = 0x180;          // 0x600 bytes
constexpr size_t kMaxSamples = 8000;

wchar_t g_dir[MAX_PATH] = {};

bool read_words(const void* from, uint32_t* out, uint32_t words) {
    __try {
        std::memcpy(out, from, words * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(out, 0, words * 4);
        return false;
    }
}

void* read_object(const uint8_t* base) {
    __try {
        return *reinterpret_cast<void* const*>(base + kObjectRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void write_capture(int number, const std::vector<uint32_t>& data, const std::vector<double>& ms) {
    const size_t samples = ms.size();
    if (samples < 2) return;
    std::vector<uint32_t> keep;
    for (uint32_t c = 0; c < kWords; ++c) {
        for (size_t s = 1; s < samples; ++s) {
            if (data[s * kWords + c] != data[c]) {
                keep.push_back(c);
                break;
            }
        }
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%sDSR-FPS-Unlock-input-%d.csv", g_dir, number);
    FILE* file = nullptr;
    _wfopen_s(&file, path, L"w");
    if (!file) return;
    std::fprintf(file, "row,ms");
    for (uint32_t c : keep) std::fprintf(file, ",+%03X", c * 4);
    std::fputc(10, file);
    for (size_t s = 0; s < samples; ++s) {
        std::fprintf(file, "%zu,%.3f", s, ms[s]);
        for (uint32_t c : keep) std::fprintf(file, ",%x", data[s * kWords + c]);
        std::fputc(10, file);
    }
    std::fclose(file);
    LOG_INFO("Input dump %d: %zu samples, %zu of %u words changed", number, samples, keep.size(), kWords);
}

// The HUD shortcut handler calls slot 6 of this object's vtable with 0xC or 0xD and acts when the
// answer is 0xF. This wraps that slot to log how the answer changes while a button is held.
using StateFn = int (*)(void* self, int index);
StateFn g_state_orig = nullptr;
int g_state_last[32];
LONG g_state_lines = 0;

int hook_state(void* self, int index) {
    const int result = g_state_orig(self, index);
    if (index >= 0 && index < 32 && g_state_last[index] != result + 1 && g_state_lines < 1500) {
        g_state_last[index] = result + 1;
        InterlockedIncrement(&g_state_lines);
        LOG_INFO("[input] state index=0x%X -> 0x%X", index, result);
    }
    return result;
}

void hook_state_slot(void* object) {
    static bool done = false;
    if (done || !object) return;
    done = true;
    void** vtable = nullptr;
    __try {
        vtable = *reinterpret_cast<void***>(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!vtable) return;
    void** slot = vtable + 6;
    LOG_INFO("[input] state object %p, vtable %p, slot 6 = %p", object, static_cast<void*>(vtable), *slot);
    g_state_orig = reinterpret_cast<StateFn>(*slot);
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = reinterpret_cast<void*>(hook_state);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old, &ignored);
        LOG_INFO("[input] state hook installed");
    }
}

DWORD WINAPI dump_thread(void*) {
    const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    bool recording = false;
    bool key_was = false;
    int capture = 0;
    std::vector<uint32_t> data;
    std::vector<double> ms;
    LARGE_INTEGER t0{};
    for (;;) {
        Sleep(1);
        const bool key = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
        if (key && !key_was) {
            if (recording) {
                recording = false;
                write_capture(++capture, data, ms);
                data.clear();
                ms.clear();
            } else {
                recording = true;
                QueryPerformanceCounter(&t0);
                LOG_INFO("Input dump: recording");
            }
        }
        key_was = key;
        if (!recording) continue;
        void* object = read_object(base);
        if (!object) continue;
        const size_t at = data.size();
        data.resize(at + kWords);
        read_words(object, &data[at], kWords);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        ms.push_back(1000.0 * static_cast<double>(now.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));
        if (ms.size() >= kMaxSamples) {
            recording = false;
            write_capture(++capture, data, ms);
            data.clear();
            ms.clear();
        }
    }
    return 0;
}

}  // namespace

void inputdump_start(const wchar_t* dll_path) {
    wcsncpy_s(g_dir, dll_path, _TRUNCATE);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    HANDLE thread = CreateThread(nullptr, 0, dump_thread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        LOG_INFO("Input dump available: F12 starts and stops a recording");
    }
}
