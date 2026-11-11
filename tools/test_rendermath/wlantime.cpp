// Times the Wi-Fi API calls the Steam client makes every 5 s (read-only): wlantime.exe [rounds]
// WlanOpenHandle opens a session with the WLAN AutoConfig service; Steam's thread that answers games waits for it.
#include <windows.h>

#include <wlanapi.h>

#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "wlanapi.lib")

static double ms_since(LARGE_INTEGER a, LARGE_INTEGER f) {
    LARGE_INTEGER b;
    QueryPerformanceCounter(&b);
    return 1000.0 * double(b.QuadPart - a.QuadPart) / f.QuadPart;
}

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 5;
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    for (int i = 0; i < rounds; ++i) {
        HANDLE h = nullptr;
        DWORD ver = 0;
        QueryPerformanceCounter(&t);
        const DWORD r = WlanOpenHandle(2, nullptr, &ver, &h);
        const double open_ms = ms_since(t, f);
        double enum_ms = 0, query_ms = 0, close_ms = 0;
        DWORD n = 0;
        if (r == ERROR_SUCCESS) {
            PWLAN_INTERFACE_INFO_LIST list = nullptr;
            QueryPerformanceCounter(&t);
            if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
                enum_ms = ms_since(t, f);
                n = list->dwNumberOfItems;
                if (n) {
                    DWORD size = 0;
                    PVOID data = nullptr;
                    QueryPerformanceCounter(&t);
                    if (WlanQueryInterface(h, &list->InterfaceInfo[0].InterfaceGuid, wlan_intf_opcode_current_connection,
                                           nullptr, &size, &data, nullptr) == ERROR_SUCCESS) {
                        query_ms = ms_since(t, f);
                        WlanFreeMemory(data);
                    }
                }
                WlanFreeMemory(list);
            }
            QueryPerformanceCounter(&t);
            WlanCloseHandle(h, nullptr);
            close_ms = ms_since(t, f);
        }
        std::printf("round %d: WlanOpenHandle %.2f ms (result %lu), WlanEnumInterfaces %.2f ms (%lu interfaces), "
                    "WlanQueryInterface %.2f ms, WlanCloseHandle %.2f ms\n", i, open_ms, r, enum_ms, n, query_ms,
                    close_ms);
        Sleep(300);
    }
    return 0;
}
