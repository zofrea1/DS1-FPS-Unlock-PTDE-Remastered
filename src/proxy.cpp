#include "proxy.h"

#include "log.h"
#include "state.h"

#include <cstdint>
#include <cstring>

namespace {

using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, const GUID*, void**, void*);
using CreateDeviceFn = HRESULT(WINAPI*)(void*, const GUID*, void**, void*);
using GetDeviceStateFn = HRESULT(WINAPI*)(void*, DWORD, void*);
using GetDeviceDataFn = HRESULT(WINAPI*)(void*, DWORD, void*, DWORD*, DWORD);

constexpr int kMaxInterfaces = 4;
constexpr int kMaxVtables = 4;
constexpr int kMaxDevices = 32;

struct InterfaceHook {
    void** vtable = nullptr;
    CreateDeviceFn create_device = nullptr;
};

struct VtableHook {
    void** vtable = nullptr;
    GetDeviceStateFn get_state = nullptr;
    GetDeviceDataFn get_data = nullptr;
};

struct MouseDevice {
    void* device = nullptr;
    int64_t x_remainder = 0;
    int64_t y_remainder = 0;
};

CreateFn g_create = nullptr;
HMODULE g_system = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
InterfaceHook g_interfaces[kMaxInterfaces];
VtableHook g_vtables[kMaxVtables];
MouseDevice g_devices[kMaxDevices];
std::atomic<int> g_interface_lock{0};
std::atomic<int> g_mouse_lock{0};
std::atomic<int> g_mouse_logged{0};

// GUID_SysMouse. The keyboard and gamepad are left alone.
constexpr GUID kSystemMouse = {
    0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

void spin_lock(std::atomic<int>& lock) {
    while (lock.exchange(1, std::memory_order_acquire) != 0) {
        Sleep(0);
    }
}

void spin_unlock(std::atomic<int>& lock) {
    lock.store(0, std::memory_order_release);
}

bool same_guid(const GUID* guid) {
    return guid && guid->Data1 == kSystemMouse.Data1 && guid->Data2 == kSystemMouse.Data2 &&
           guid->Data3 == kSystemMouse.Data3 &&
           memcmp(guid->Data4, kSystemMouse.Data4, sizeof(guid->Data4)) == 0;
}

bool executable(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) {
        return false;
    }
    const DWORD protect = info.Protect & 0xFF;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

bool patch_pointer(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return false;
    }
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), value);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

// Mouse deltas are sampled once per displayed frame and then multiplied by a
// 1/60 step. Scale the counts by TargetFPS/60 so look speed stays put.
LONG scale_axis(LONG value, int64_t* remainder) {
    const int64_t target = g_target_fps.load(std::memory_order_relaxed);
    const int64_t numerator = static_cast<int64_t>(value) * target + *remainder;
    const int64_t scaled = numerator / 60;
    *remainder = numerator - scaled * 60;
    if (scaled > INT32_MAX) {
        *remainder = 0;
        return INT32_MAX;
    }
    if (scaled < INT32_MIN) {
        *remainder = 0;
        return INT32_MIN;
    }
    return static_cast<LONG>(scaled);
}

MouseDevice* find_mouse(void* device) {
    for (auto& mouse : g_devices) {
        if (mouse.device == device) {
            return &mouse;
        }
    }
    return nullptr;
}

HRESULT WINAPI hook_get_device_state(void* device, DWORD size, void* state) {
    void** vtable = device ? *reinterpret_cast<void***>(device) : nullptr;
    GetDeviceStateFn original = nullptr;
    MouseDevice* mouse = nullptr;
    spin_lock(g_mouse_lock);
    for (const auto& hook : g_vtables) {
        if (hook.vtable == vtable) {
            original = hook.get_state;
        }
    }
    mouse = find_mouse(device);
    spin_unlock(g_mouse_lock);
    if (!original) {
        return E_FAIL;
    }
    const HRESULT result = original(device, size, state);
    if (SUCCEEDED(result) && state && mouse && (size == 16 || size == 20) &&
        g_scheduler_active.load(std::memory_order_acquire) != 0) {
        auto* axes = static_cast<LONG*>(state);
        axes[0] = scale_axis(axes[0], &mouse->x_remainder);
        axes[1] = scale_axis(axes[1], &mouse->y_remainder);
        if (g_mouse_logged.exchange(1) == 0) {
            LOG_INFO("Mouse scale active (%u/60)", g_target_fps.load());
        }
    }
    return result;
}

HRESULT WINAPI hook_get_device_data(void* device, DWORD object_size, void* objects, DWORD* count,
                                    DWORD flags) {
    void** vtable = device ? *reinterpret_cast<void***>(device) : nullptr;
    GetDeviceDataFn original = nullptr;
    MouseDevice* mouse = nullptr;
    spin_lock(g_mouse_lock);
    for (const auto& hook : g_vtables) {
        if (hook.vtable == vtable) {
            original = hook.get_data;
        }
    }
    mouse = find_mouse(device);
    spin_unlock(g_mouse_lock);
    if (!original) {
        return E_FAIL;
    }
    const HRESULT result = original(device, object_size, objects, count, flags);
    if (SUCCEEDED(result) && objects && count && object_size >= 8 && mouse &&
        g_scheduler_active.load(std::memory_order_acquire) != 0) {
        auto* object = static_cast<uint8_t*>(objects);
        for (DWORD i = 0; i < *count; ++i, object += object_size) {
            const DWORD offset = *reinterpret_cast<DWORD*>(object);
            auto* value = reinterpret_cast<LONG*>(object + sizeof(DWORD));
            if (offset == 0) {
                *value = scale_axis(*value, &mouse->x_remainder);
            } else if (offset == 4) {
                *value = scale_axis(*value, &mouse->y_remainder);
            }
        }
        if (g_mouse_logged.exchange(1) == 0) {
            LOG_INFO("Mouse scale active, buffered (%u/60)", g_target_fps.load());
        }
    }
    return result;
}

bool hook_mouse_device(void* device) {
    if (!device) {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(device);
    // IDirectInputDevice8: GetDeviceState is slot 9, GetDeviceData is slot 10.
    if (!vtable || !executable(vtable[9]) || !executable(vtable[10])) {
        return false;
    }
    spin_lock(g_mouse_lock);
    VtableHook* existing = nullptr;
    VtableHook* free_vtable = nullptr;
    for (auto& hook : g_vtables) {
        if (hook.vtable == vtable) {
            existing = &hook;
        } else if (!hook.vtable && !free_vtable) {
            free_vtable = &hook;
        }
    }
    if (!existing) {
        if (!free_vtable) {
            spin_unlock(g_mouse_lock);
            return false;
        }
        existing = free_vtable;
        existing->vtable = vtable;
        existing->get_state = reinterpret_cast<GetDeviceStateFn>(vtable[9]);
        existing->get_data = reinterpret_cast<GetDeviceDataFn>(vtable[10]);
        if (!patch_pointer(vtable + 9, reinterpret_cast<void*>(hook_get_device_state)) ||
            !patch_pointer(vtable + 10, reinterpret_cast<void*>(hook_get_device_data))) {
            existing->vtable = nullptr;
            existing->get_state = nullptr;
            existing->get_data = nullptr;
            spin_unlock(g_mouse_lock);
            return false;
        }
    }
    for (auto& mouse : g_devices) {
        if (mouse.device == device) {
            spin_unlock(g_mouse_lock);
            return true;
        }
    }
    for (auto& mouse : g_devices) {
        if (!mouse.device) {
            mouse.device = device;
            mouse.x_remainder = 0;
            mouse.y_remainder = 0;
            spin_unlock(g_mouse_lock);
            return true;
        }
    }
    spin_unlock(g_mouse_lock);
    return false;
}

HRESULT WINAPI hook_create_device(void* direct_input, const GUID* guid, void** output, void* outer) {
    void** vtable = direct_input ? *reinterpret_cast<void***>(direct_input) : nullptr;
    CreateDeviceFn original = nullptr;
    spin_lock(g_interface_lock);
    for (const auto& hook : g_interfaces) {
        if (hook.vtable == vtable) {
            original = hook.create_device;
        }
    }
    spin_unlock(g_interface_lock);
    if (!original) {
        return E_FAIL;
    }
    const HRESULT result = original(direct_input, guid, output, outer);
    if (SUCCEEDED(result) && output && *output && same_guid(guid) && !hook_mouse_device(*output)) {
        LOG_ERROR("Could not scale the system mouse (Win32=%lu)", GetLastError());
    }
    return result;
}

bool hook_interface(void* direct_input) {
    if (!direct_input) {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(direct_input);
    // IDirectInput8::CreateDevice is slot 3.
    if (!vtable || !executable(vtable[3])) {
        return false;
    }
    spin_lock(g_interface_lock);
    for (const auto& hook : g_interfaces) {
        if (hook.vtable == vtable) {
            const bool installed = vtable[3] == reinterpret_cast<void*>(hook_create_device);
            spin_unlock(g_interface_lock);
            return installed;
        }
    }
    for (auto& hook : g_interfaces) {
        if (!hook.vtable) {
            hook.vtable = vtable;
            hook.create_device = reinterpret_cast<CreateDeviceFn>(vtable[3]);
            if (!patch_pointer(vtable + 3, reinterpret_cast<void*>(hook_create_device))) {
                hook.vtable = nullptr;
                hook.create_device = nullptr;
                spin_unlock(g_interface_lock);
                return false;
            }
            spin_unlock(g_interface_lock);
            return true;
        }
    }
    spin_unlock(g_interface_lock);
    return false;
}

BOOL CALLBACK load_system(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length + 16 >= MAX_PATH) {
        return FALSE;
    }
    lstrcatW(path, L"\\dinput8.dll");
    g_system = LoadLibraryW(path);
    if (!g_system) {
        return FALSE;
    }
    g_create = reinterpret_cast<CreateFn>(GetProcAddress(g_system, "DirectInput8Create"));
    return g_create != nullptr;
}

}  // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version,
                                              const GUID* interface_id, void** output, void* outer) {
    if (!InitOnceExecuteOnce(&g_once, load_system, nullptr, nullptr) || !g_create) {
        return E_FAIL;
    }
    const HRESULT result = g_create(instance, version, interface_id, output, outer);
    if (SUCCEEDED(result) && output && *output && g_is_game.load(std::memory_order_acquire) != 0 &&
        !hook_interface(*output)) {
        LOG_ERROR("Could not watch DirectInput device creation (Win32=%lu)", GetLastError());
    }
    return result;
}
