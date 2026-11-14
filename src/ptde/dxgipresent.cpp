// DXVK presenting through DXGI: see dxgipresent.h.
#include "dxgipresent.h"

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.h"

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Real Vulkan entry points

PFN_vkGetInstanceProcAddr g_gipa = nullptr;  // the Vulkan loader's
VkInstance g_instance = VK_NULL_HANDLE;

struct InstanceFns {
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties GetPhysicalDeviceExternalSemaphoreProperties;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR GetPhysicalDeviceSurfaceCapabilities2KHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR GetPhysicalDeviceSurfaceFormats2KHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR GetPhysicalDeviceWin32PresentationSupportKHR;
};
InstanceFns vi{};

struct DeviceFns {
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2;
    PFN_vkQueueSubmit2 QueueSubmit2;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
};

// ---------------------------------------------------------------------------------------------------------------
// Objects

struct Window {
    HWND hwnd = nullptr;
    IDXGISwapChain1* chain = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT flags = 0;
};

struct Dev {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    DeviceFns fn{};
    bool dxgi = false;  // Direct3D 11 interop set up: our swap chains for its surfaces
    uint32_t family = 0;  // the graphics queue family
    VkPhysicalDeviceMemoryProperties memory{};
    ID3D11Device5* d3d = nullptr;
    ID3D11DeviceContext4* ctx = nullptr;
    IDXGIFactory2* factory = nullptr;
    bool tearing = false;
    ID3D11Fence* ready_fence = nullptr;    // signalled by Vulkan: frame done
    ID3D11Fence* release_fence = nullptr;  // signalled by Direct3D 11: copy done
    HANDLE ready_event = nullptr;          // paced presenting: set when ready_fence reaches a frame's value
    VkSemaphore ready_sem = VK_NULL_HANDLE;
    VkSemaphore release_sem = VK_NULL_HANDLE;
    uint64_t ready_value = 0;
    uint64_t release_value = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<Window*> windows;
};

struct Surface {
    HWND hwnd = nullptr;
    VkSurfaceKHR real = VK_NULL_HANDLE;  // when Vulkan's own is used
    bool ours = false;
};

struct Image {
    ID3D11Texture2D* tex = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer release_cb = VK_NULL_HANDLE;  // ownership to the outside after DXVK's frame
    VkCommandBuffer acquire_cb = VK_NULL_HANDLE;  // back for DXVK (contents not kept)
    std::atomic<uint64_t> released{0};           // release fence value after its last copy
};

constexpr uint32_t kMaxImages = 8;

struct Swap {
    Dev* dev = nullptr;
    Window* window = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t count = 0;
    uint32_t next = 0;
    Image images[kMaxImages];
};

// An acquire DXVK has not submitted a wait for yet. The copy-done value to wait for is read when that submission comes
// (our_submit2), not at the acquire: DXVK acquires on its CS thread and submits and presents on its submission thread,
// in order, so at the submission the image's last present has run and set it; at the acquire a CS thread a whole swap
// chain ahead could still see the value of the present before, and draw into the image before its copy.
struct Pending {
    VkSemaphore sem = VK_NULL_HANDLE;
    Swap* swap = nullptr;
    uint32_t index = 0;
};

std::mutex g_lock;  // registries and pending acquires
std::mutex g_d3d;   // the Direct3D 11 immediate context and the DXGI swap chains
std::vector<Dev*> g_devs;
std::vector<Surface*> g_surfaces;
std::vector<Swap*> g_swaps;
std::vector<std::pair<VkQueue, Dev*>> g_queues;
Pending g_pending[8];
std::atomic<int> g_pending_count{0};

// Statistics for dxgipresent_report.
std::atomic<uint32_t> g_presents{0};
std::atomic<uint32_t> g_patched{0};
std::atomic<uint64_t> g_present_us{0};
std::atomic<uint32_t> g_present_max_us{0};
std::atomic<long> g_last_hr{0};
std::atomic<int> g_composition{-1};  // DXGI_FRAME_PRESENTATION_MODE of the last sample, -1 none yet
bool g_logged_fence_acquire = false;
bool g_logged_present_error = false;

// Paced presenting (dxgipresent_set_pacing). With variable refresh the display starts a refresh when a frame is
// flipped, so the times of the flips are the times the player sees. Left to the GPU (the Direct3D 11 queue waits for
// Vulkan's frame, copies and flips whenever that is done), frames that DXVK's threads hand over unevenly can be
// flipped closer together than the display's fastest refresh, even under a frame rate cap below it (measured in
// Firelink at MaxFPS 225: 17% of the flips under the 4.17 ms of a 240 Hz display, the closest 2.4 ms): the display
// then sits at its ceiling and shows some frames late. Paced, the presenting thread itself waits for Vulkan's frame
// and then holds the flip until the display can refresh again:
//   - under a cap (MaxFPS) at or below the display's rate: no flip closer to the one before than the display's
//     fastest refresh (and 1.5%). The cap still sets the rate; only the frames that came too soon are
//     held, a fraction of a millisecond (same test, paced: none too close, the frame rate unchanged);
//   - without a cap, or with one above the display's rate: no hold (the display cannot follow anyway, and holding
//     would cap the game at the display's rate).
// 0 = off (as before), 1 = wait and hold, 2 = wait only (for measuring what unpaced flips are like).
std::atomic<int> g_pacing{0};
int (*g_cap_fps)() = nullptr;
std::atomic<int> g_refresh_mhz{0};  // the display under the window, in mHz (0 unknown)
std::atomic<int> g_test_refresh_mhz{0};  // development: paced as if the display had this rate
LONGLONG g_qpc_freq = 0;
LONGLONG g_last_flip = 0;           // presenting thread: when the last flip was asked for
HANDLE g_pace_timer = nullptr;      // presenting thread
bool g_pace_timer_tried = false;

// Flip spacing since the last report (the presenting thread adds, the report takes).
std::mutex g_spacing_lock;
constexpr uint32_t kSpacingSamples = 4096;
uint32_t g_spacing_us[kSpacingSamples];
uint32_t g_spacing_count = 0;
uint32_t g_spacing_close = 0;       // flips closer together than the display's fastest refresh
uint64_t g_ready_wait_us = 0;       // paced: waiting for Vulkan's frame
uint64_t g_hold_us = 0;             // paced: holding a finished frame for its step
uint32_t g_held = 0;
uint32_t g_ready_timeouts = 0;

LONGLONG qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// The presenting thread until `target` (performance counter ticks): a high resolution timer, the last 0.3 ms spun.
void pace_until(LONGLONG target) {
    if (!g_pace_timer_tried) {
        g_pace_timer_tried = true;
        g_pace_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */,
                                              SYNCHRONIZE | TIMER_MODIFY_STATE);
    }
    LONGLONG now = qpc_now();
    const LONGLONG spin = g_qpc_freq * 3 / 10000;
    while (g_pace_timer && now + spin < target) {
        LARGE_INTEGER due{};
        due.QuadPart = -((target - now - spin) * 10000000LL / g_qpc_freq);
        if (due.QuadPart >= 0 || !SetWaitableTimer(g_pace_timer, &due, 0, nullptr, nullptr, FALSE)) break;
        WaitForSingleObject(g_pace_timer, 50);
        now = qpc_now();
    }
    while (now < target) {
        YieldProcessor();
        now = qpc_now();
    }
}

// The refresh rate of the display the window is on.
void sample_refresh(HWND hwnd) {
    if (g_test_refresh_mhz.load() > 0) {
        g_refresh_mhz = g_test_refresh_mhz.load();
        return;
    }
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1) {
        g_refresh_mhz = static_cast<int>(dm.dmDisplayFrequency) * 1000;
    }
}

// Paced presenting, on the presenting thread after the submission that signals `ready`: wait for Vulkan's frame.
void pace_wait_ready(Dev* d, uint64_t ready) {
    if (g_qpc_freq <= 0) return;
    const LONGLONG t0 = qpc_now();
    bool timed_out = false;
    if (!d->ready_event) d->ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (d->ready_event && SUCCEEDED(d->ready_fence->SetEventOnCompletion(ready, d->ready_event))) {
        // A device that stops answering must not hang the game here: the GPU-side wait that follows still holds.
        timed_out = WaitForSingleObject(d->ready_event, 500) != WAIT_OBJECT_0;
    }
    const LONGLONG t1 = qpc_now();
    std::lock_guard<std::mutex> g(g_spacing_lock);
    g_ready_wait_us += static_cast<uint64_t>((t1 - t0) * 1000000 / g_qpc_freq);
    if (timed_out) ++g_ready_timeouts;
}

// Paced presenting, just before the flip is asked for (the copy is already queued): hold it until the display can
// refresh again.
void pace_hold() {
    if (g_qpc_freq <= 0 || !g_last_flip) return;
    const int cap = g_cap_fps ? g_cap_fps() : 0;
    const int mhz = g_refresh_mhz.load();
    if (cap <= 0 || mhz <= 0 || cap * 1000 > mhz) return;
    // 1.5% over the refresh time
    const LONGLONG target = g_last_flip + g_qpc_freq * 1015 / mhz;
    const LONGLONG t1 = qpc_now();
    if (t1 >= target) return;
    pace_until(target);
    const LONGLONG held = qpc_now() - t1;
    std::lock_guard<std::mutex> g(g_spacing_lock);
    g_hold_us += static_cast<uint64_t>(held * 1000000 / g_qpc_freq);
    ++g_held;
}

// The flip is asked for now: its spacing from the one before.
void note_flip() {
    if (g_qpc_freq <= 0) return;
    const LONGLONG now = qpc_now();
    if (g_last_flip) {
        const uint32_t us = static_cast<uint32_t>((std::min<LONGLONG>)((now - g_last_flip) * 1000000 / g_qpc_freq,
                                                                     60000000));
        const int mhz = g_refresh_mhz.load();
        std::lock_guard<std::mutex> g(g_spacing_lock);
        if (g_spacing_count < kSpacingSamples) g_spacing_us[g_spacing_count++] = us;
        if (mhz > 0 && us * static_cast<uint64_t>(mhz) < 1000000000ull) ++g_spacing_close;
    }
    g_last_flip = now;
}

// The NVIDIA driver's word on variable refresh for the display the window is on (NvAPI; nothing on other vendors).
// "in VRR mode" is the one that says the display is following the frames right now.
struct NvVrrInfo {
    uint32_t version;
    uint32_t flags;  // enabled, possible, requested, indicator, display in VRR mode
    uint32_t reserved[4];
};
HWND g_last_hwnd = nullptr;

bool nvidia_vrr(char* out, size_t size) {
    using Query = void* (__cdecl*)(uint32_t);
    using InitFn = int (__cdecl*)();
    using ByName = int (__cdecl*)(const char*, uint32_t*);
    using VrrFn = int (__cdecl*)(uint32_t, NvVrrInfo*);
    static bool tried = false;
    static ByName by_name = nullptr;
    static VrrFn vrr = nullptr;
    if (!tried) {
        tried = true;
        HMODULE nv = LoadLibraryExW(sizeof(void*) == 8 ? L"nvapi64.dll" : L"nvapi.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
        Query query = nv ? reinterpret_cast<Query>(GetProcAddress(nv, "nvapi_QueryInterface")) : nullptr;
        if (query) {
            InitFn init = reinterpret_cast<InitFn>(query(0x0150E828));
            if (init && init() == 0) {
                by_name = reinterpret_cast<ByName>(query(0xAE457190));
                vrr = reinterpret_cast<VrrFn>(query(0xDF8FDA57));
            }
        }
    }
    if (!by_name || !vrr || !g_last_hwnd) return false;
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(g_last_hwnd, MONITOR_DEFAULTTOPRIMARY);
    uint32_t id = 0;
    if (!mon || !GetMonitorInfoA(mon, &mi) || by_name(mi.szDevice, &id) != 0) return false;
    NvVrrInfo info{};
    info.version = sizeof(info) | (1u << 16);
    if (vrr(id, &info) != 0) return false;
    std::snprintf(out, size, "G-SYNC: set up for this display %s, possible %s, asked for by the game %s, display "
                  "following the frames now %s", (info.flags & 1) ? "yes" : "no", (info.flags & 2) ? "yes" : "no",
                  (info.flags & 4) ? "yes" : "no", (info.flags & 16) ? "yes" : "no");
    return true;
}

template <typename T>
uint64_t to_handle(T* p) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
}

Surface* find_surface(VkSurfaceKHR s) {
    std::lock_guard<std::mutex> g(g_lock);
    for (Surface* f : g_surfaces) {
        if (to_handle(f) == (uint64_t)(s)) return f;
    }
    return nullptr;
}

Swap* find_swap(VkSwapchainKHR s) {
    std::lock_guard<std::mutex> g(g_lock);
    for (Swap* w : g_swaps) {
        if (to_handle(w) == (uint64_t)(s)) return w;
    }
    return nullptr;
}

Dev* find_dev(VkDevice d) {
    std::lock_guard<std::mutex> g(g_lock);
    for (Dev* v : g_devs) {
        if (v->device == d) return v;
    }
    return nullptr;
}

Dev* find_queue(VkQueue q) {
    std::lock_guard<std::mutex> g(g_lock);
    for (auto& e : g_queues) {
        if (e.first == q) return e.second;
    }
    return nullptr;
}

Dev* any_dxgi_dev() {
    std::lock_guard<std::mutex> g(g_lock);
    for (Dev* v : g_devs) {
        if (v->dxgi) return v;
    }
    return nullptr;
}

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

VkExtent2D client_extent(HWND hwnd) {
    RECT r{};
    if (!hwnd || IsIconic(hwnd) || !GetClientRect(hwnd, &r)) return {0, 0};
    return {static_cast<uint32_t>(r.right - r.left), static_cast<uint32_t>(r.bottom - r.top)};
}

DXGI_FORMAT dxgi_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Extensions left out of what DXVK sees (the replacement swap chain has none of these)

const char* const kHiddenInstance[] = {
    "VK_KHR_get_surface_capabilities2", "VK_EXT_surface_maintenance1", "VK_KHR_surface_maintenance1",
    "VK_EXT_swapchain_colorspace",      "VK_KHR_surface_protected_capabilities",
};
const char* const kHiddenDevice[] = {
    "VK_KHR_present_id",          "VK_KHR_present_id2",
    "VK_KHR_present_wait",        "VK_KHR_present_wait2",
    "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1",
    "VK_EXT_full_screen_exclusive",  "VK_EXT_hdr_metadata",
    "VK_NV_low_latency2",         "VK_KHR_incremental_present",
    "VK_KHR_swapchain_mutable_format", "VK_GOOGLE_display_timing",
    "VK_EXT_present_mode_fifo_latest_ready", "VK_KHR_present_mode_fifo_latest_ready",
    "VK_AMD_display_native_hdr",  "VK_EXT_display_control",
    "VK_NV_present_barrier",      "VK_KHR_display_swapchain",
};

template <size_t N, typename Call>
VkResult filtered(const char* const (&hidden)[N], Call call, uint32_t* count, VkExtensionProperties* props) {
    uint32_t n = 0;
    VkResult r = call(&n, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkExtensionProperties> all(n);
    r = call(&n, all.data());
    if (r < 0) return r;
    all.resize(n);
    std::vector<VkExtensionProperties> kept;
    for (const auto& e : all) {
        bool hide = false;
        for (const char* h : hidden) hide = hide || std::strcmp(e.extensionName, h) == 0;
        if (!hide) kept.push_back(e);
    }
    if (!props) {
        *count = static_cast<uint32_t>(kept.size());
        return VK_SUCCESS;
    }
    const uint32_t k = *count < kept.size() ? *count : static_cast<uint32_t>(kept.size());
    if (k) std::memcpy(props, kept.data(), k * sizeof(VkExtensionProperties));
    *count = k;
    return k < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL our_enum_instance_ext(const char* layer, uint32_t* count,
                                                     VkExtensionProperties* props) {
    if (layer) return vi.EnumerateInstanceExtensionProperties(layer, count, props);
    return filtered(kHiddenInstance, [&](uint32_t* c, VkExtensionProperties* p) {
        return vi.EnumerateInstanceExtensionProperties(nullptr, c, p);
    }, count, props);
}

VKAPI_ATTR VkResult VKAPI_CALL our_enum_device_ext(VkPhysicalDevice pd, const char* layer, uint32_t* count,
                                                   VkExtensionProperties* props) {
    if (layer) return vi.EnumerateDeviceExtensionProperties(pd, layer, count, props);
    return filtered(kHiddenDevice, [&](uint32_t* c, VkExtensionProperties* p) {
        return vi.EnumerateDeviceExtensionProperties(pd, nullptr, c, p);
    }, count, props);
}

// ---------------------------------------------------------------------------------------------------------------
// Device: Direct3D 11 on the same adapter, the two shared fences

bool has_extension(const VkDeviceCreateInfo* ci, const char* name) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        if (std::strcmp(ci->ppEnabledExtensionNames[i], name) == 0) return true;
    }
    return false;
}

bool import_fence(Dev* d, ID3D11Fence** fence, VkSemaphore* sem, const char* what) {
    HRESULT hr = d->d3d->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void**>(fence));
    if (FAILED(hr)) {
        LOG_ERROR("DXGI present: Direct3D 11 could not make the %s fence (%08lX)", what, hr);
        return false;
    }
    HANDLE h = nullptr;
    hr = (*fence)->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(hr)) {
        LOG_ERROR("DXGI present: the %s fence could not be shared (%08lX)", what, hr);
        return false;
    }
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    VkResult vr = d->fn.CreateSemaphore(d->device, &sci, nullptr, sem);
    if (vr == VK_SUCCESS) {
        VkImportSemaphoreWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        imp.semaphore = *sem;
        imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        imp.handle = h;
        vr = d->fn.ImportSemaphoreWin32HandleKHR(d->device, &imp);
    }
    CloseHandle(h);
    if (vr != VK_SUCCESS) {
        LOG_ERROR("DXGI present: Vulkan could not take the %s fence (%d)", what, vr);
        return false;
    }
    return true;
}

template <typename Fn>
bool resolve(Dev* d, Fn& out, const char* name) {
    out = reinterpret_cast<Fn>(vi.GetDeviceProcAddr(d->device, name));
    return out != nullptr;
}

bool resolve_device(Dev* d) {
    bool ok = true;
#define RES(x) ok = resolve(d, d->fn.x, "vk" #x) && ok
    RES(DestroyDevice); RES(GetDeviceQueue); RES(GetDeviceQueue2); RES(QueueSubmit2); RES(CreateImage);
    RES(DestroyImage); RES(GetImageMemoryRequirements); RES(AllocateMemory); RES(FreeMemory); RES(BindImageMemory);
    RES(CreateSemaphore); RES(DestroySemaphore); RES(CreateCommandPool); RES(DestroyCommandPool);
    RES(AllocateCommandBuffers); RES(FreeCommandBuffers); RES(BeginCommandBuffer); RES(EndCommandBuffer);
    RES(CmdPipelineBarrier2); RES(CreateSwapchainKHR); RES(DestroySwapchainKHR); RES(GetSwapchainImagesKHR);
    RES(AcquireNextImageKHR); RES(QueuePresentKHR);
#undef RES
    // Interop only (may be missing when the driver lacks the extensions).
    resolve(d, d->fn.GetMemoryWin32HandlePropertiesKHR, "vkGetMemoryWin32HandlePropertiesKHR");
    resolve(d, d->fn.ImportSemaphoreWin32HandleKHR, "vkImportSemaphoreWin32HandleKHR");
    return ok;
}

// Whether this physical device can import Direct3D 11 textures and fences, and its LUID.
bool interop_supported(VkPhysicalDevice pd, LUID* luid, char* why, size_t why_size) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vi.GetPhysicalDeviceProperties2(pd, &props);
    if (!id.deviceLUIDValid) {
        std::snprintf(why, why_size, "the Vulkan device has no adapter LUID");
        return false;
    }
    std::memcpy(luid, id.deviceLUID, sizeof(LUID));

    VkPhysicalDeviceExternalImageFormatInfo ext_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkPhysicalDeviceImageFormatInfo2 fmt_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext_info};
    fmt_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    fmt_info.type = VK_IMAGE_TYPE_2D;
    fmt_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    fmt_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkExternalImageFormatProperties ext_props{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 fmt_props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ext_props};
    VkResult vr = vi.GetPhysicalDeviceImageFormatProperties2(pd, &fmt_info, &fmt_props);
    if (vr != VK_SUCCESS ||
        !(ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        std::snprintf(why, why_size, "Vulkan cannot import Direct3D 11 textures (%d, features %X)", vr,
                      ext_props.externalMemoryProperties.externalMemoryFeatures);
        return false;
    }

    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo sem_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &type};
    sem_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkExternalSemaphoreProperties sem_props{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vi.GetPhysicalDeviceExternalSemaphoreProperties(pd, &sem_info, &sem_props);
    if (!(sem_props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) {
        std::snprintf(why, why_size, "Vulkan cannot import Direct3D fences as timeline semaphores (features %X)",
                      sem_props.externalSemaphoreFeatures);
        return false;
    }
    return true;
}

bool create_d3d11(Dev* d, const LUID& luid, char* why, size_t why_size) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    using FactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto create_factory = dxgi ? reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    auto create_device = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"))
                               : nullptr;
    if (!create_factory || !create_device) {
        std::snprintf(why, why_size, "Direct3D 11 / DXGI 1.3 are not available");
        return false;
    }
    IDXGIFactory4* f4 = nullptr;
    HRESULT hr = create_factory(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&f4));
    if (FAILED(hr)) {
        std::snprintf(why, why_size, "no DXGI 1.4 factory (%08lX)", hr);
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    hr = f4->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
    if (FAILED(hr)) {
        f4->Release();
        std::snprintf(why, why_size, "DXGI has no adapter with Vulkan's LUID (%08lX)", hr);
        return false;
    }
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(f4->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
            d->tearing = tearing != FALSE;
        }
        f5->Release();
    }
    f4->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&d->factory));
    f4->Release();

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &dev, nullptr,
                       &ctx);
    adapter->Release();
    if (FAILED(hr)) {
        std::snprintf(why, why_size, "Direct3D 11 device creation failed (%08lX)", hr);
        return false;
    }
    dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&d->d3d));
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&d->ctx));
    dev->Release();
    ctx->Release();
    if (!d->d3d || !d->ctx || !d->factory) {
        std::snprintf(why, why_size, "Direct3D 11.4 (fences) is not available");
        return false;
    }
    // Frames in flight are already limited by DXVK and the frame limiter; two queued presents keep the GPU fed.
    IDXGIDevice1* dxgi_dev = nullptr;
    if (SUCCEEDED(d->d3d->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgi_dev)))) {
        dxgi_dev->SetMaximumFrameLatency(2);
        dxgi_dev->Release();
    }
    return true;
}

void destroy_dev_interop(Dev* d) {
    {
        std::lock_guard<std::mutex> g(g_d3d);
        for (Window* w : d->windows) {
            release(w->chain);
            delete w;
        }
        d->windows.clear();
        if (d->ctx) {
            d->ctx->ClearState();
            d->ctx->Flush();
        }
        release(d->ready_fence);
        release(d->release_fence);
        if (d->ready_event) CloseHandle(d->ready_event);
        d->ready_event = nullptr;
        release(d->ctx);
        release(d->d3d);
        release(d->factory);
    }
    if (d->pool) d->fn.DestroyCommandPool(d->device, d->pool, nullptr);
    if (d->ready_sem) d->fn.DestroySemaphore(d->device, d->ready_sem, nullptr);
    if (d->release_sem) d->fn.DestroySemaphore(d->device, d->release_sem, nullptr);
    d->pool = VK_NULL_HANDLE;
    d->ready_sem = d->release_sem = VK_NULL_HANDLE;
    d->dxgi = false;
}

VKAPI_ATTR VkResult VKAPI_CALL our_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                                 const VkAllocationCallbacks* alloc, VkDevice* out) {
    const VkResult vr = vi.CreateDevice(pd, ci, alloc, out);
    if (vr != VK_SUCCESS) return vr;
    Dev* d = new Dev;
    d->device = *out;
    d->phys = pd;
    if (!resolve_device(d)) {
        LOG_ERROR("DXGI present: some Vulkan device functions are missing; DXVK presents through Vulkan");
        delete d;
        return vr;
    }

    char why[160] = "";
    LUID luid{};
    bool ok = has_extension(ci, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) &&
              has_extension(ci, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) &&
              d->fn.GetMemoryWin32HandlePropertiesKHR && d->fn.ImportSemaphoreWin32HandleKHR;
    if (!ok) std::snprintf(why, sizeof(why), "DXVK's Vulkan device has no Win32 memory / semaphore sharing");
    ok = ok && interop_supported(pd, &luid, why, sizeof(why));
    ok = ok && create_d3d11(d, luid, why, sizeof(why));
    ok = ok && import_fence(d, &d->ready_fence, &d->ready_sem, "frame-ready") &&
         import_fence(d, &d->release_fence, &d->release_sem, "copy-done");

    // The graphics queue family: the first one asked for that does graphics (DXVK presents from it).
    if (ok) {
        uint32_t n = 0;
        vi.GetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
        std::vector<VkQueueFamilyProperties> fam(n);
        vi.GetPhysicalDeviceQueueFamilyProperties(pd, &n, fam.data());
        bool found = false;
        for (uint32_t i = 0; i < ci->queueCreateInfoCount && !found; ++i) {
            const uint32_t f = ci->pQueueCreateInfos[i].queueFamilyIndex;
            if (f < n && (fam[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                d->family = f;
                found = true;
            }
        }
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = d->family;
        ok = found && d->fn.CreateCommandPool(d->device, &pci, nullptr, &d->pool) == VK_SUCCESS;
        if (!ok) std::snprintf(why, sizeof(why), "no graphics queue / command pool");
    }
    vi.GetPhysicalDeviceMemoryProperties(pd, &d->memory);

    if (ok) {
        d->dxgi = true;
        LOG_INFO("DXGI present: DXVK's frames go out through a DXGI flip-model swap chain (Direct3D 11 interop; "
                 "tearing for variable refresh %s)", d->tearing ? "allowed" : "NOT available on this system");
    } else {
        destroy_dev_interop(d);
        LOG_ERROR("DXGI present: not possible here (%s); DXVK presents through Vulkan", why);
    }
    std::lock_guard<std::mutex> g(g_lock);
    g_devs.push_back(d);
    return vr;
}

template <typename Fn>
Fn device_fn(VkDevice device, const char* name) {
    return reinterpret_cast<Fn>(vi.GetDeviceProcAddr(device, name));
}

VKAPI_ATTR void VKAPI_CALL our_destroy_device(VkDevice device, const VkAllocationCallbacks* alloc) {
    Dev* d = find_dev(device);
    if (!d) {
        if (auto f = device_fn<PFN_vkDestroyDevice>(device, "vkDestroyDevice")) f(device, alloc);
        return;
    }
    const PFN_vkDestroyDevice destroy = d->fn.DestroyDevice;
    destroy_dev_interop(d);
    {
        std::lock_guard<std::mutex> g(g_lock);
        for (size_t i = 0; i < g_devs.size(); ++i) {
            if (g_devs[i] == d) {
                g_devs.erase(g_devs.begin() + i);
                break;
            }
        }
        for (size_t i = g_queues.size(); i-- > 0;) {
            if (g_queues[i].second == d) g_queues.erase(g_queues.begin() + i);
        }
    }
    delete d;
    destroy(device, alloc);
}

void remember_queue(VkDevice device, VkQueue q) {
    Dev* d = find_dev(device);
    if (!d || !q) return;
    std::lock_guard<std::mutex> g(g_lock);
    for (auto& e : g_queues) {
        if (e.first == q) return;
    }
    g_queues.emplace_back(q, d);
}

VKAPI_ATTR void VKAPI_CALL our_get_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue* q) {
    Dev* d = find_dev(device);
    if (!d) {
        device_fn<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue")(device, family, index, q);
        return;
    }
    d->fn.GetDeviceQueue(device, family, index, q);
    remember_queue(device, *q);
}

VKAPI_ATTR void VKAPI_CALL our_get_queue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* q) {
    Dev* d = find_dev(device);
    if (!d) {
        device_fn<PFN_vkGetDeviceQueue2>(device, "vkGetDeviceQueue2")(device, info, q);
        return;
    }
    d->fn.GetDeviceQueue2(device, info, q);
    remember_queue(device, *q);
}

// ---------------------------------------------------------------------------------------------------------------
// Surfaces

VKAPI_ATTR VkResult VKAPI_CALL our_create_surface(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* ci,
                                                  const VkAllocationCallbacks* alloc, VkSurfaceKHR* out) {
    Surface* s = new Surface;
    s->hwnd = ci->hwnd;
    s->ours = any_dxgi_dev() != nullptr;
    if (!s->ours) {
        const VkResult vr = vi.CreateWin32SurfaceKHR(instance, ci, alloc, &s->real);
        if (vr != VK_SUCCESS) {
            delete s;
            return vr;
        }
    }
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_surfaces.push_back(s);
    }
    *out = (VkSurfaceKHR)(to_handle(s));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL our_destroy_surface(VkInstance instance, VkSurfaceKHR surface,
                                               const VkAllocationCallbacks* alloc) {
    Surface* s = find_surface(surface);
    if (!s) {
        if (surface) vi.DestroySurfaceKHR(instance, surface, alloc);
        return;
    }
    if (s->real) vi.DestroySurfaceKHR(instance, s->real, alloc);
    std::lock_guard<std::mutex> g(g_lock);
    for (size_t i = 0; i < g_surfaces.size(); ++i) {
        if (g_surfaces[i] == s) {
            g_surfaces.erase(g_surfaces.begin() + i);
            break;
        }
    }
    delete s;
}

VkSurfaceKHR real_surface(VkSurfaceKHR s) {
    Surface* f = find_surface(s);
    return f ? f->real : s;
}

VKAPI_ATTR VkResult VKAPI_CALL our_surface_support(VkPhysicalDevice pd, uint32_t family, VkSurfaceKHR s,
                                                   VkBool32* supported) {
    Surface* f = find_surface(s);
    if (f && f->ours) {
        *supported = VK_TRUE;
        return VK_SUCCESS;
    }
    return vi.GetPhysicalDeviceSurfaceSupportKHR(pd, family, real_surface(s), supported);
}

VKAPI_ATTR VkBool32 VKAPI_CALL our_win32_support(VkPhysicalDevice pd, uint32_t family) {
    return vi.GetPhysicalDeviceWin32PresentationSupportKHR ? vi.GetPhysicalDeviceWin32PresentationSupportKHR(pd, family)
                                                           : VK_TRUE;
}

void fill_caps(const Surface* f, VkSurfaceCapabilitiesKHR* c) {
    const VkExtent2D e = client_extent(f->hwnd);
    std::memset(c, 0, sizeof(*c));
    c->minImageCount = 2;
    c->maxImageCount = kMaxImages;
    c->currentExtent = e;
    // A minimized window: no swap chain can be made (Vulkan's Win32 rule), DXVK then skips presenting.
    c->minImageExtent = e.width ? VkExtent2D{1, 1} : VkExtent2D{0, 0};
    c->maxImageExtent = e.width ? VkExtent2D{16384, 16384} : VkExtent2D{0, 0};
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL our_surface_caps(VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR* c) {
    Surface* f = find_surface(s);
    if (f && f->ours) {
        fill_caps(f, c);
        return VK_SUCCESS;
    }
    return vi.GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, real_surface(s), c);
}

VKAPI_ATTR VkResult VKAPI_CALL our_surface_caps2(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                 VkSurfaceCapabilities2KHR* c) {
    Surface* f = find_surface(info->surface);
    if (f && f->ours) {
        fill_caps(f, &c->surfaceCapabilities);
        return VK_SUCCESS;
    }
    VkPhysicalDeviceSurfaceInfo2KHR copy = *info;
    copy.surface = real_surface(info->surface);
    return vi.GetPhysicalDeviceSurfaceCapabilities2KHR(pd, &copy, c);
}

const VkSurfaceFormatKHR kFormats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
const VkPresentModeKHR kModes[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                   VK_PRESENT_MODE_FIFO_KHR};

template <typename T, size_t N>
VkResult copy_list(const T (&list)[N], uint32_t* count, T* out) {
    if (!out) {
        *count = N;
        return VK_SUCCESS;
    }
    const uint32_t k = *count < N ? *count : static_cast<uint32_t>(N);
    for (uint32_t i = 0; i < k; ++i) out[i] = list[i];
    *count = k;
    return k < N ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL our_surface_formats(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t* count,
                                                   VkSurfaceFormatKHR* out) {
    Surface* f = find_surface(s);
    if (f && f->ours) return copy_list(kFormats, count, out);
    return vi.GetPhysicalDeviceSurfaceFormatsKHR(pd, real_surface(s), count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL our_surface_formats2(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                    uint32_t* count, VkSurfaceFormat2KHR* out) {
    Surface* f = find_surface(info->surface);
    if (f && f->ours) {
        constexpr uint32_t n = sizeof(kFormats) / sizeof(kFormats[0]);
        if (!out) {
            *count = n;
            return VK_SUCCESS;
        }
        const uint32_t k = *count < n ? *count : n;
        for (uint32_t i = 0; i < k; ++i) out[i].surfaceFormat = kFormats[i];
        *count = k;
        return k < n ? VK_INCOMPLETE : VK_SUCCESS;
    }
    VkPhysicalDeviceSurfaceInfo2KHR copy = *info;
    copy.surface = real_surface(info->surface);
    return vi.GetPhysicalDeviceSurfaceFormats2KHR(pd, &copy, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL our_surface_modes(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t* count,
                                                 VkPresentModeKHR* out) {
    Surface* f = find_surface(s);
    if (f && f->ours) return copy_list(kModes, count, out);
    return vi.GetPhysicalDeviceSurfacePresentModesKHR(pd, real_surface(s), count, out);
}

// ---------------------------------------------------------------------------------------------------------------
// Swap chains

uint32_t memory_type(const Dev* d, uint32_t bits) {
    for (uint32_t i = 0; i < d->memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (d->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < 32; ++i) {
        if (bits & (1u << i)) return i;
    }
    return UINT32_MAX;
}

void record_barrier(Dev* d, VkCommandBuffer cb, VkImage image, bool release_to_outside) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    d->fn.BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    if (release_to_outside) {
        // DXVK left the frame in PRESENT_SRC; hand it to Direct3D 11.
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = d->family;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    } else {
        // Back for DXVK's next frame; the old contents are not needed (DXVK draws the whole image).
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    d->fn.CmdPipelineBarrier2(cb, &di);
    d->fn.EndCommandBuffer(cb);
}

void destroy_images(Swap* s) {
    Dev* d = s->dev;
    for (uint32_t i = 0; i < s->count; ++i) {
        Image& im = s->images[i];
        if (im.release_cb) d->fn.FreeCommandBuffers(d->device, d->pool, 1, &im.release_cb);
        if (im.acquire_cb) d->fn.FreeCommandBuffers(d->device, d->pool, 1, &im.acquire_cb);
        if (im.image) d->fn.DestroyImage(d->device, im.image, nullptr);
        if (im.memory) d->fn.FreeMemory(d->device, im.memory, nullptr);
        im.release_cb = im.acquire_cb = VK_NULL_HANDLE;
        im.image = VK_NULL_HANDLE;
        im.memory = VK_NULL_HANDLE;
        release(im.tex);
    }
    s->count = 0;
}

VkResult create_image(Swap* s, Image& im, const VkSwapchainCreateInfoKHR* ci) {
    Dev* d = s->dev;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = s->extent.width;
    td.Height = s->extent.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = dxgi_format(s->format);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = d->d3d->CreateTexture2D(&td, nullptr, &im.tex);
    if (FAILED(hr)) {
        LOG_ERROR("DXGI present: Direct3D 11 could not make a %ux%u swap image (%08lX)", td.Width, td.Height, hr);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    HANDLE h = nullptr;
    IDXGIResource1* res = nullptr;
    hr = im.tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res));
    if (SUCCEEDED(hr)) {
        hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
        res->Release();
    }
    if (FAILED(hr)) {
        LOG_ERROR("DXGI present: the swap image could not be shared (%08lX)", hr);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = s->format;
    ici.extent = {s->extent.width, s->extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = ci->imageUsage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult vr = d->fn.CreateImage(d->device, &ici, nullptr, &im.image);
    if (vr == VK_SUCCESS) {
        VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        d->fn.GetMemoryWin32HandlePropertiesKHR(d->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, h, &hp);
        VkMemoryRequirements req{};
        d->fn.GetImageMemoryRequirements(d->device, im.image, &req);
        uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits;
        if (!bits) bits = req.memoryTypeBits;
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = im.image;
        VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imp.handle = h;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memory_type(d, bits);
        vr = d->fn.AllocateMemory(d->device, &mai, nullptr, &im.memory);
        if (vr == VK_SUCCESS) vr = d->fn.BindImageMemory(d->device, im.image, im.memory, 0);
    }
    CloseHandle(h);  // an NT handle import does not take the handle over
    if (vr != VK_SUCCESS) {
        LOG_ERROR("DXGI present: Vulkan could not take the swap image (%d)", vr);
        return vr;
    }
    VkCommandBuffer cbs[2] = {};
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = d->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    vr = d->fn.AllocateCommandBuffers(d->device, &cai, cbs);
    if (vr != VK_SUCCESS) return vr;
    im.release_cb = cbs[0];
    im.acquire_cb = cbs[1];
    record_barrier(d, im.release_cb, im.image, true);
    record_barrier(d, im.acquire_cb, im.image, false);
    im.released = 0;
    return VK_SUCCESS;
}

// The DXGI swap chain of a window (one per window for the device's life), sized and formatted for this Vulkan one.
bool fit_window(Swap* s, HWND hwnd) {
    Dev* d = s->dev;
    Window* w = nullptr;
    for (Window* x : d->windows) {
        if (x->hwnd == hwnd) w = x;
    }
    const DXGI_FORMAT fmt = dxgi_format(s->format);
    const UINT flags = d->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    std::lock_guard<std::mutex> g(g_d3d);
    if (!w) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = s->extent.width;
        desc.Height = s->extent.height;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = flags;
        IDXGISwapChain1* chain = nullptr;
        const HRESULT hr = d->factory->CreateSwapChainForHwnd(d->d3d, hwnd, &desc, nullptr, nullptr, &chain);
        if (FAILED(hr)) {
            LOG_ERROR("DXGI present: no DXGI swap chain for the game window (%08lX)", hr);
            return false;
        }
        d->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
        w = new Window;
        w->hwnd = hwnd;
        w->chain = chain;
        w->width = desc.Width;
        w->height = desc.Height;
        w->format = fmt;
        w->flags = flags;
        d->windows.push_back(w);
        LOG_INFO("DXGI present: DXGI flip-model swap chain %ux%u on the game window", desc.Width, desc.Height);
    } else if (w->width != s->extent.width || w->height != s->extent.height || w->format != fmt) {
        d->ctx->ClearState();
        d->ctx->Flush();
        const HRESULT hr = w->chain->ResizeBuffers(0, s->extent.width, s->extent.height, fmt, w->flags);
        if (FAILED(hr)) {
            LOG_ERROR("DXGI present: resizing the DXGI swap chain to %ux%u failed (%08lX)", s->extent.width,
                      s->extent.height, hr);
            return false;
        }
        w->width = s->extent.width;
        w->height = s->extent.height;
        w->format = fmt;
        LOG_INFO("DXGI present: DXGI swap chain now %ux%u", w->width, w->height);
    }
    s->window = w;
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL our_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                    const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    Dev* d = find_dev(device);
    Surface* f = find_surface(ci->surface);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (!f || !f->ours) {
        VkSwapchainCreateInfoKHR copy = *ci;
        copy.surface = f ? f->real : ci->surface;
        return d->fn.CreateSwapchainKHR(device, &copy, alloc, out);
    }
    if (!d->dxgi || dxgi_format(ci->imageFormat) == DXGI_FORMAT_UNKNOWN || !ci->imageExtent.width ||
        !ci->imageExtent.height) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Swap* s = new Swap;
    s->dev = d;
    s->format = ci->imageFormat;
    s->extent = ci->imageExtent;
    s->mode = ci->presentMode;
    s->count = ci->minImageCount < 2 ? 2 : ci->minImageCount > kMaxImages ? kMaxImages : ci->minImageCount;
    if (!fit_window(s, f->hwnd)) {
        delete s;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < s->count; ++i) {
        const VkResult vr = create_image(s, s->images[i], ci);
        if (vr != VK_SUCCESS) {
            s->count = i + 1;
            destroy_images(s);
            delete s;
            return vr;
        }
    }
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_swaps.push_back(s);
    }
    static const char* const kModeNames[] = {"immediate", "mailbox", "FIFO", "FIFO relaxed"};
    LOG_INFO("DXGI present: swap chain %ux%u, %u images, %s mode", s->extent.width, s->extent.height, s->count,
             s->mode <= 3 ? kModeNames[s->mode] : "other");
    *out = (VkSwapchainKHR)(to_handle(s));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL our_destroy_swapchain(VkDevice device, VkSwapchainKHR sc,
                                                 const VkAllocationCallbacks* alloc) {
    Swap* s = find_swap(sc);
    if (!s) {
        Dev* d = find_dev(device);
        if (d && sc) d->fn.DestroySwapchainKHR(device, sc, alloc);
        return;
    }
    {
        std::lock_guard<std::mutex> g(g_lock);
        for (size_t i = 0; i < g_swaps.size(); ++i) {
            if (g_swaps[i] == s) {
                g_swaps.erase(g_swaps.begin() + i);
                break;
            }
        }
        for (Pending& p : g_pending) {
            if (p.swap == s) {
                p = Pending{};
                g_pending_count.fetch_sub(1);
            }
        }
    }
    // DXVK waits for its device to go idle first (it has no present fences without swapchain maintenance); wait for
    // Direct3D 11's copies too, then the shared textures can go.
    Dev* d = s->dev;
    {
        std::lock_guard<std::mutex> g(g_d3d);
        d->ctx->Flush();
        const uint64_t last = d->release_value;
        if (d->release_fence->GetCompletedValue() < last) {
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (e && SUCCEEDED(d->release_fence->SetEventOnCompletion(last, e))) WaitForSingleObject(e, 2000);
            if (e) CloseHandle(e);
        }
    }
    destroy_images(s);
    delete s;
}

VKAPI_ATTR VkResult VKAPI_CALL our_swapchain_images(VkDevice device, VkSwapchainKHR sc, uint32_t* count,
                                                    VkImage* out) {
    Swap* s = find_swap(sc);
    if (!s) {
        Dev* d = find_dev(device);
        return d ? d->fn.GetSwapchainImagesKHR(device, sc, count, out) : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!out) {
        *count = s->count;
        return VK_SUCCESS;
    }
    const uint32_t k = *count < s->count ? *count : s->count;
    for (uint32_t i = 0; i < k; ++i) out[i] = s->images[i].image;
    *count = k;
    return k < s->count ? VK_INCOMPLETE : VK_SUCCESS;
}

bool same_extent(VkExtent2D a, VkExtent2D b) {
    return a.width == b.width && a.height == b.height;
}

VKAPI_ATTR VkResult VKAPI_CALL our_acquire(VkDevice device, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem,
                                           VkFence fence, uint32_t* index) {
    Swap* s = find_swap(sc);
    if (!s) {
        Dev* d = find_dev(device);
        return d ? d->fn.AcquireNextImageKHR(device, sc, timeout, sem, fence, index) : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!same_extent(client_extent(s->window->hwnd), s->extent)) return VK_ERROR_OUT_OF_DATE_KHR;
    if (fence && !g_logged_fence_acquire) {
        g_logged_fence_acquire = true;
        LOG_ERROR("DXGI present: DXVK acquired a swap image with a fence, which this replacement does not signal");
    }
    std::lock_guard<std::mutex> g(g_lock);
    const uint32_t i = s->next;
    s->next = (i + 1) % s->count;
    if (sem) {
        Pending* slot = nullptr;
        for (Pending& p : g_pending) {
            if (p.sem == sem) slot = &p;  // the same semaphore again: the old entry was never used
        }
        if (!slot) {
            for (Pending& p : g_pending) {
                if (!p.sem && !slot) slot = &p;
            }
            if (slot) g_pending_count.fetch_add(1);
        }
        if (slot) {
            slot->sem = sem;
            slot->swap = s;
            slot->index = i;
        }
    }
    *index = i;
    return VK_SUCCESS;
}

// Every queue submission: one that waits for an acquire semaphore of ours waits for Direct3D 11's copy-done value of
// that image instead, and takes the image back first.
VKAPI_ATTR VkResult VKAPI_CALL our_submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
                                           VkFence fence) {
    Dev* d = find_queue(queue);
    if (!d) {
        // Not a queue we saw handed out: the first device's function is the only one there is.
        std::lock_guard<std::mutex> g(g_lock);
        if (g_devs.empty()) return VK_ERROR_INITIALIZATION_FAILED;
        d = g_devs.front();
    }
    if (g_pending_count.load() == 0) return d->fn.QueueSubmit2(queue, count, submits, fence);

    std::vector<VkSubmitInfo2> subs;
    std::vector<std::vector<VkSemaphoreSubmitInfo>> waits;
    std::vector<std::vector<VkCommandBufferSubmitInfo>> cbs;
    {
        std::lock_guard<std::mutex> g(g_lock);
        for (uint32_t k = 0; k < count; ++k) {
            const VkSubmitInfo2& si = submits[k];
            for (uint32_t j = 0; j < si.waitSemaphoreInfoCount; ++j) {
                for (Pending& p : g_pending) {
                    if (!p.sem || p.sem != si.pWaitSemaphoreInfos[j].semaphore) continue;
                    if (subs.empty()) {
                        subs.assign(submits, submits + count);
                        waits.resize(count);
                        cbs.resize(count);
                    }
                    if (waits[k].empty()) {
                        waits[k].assign(si.pWaitSemaphoreInfos, si.pWaitSemaphoreInfos + si.waitSemaphoreInfoCount);
                        cbs[k].assign(si.pCommandBufferInfos, si.pCommandBufferInfos + si.commandBufferInfoCount);
                    }
                    VkSemaphoreSubmitInfo& w = waits[k][j];
                    w.semaphore = d->release_sem;
                    w.value = p.swap->images[p.index].released.load();
                    w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                    cbi.commandBuffer = p.swap->images[p.index].acquire_cb;
                    cbs[k].insert(cbs[k].begin(), cbi);
                    p = Pending{};
                    g_pending_count.fetch_sub(1);
                    g_patched.fetch_add(1);
                }
            }
        }
    }
    if (subs.empty()) return d->fn.QueueSubmit2(queue, count, submits, fence);
    for (uint32_t k = 0; k < count; ++k) {
        if (waits[k].empty()) continue;
        subs[k].pWaitSemaphoreInfos = waits[k].data();
        subs[k].pCommandBufferInfos = cbs[k].data();
        subs[k].commandBufferInfoCount = static_cast<uint32_t>(cbs[k].size());
    }
    return d->fn.QueueSubmit2(queue, count, subs.data(), fence);
}

VKAPI_ATTR VkResult VKAPI_CALL our_present(VkQueue queue, const VkPresentInfoKHR* pi) {
    Swap* s = pi->swapchainCount == 1 ? find_swap(pi->pSwapchains[0]) : nullptr;
    if (!s) {
        Dev* d = find_queue(queue);
        if (!d) {
            std::lock_guard<std::mutex> g(g_lock);
            if (g_devs.empty()) return VK_ERROR_INITIALIZATION_FAILED;
            d = g_devs.front();
        }
        return d->fn.QueuePresentKHR(queue, pi);
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    Dev* d = s->dev;
    const uint32_t i = pi->pImageIndices[0];
    Image& im = s->images[i < s->count ? i : 0];

    // Vulkan: after DXVK's frame, hand the image out and signal frame-ready.
    VkSemaphoreSubmitInfo waits[8];
    const uint32_t nw = pi->waitSemaphoreCount < 8 ? pi->waitSemaphoreCount : 8;
    for (uint32_t k = 0; k < nw; ++k) {
        waits[k] = VkSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[k].semaphore = pi->pWaitSemaphores[k];
        waits[k].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = im.release_cb;
    const uint64_t ready = ++d->ready_value;
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = d->ready_sem;
    sig.value = ready;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = nw;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    VkResult vr = d->fn.QueueSubmit2(queue, 1, &si, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) return vr;

    if (g_qpc_freq <= 0) {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f)) g_qpc_freq = f.QuadPart;
    }
    g_last_hwnd = s->window->hwnd;
    if ((g_presents.load() & 255) == 0) sample_refresh(s->window->hwnd);
    const int pacing = g_pacing.load();
    if (pacing) pace_wait_ready(d, ready);

    // Direct3D 11: wait for it on the GPU, copy into the DXGI back buffer, signal copy-done, present.
    HRESULT hr;
    {
        std::lock_guard<std::mutex> g(g_d3d);
        d->ctx->Wait(d->ready_fence, ready);
        ID3D11Texture2D* back = nullptr;
        hr = s->window->chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
        if (SUCCEEDED(hr)) {
            d->ctx->CopyResource(back, im.tex);
            back->Release();
        }
        const uint64_t done = ++d->release_value;
        d->ctx->Signal(d->release_fence, done);
        im.released = done;
        UINT sync = 1, flags = 0;
        if (s->mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            sync = 0;
            flags = d->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        } else if (s->mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            sync = 0;
        }
        if (pacing == 1 && sync == 0) pace_hold();  // with vertical sync DXGI spaces the flips itself
        note_flip();
        if (SUCCEEDED(hr)) hr = s->window->chain->Present(sync, flags);
        // How Windows shows the frames (composed by the desktop window manager, or flipped straight to the display:
        // what variable refresh needs), sampled now and then.
        if ((g_presents.load() & 255) == 255) {
            IDXGISwapChainMedia* media = nullptr;
            if (SUCCEEDED(s->window->chain->QueryInterface(__uuidof(IDXGISwapChainMedia),
                                                            reinterpret_cast<void**>(&media)))) {
                DXGI_FRAME_STATISTICS_MEDIA st{};
                if (SUCCEEDED(media->GetFrameStatisticsMedia(&st))) g_composition = static_cast<int>(st.CompositionMode);
                media->Release();
            }
        }
    }
    g_last_hr = hr;
    if (FAILED(hr) && !g_logged_present_error) {
        g_logged_present_error = true;
        LOG_ERROR("DXGI present: Present failed (%08lX)", hr);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    const uint32_t us = static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    g_present_us.fetch_add(us);
    uint32_t prev = g_present_max_us.load();
    while (us > prev && !g_present_max_us.compare_exchange_weak(prev, us)) {
    }
    g_presents.fetch_add(1);

    vr = same_extent(client_extent(s->window->hwnd), s->extent) ? VK_SUCCESS : VK_SUBOPTIMAL_KHR;
    if (pi->pResults) pi->pResults[0] = vr;
    return vr;
}

// ---------------------------------------------------------------------------------------------------------------
// Lookup

PFN_vkVoidFunction VKAPI_CALL our_gdpa(VkDevice device, const char* name);

struct Entry {
    const char* name;
    PFN_vkVoidFunction fn;
};

#define E(n, f) {n, reinterpret_cast<PFN_vkVoidFunction>(&f)}
const Entry kDeviceEntries[] = {
    E("vkGetDeviceProcAddr", our_gdpa),
    E("vkDestroyDevice", our_destroy_device),
    E("vkGetDeviceQueue", our_get_queue),
    E("vkGetDeviceQueue2", our_get_queue2),
    E("vkQueueSubmit2", our_submit2),
    E("vkQueueSubmit2KHR", our_submit2),
    E("vkCreateSwapchainKHR", our_create_swapchain),
    E("vkDestroySwapchainKHR", our_destroy_swapchain),
    E("vkGetSwapchainImagesKHR", our_swapchain_images),
    E("vkAcquireNextImageKHR", our_acquire),
    E("vkQueuePresentKHR", our_present),
};
#undef E

PFN_vkVoidFunction device_entry(const char* name) {
    for (const Entry& e : kDeviceEntries) {
        if (std::strcmp(e.name, name) == 0) return e.fn;
    }
    return nullptr;
}

PFN_vkVoidFunction VKAPI_CALL our_gdpa(VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (PFN_vkVoidFunction f = device_entry(name)) {
        // Only for functions the device really has (a missing one stays missing).
        return vi.GetDeviceProcAddr(device, name) ? f : nullptr;
    }
    return vi.GetDeviceProcAddr(device, name);
}

PFN_vkVoidFunction VKAPI_CALL our_gipa(VkInstance instance, const char* name);

void set_instance(VkInstance instance) {
    g_instance = instance;
#define RI(x) vi.x = reinterpret_cast<PFN_vk##x>(g_gipa(instance, "vk" #x))
    RI(EnumerateDeviceExtensionProperties); RI(CreateDevice); RI(GetDeviceProcAddr);
    RI(GetPhysicalDeviceProperties2); RI(GetPhysicalDeviceMemoryProperties); RI(GetPhysicalDeviceQueueFamilyProperties);
    RI(GetPhysicalDeviceImageFormatProperties2); RI(GetPhysicalDeviceExternalSemaphoreProperties);
    RI(CreateWin32SurfaceKHR); RI(DestroySurfaceKHR); RI(GetPhysicalDeviceSurfaceSupportKHR);
    RI(GetPhysicalDeviceSurfaceCapabilitiesKHR); RI(GetPhysicalDeviceSurfaceCapabilities2KHR);
    RI(GetPhysicalDeviceSurfaceFormatsKHR); RI(GetPhysicalDeviceSurfaceFormats2KHR);
    RI(GetPhysicalDeviceSurfacePresentModesKHR); RI(GetPhysicalDeviceWin32PresentationSupportKHR);
#undef RI
}

PFN_vkVoidFunction VKAPI_CALL our_gipa(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&our_gipa);
    if (!instance) {
        if (std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0) {
            vi.EnumerateInstanceExtensionProperties =
                reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(g_gipa(nullptr, name));
            return vi.EnumerateInstanceExtensionProperties
                       ? reinterpret_cast<PFN_vkVoidFunction>(&our_enum_instance_ext)
                       : nullptr;
        }
        return g_gipa(nullptr, name);
    }
    if (instance != g_instance) set_instance(instance);
    const PFN_vkVoidFunction real = g_gipa(instance, name);
    if (!real) return nullptr;
#define I(n, f) if (std::strcmp(name, n) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&f)
    I("vkEnumerateDeviceExtensionProperties", our_enum_device_ext);
    I("vkCreateDevice", our_create_device);
    I("vkCreateWin32SurfaceKHR", our_create_surface);
    I("vkDestroySurfaceKHR", our_destroy_surface);
    I("vkGetPhysicalDeviceSurfaceSupportKHR", our_surface_support);
    I("vkGetPhysicalDeviceWin32PresentationSupportKHR", our_win32_support);
    I("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", our_surface_caps);
    I("vkGetPhysicalDeviceSurfaceCapabilities2KHR", our_surface_caps2);
    I("vkGetPhysicalDeviceSurfaceFormatsKHR", our_surface_formats);
    I("vkGetPhysicalDeviceSurfaceFormats2KHR", our_surface_formats2);
    I("vkGetPhysicalDeviceSurfacePresentModesKHR", our_surface_modes);
#undef I
    if (PFN_vkVoidFunction f = device_entry(name)) return f;
    return real;
}

// ---------------------------------------------------------------------------------------------------------------
// DXVK's GetProcAddress

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_get_proc = nullptr;

FARPROC WINAPI hook_get_proc(HMODULE module, LPCSTR name) {
    FARPROC p = g_get_proc(module, name);
    if (p && HIWORD(reinterpret_cast<uintptr_t>(name)) && std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
        g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(p);
        return reinterpret_cast<FARPROC>(&our_gipa);
    }
    return p;
}

void** import_slot(HMODULE module, const char* dll, const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                                                        : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            auto* n = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(n->Name), name) == 0) {
                return reinterpret_cast<void**>(&slots[i].u1.Function);
            }
        }
    }
    return nullptr;
}

}  // namespace

bool dxgipresent_install(HMODULE dxvk) {
    void** slot = import_slot(dxvk, "KERNEL32.dll", "GetProcAddress");
    if (!slot) {
        LOG_ERROR("DXGI present: DXVK's GetProcAddress import was not found; DXVK presents through Vulkan");
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG_ERROR("DXGI present: DXVK's import table could not be written (%lu)", GetLastError());
        return false;
    }
    g_get_proc = reinterpret_cast<GetProcAddressFn>(*slot);
    *slot = reinterpret_cast<void*>(&hook_get_proc);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

const char* dxgipresent_composition() {
    switch (g_composition.load()) {
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: return "composed by the desktop window manager";
    case DXGI_FRAME_PRESENTATION_MODE_OVERLAY: return "hardware overlay";
    case DXGI_FRAME_PRESENTATION_MODE_NONE: return "flipped straight to the display";
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE: return "composition failure";
    default: return "not known yet";
    }
}

void dxgipresent_report() {
    const uint32_t n = g_presents.exchange(0);
    if (!n) return;
    const uint64_t us = g_present_us.exchange(0);
    const uint32_t max_us = g_present_max_us.exchange(0);
    LOG_INFO("DXGI present: %u frames, present call avg %.2f ms max %.2f ms, %u acquire waits rewritten, last result "
             "%08lX, %s", n, us / 1000.0 / n, max_us / 1000.0, g_patched.exchange(0),
             static_cast<unsigned long>(g_last_hr.load()), dxgipresent_composition());

    static uint32_t sorted[kSpacingSamples];
    uint32_t count, close_count, held, timeouts;
    uint64_t wait_us, hold_us;
    {
        std::lock_guard<std::mutex> g(g_spacing_lock);
        count = g_spacing_count;
        std::memcpy(sorted, g_spacing_us, count * sizeof(uint32_t));
        close_count = g_spacing_close;
        held = g_held;
        timeouts = g_ready_timeouts;
        wait_us = g_ready_wait_us;
        hold_us = g_hold_us;
        g_spacing_count = g_spacing_close = g_held = g_ready_timeouts = 0;
        g_ready_wait_us = g_hold_us = 0;
    }
    if (count >= 8) {
        std::sort(sorted, sorted + count);
        uint64_t sum = 0;
        for (uint32_t k = 0; k < count; ++k) sum += sorted[k];
        const int pacing = g_pacing.load();
        const int mhz = g_refresh_mhz.load();
        char paced[160] = "not paced (the GPU flips each frame when it is done; these are the times they were handed over)";
        if (pacing) {
            std::snprintf(paced, sizeof(paced), "%s: waited for the frame avg %.2f ms, held %u frames avg %.2f ms%s",
                          pacing == 1 ? "paced" : "waiting for each frame, not held", wait_us / 1000.0 / n, held,
                          held ? hold_us / 1000.0 / held : 0.0, timeouts ? ", SOME WAITS TIMED OUT" : "");
        }
        LOG_INFO("DXGI present: flips %.2f ms apart on average (1%% %.2f, 10%% %.2f, half %.2f, 90%% %.2f, 99%% %.2f, "
                 "longest %.2f ms); %u of %u closer than the display's fastest refresh (%.2f ms at %.0f Hz); %s",
                 sum / 1000.0 / count, sorted[count / 100] / 1000.0, sorted[count / 10] / 1000.0,
                 sorted[count / 2] / 1000.0, sorted[count * 9 / 10] / 1000.0, sorted[count * 99 / 100] / 1000.0,
                 sorted[count - 1] / 1000.0, close_count, count, mhz > 0 ? 1000000.0 / mhz : 0.0, mhz / 1000.0, paced);
    }
    char nv[200];
    if (nvidia_vrr(nv, sizeof(nv))) LOG_INFO("DXGI present: %s", nv);
}

void dxgipresent_set_pacing(int mode, int (*cap_fps)()) {
    if (cap_fps) g_cap_fps = cap_fps;
    g_pacing = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

int dxgipresent_pacing() {
    return g_pacing.load();
}

void dxgipresent_test_refresh(int hz) {
    g_test_refresh_mhz = hz > 0 ? hz * 1000 : 0;
    if (hz > 0) g_refresh_mhz = hz * 1000;
}
