#include "laa.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string narrow(const std::wstring& w) {
    char buf[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

bool read_file(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0x400 && size.QuadPart < 256ll * 1024 * 1024;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size();
    }
    CloseHandle(h);
    return ok;
}

bool write_file(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size() &&
                    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// The ImageHlp checksum (CheckSumMappedFile): 16-bit words with end-around carry, the CheckSum field as 0, plus the
// file's length.
uint32_t pe_checksum(const std::vector<uint8_t>& d, size_t sum_off) {
    uint64_t s = 0;
    for (size_t i = 0; i < d.size(); i += 2) {
        if (i == sum_off || i == sum_off + 2) continue;
        const uint32_t word = d[i] | (i + 1 < d.size() ? d[i + 1] << 8 : 0);
        s += word;
        s = (s & 0xFFFF) + (s >> 16);
    }
    s = (s & 0xFFFF) + (s >> 16);
    return static_cast<uint32_t>(s + d.size());
}

struct Header {
    size_t characteristics = 0;  // offset of the COFF Characteristics
    size_t checksum = 0;         // offset of the optional header's CheckSum
};

// A plain 32-bit x86 executable without a Steam DRM wrapper (.bind), or nothing.
bool parse(const std::vector<uint8_t>& d, Header* h, std::string* why) {
    auto u16 = [&d](size_t o) { return static_cast<uint32_t>(d[o] | d[o + 1] << 8); };
    auto u32 = [&d, &u16](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (d.size() < 0x400 || d[0] != 'M' || d[1] != 'Z') return *why = "not an executable", false;
    const size_t pe = u32(0x3C);
    if (pe + 0x200 > d.size() || std::memcmp(&d[pe], "PE\0\0", 4) != 0) return *why = "no PE header", false;
    if (u16(pe + 4) != IMAGE_FILE_MACHINE_I386 || u16(pe + 24) != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return *why = "not a 32-bit x86 executable", false;
    }
    const uint32_t sections = u16(pe + 6), opt = u16(pe + 20);
    for (uint32_t i = 0; i < sections; ++i) {
        const size_t s = pe + 24 + opt + i * 40;
        if (s + 40 > d.size()) return *why = "a damaged section table", false;
        if (std::memcmp(&d[s], ".bind", 5) == 0) return *why = "a Steam DRM wrapper (.bind)", false;
    }
    h->characteristics = pe + 4 + 18;
    h->checksum = pe + 24 + 64;
    return true;
}

}  // namespace

void laa_ensure(bool enabled) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    const double total_mb = GlobalMemoryStatusEx(&ms) ? static_cast<double>(ms.ullTotalVirtual) / 1048576.0 : 0.0;
    if (total_mb > 3072.0) {
        // The flag of the exe this process was started from (its header in memory), or the system's own 4 GB (Proton
        // sets WINE_LARGE_ADDRESS_AWARE: the Steam Deck had 4096 MB with the exe unflagged, 2026-11-21).
        const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        const auto* nt = base ? reinterpret_cast<const IMAGE_NT_HEADERS*>(
                                    base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew)
                              : nullptr;
        const bool flag = nt && (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE);
        LOG_INFO("LargeAddressAware: the game has %.0f MB of address space (%s)", total_mb,
                 flag ? "DARKSOULS.exe is large address aware"
                      : "given by the system, as Proton does; DARKSOULS.exe is left as it is");
        return;
    }
    if (!enabled) {
        LOG_INFO("LargeAddressAware: false: DARKSOULS.exe is left as it is; the game has %.0f MB of address space",
                 total_mb);
        return;
    }
    wchar_t buf[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return;
    const std::wstring exe = buf;
    const std::wstring backup = exe + L".before-laa";
    const std::wstring fresh = exe + L".laa-new";
    std::vector<uint8_t> d;
    if (!read_file(exe, &d)) {
        LOG_ERROR("LargeAddressAware: could not read %s (error %lu); the game keeps 2 GB of address space",
                  narrow(exe).c_str(), GetLastError());
        return;
    }
    Header hd;
    std::string why;
    if (!parse(d, &hd, &why)) {
        LOG_INFO("LargeAddressAware: %s is %s: left as it is (2 GB of address space)", narrow(exe).c_str(), why.c_str());
        return;
    }
    const uint32_t ch = d[hd.characteristics] | d[hd.characteristics + 1] << 8;
    if (ch & IMAGE_FILE_LARGE_ADDRESS_AWARE) {
        LOG_INFO("LargeAddressAware: DARKSOULS.exe has the flag on disk already; the game gets 4 GB from its next start "
                 "(this run has %.0f MB)", total_mb);
        return;
    }
    std::vector<uint8_t> p = d;
    const uint32_t nch = ch | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    p[hd.characteristics] = static_cast<uint8_t>(nch);
    p[hd.characteristics + 1] = static_cast<uint8_t>(nch >> 8);
    uint32_t old_sum = 0;
    std::memcpy(&old_sum, &d[hd.checksum], 4);
    if (old_sum) {
        const uint32_t sum = pe_checksum(p, hd.checksum);
        std::memcpy(&p[hd.checksum], &sum, 4);
    }
    if (!write_file(fresh, p)) {
        LOG_ERROR("LargeAddressAware: could not write next to DARKSOULS.exe (error %lu; a folder only an administrator "
                  "may change?); the game keeps 2 GB of address space. LargeAddressAware = false stops these tries.",
                  GetLastError());
        return;
    }
    // The running exe can be renamed (not replaced): it becomes the backup, the copy takes its place.
    if (!MoveFileExW(exe.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD err = GetLastError();
        DeleteFileW(fresh.c_str());
        LOG_ERROR("LargeAddressAware: could not rename the running DARKSOULS.exe (error %lu); nothing changed, the game "
                  "keeps 2 GB of address space",
                  err);
        return;
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), 0)) {
        const DWORD err = GetLastError();
        MoveFileExW(backup.c_str(), exe.c_str(), 0);
        DeleteFileW(fresh.c_str());
        LOG_ERROR("LargeAddressAware: could not put the flagged copy in place (error %lu); the original is back, the "
                  "game keeps 2 GB of address space",
                  err);
        return;
    }
    LOG_INFO("LargeAddressAware: DARKSOULS.exe now has the large address aware flag: the game gets 4 GB of address "
             "space from its next start (this run still has %.0f MB). The original is kept as DARKSOULS.exe.before-laa.",
             total_mb);
}
