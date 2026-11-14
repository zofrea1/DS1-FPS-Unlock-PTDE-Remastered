// DXVK presenting through DXGI: see dxgipresent.h.
#include "dxgipresent.h"

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "gamma.h"
#include "log.h"
#include "shaders/dxgi_scrgb_ps.h"
#include "shaders/dxgi_scrgb_vs.h"

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
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the frames' (Vulkan's) format
    UINT flags = 0;
    bool scrgb = false;        // the DXGI swap chain is FP16 scRGB (DxvkHdrSwapChain on a display in HDR)
    bool display_hdr = false;  // Windows runs the window's display in HDR (looked at every 128 frames)
    float white = 1.0f;        // its SDR brightness setting / 80 nits
    LONGLONG looked = 0;       // when the display's colour mode was last read (only with DxvkHdrSwapChain)
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
    uint64_t ready_value = 0;                  // DXVK's submission thread
    std::atomic<uint64_t> release_value{0};    // taken on DXVK's submission thread, signalled in that order
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<Window*> windows;
    // DxvkHdrSwapChain: the SDR picture into scRGB (shaders/dxgi_scrgb.hlsl)
    ID3D11VertexShader* scrgb_vs = nullptr;
    ID3D11PixelShader* scrgb_ps = nullptr;
    ID3D11Buffer* scrgb_cb = nullptr;
    float scrgb_cb_white = -1.0f;
    bool scrgb_failed = false;
    // Timed presenting: signalled when the next frame's copy into the back buffer is done (the flip waits for it on
    // the CPU, so Present itself has no GPU work in front of it).
    ID3D11Fence* back_fence = nullptr;
    HANDLE back_event = nullptr;
    uint64_t back_value = 0;
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
    ID3D11ShaderResourceView* srv = nullptr;     // DxvkHdrSwapChain: read by the scRGB pass
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
    // Timed presenting: each finished frame is copied into one of these at once (DXVK has its image back) and flipped
    // from here at its time.
    static constexpr uint32_t kStages = 4;
    ID3D11Texture2D* stage[kStages] = {};
    ID3D11ShaderResourceView* stage_srv[kStages] = {};
    std::atomic<bool> stage_busy[kStages] = {};
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

// Timed presenting (dxgipresent_set_pacing). With variable refresh the display starts a refresh when a frame is
// flipped, so the times of the flips are the times the player sees the pictures, and motion is smooth when they are
// spaced like the game's own time steps: the time between two frame boundaries of the draw thread (frame.cpp), which
// is what each picture moved the world by. Native Direct3D 9 comes close by itself: its flip follows the game's
// Present after a short GPU tail. Under DXVK a frame passes DXVK's command thread, its submission thread and the GPU
// before Direct3D 11 can copy and flip it, and that takes a different time each frame (a log at MaxFPS 225: flips
// 3.5 to 5.3 ms apart around steps of 4.44 ms, 8% of them closer than a 240 Hz display can refresh), so the
// display's refresh rate wanders and turning the camera is not smooth.
//
// Mode 1 (timed): a presenting thread of ours takes each finished frame and flips it a set delay after its frame
// boundary, so the flips are spaced exactly like the steps, whatever happened in DXVK's threads in between. The
// delay follows how late frames are ready (99th percentile of the last 256 plus 0.25 ms; it rises by up to 0.25 ms
// a frame and falls by at most 0.01 ms a frame, so the spacing never jumps); a frame ready after its time is
// flipped at once. Under a frame rate cap at or below the display's refresh rate no flip comes closer to the one
// before than the display's fastest refresh. DXVK's submission thread no longer waits for Direct3D 11.
// Mode 2: the same thread flips each frame as soon as it is ready (the old timing, measured the same way).
// Mode 0: no presenting thread; Direct3D 11 waits for the frame on the GPU and flips it when done (before 2.3.0).
std::atomic<int> g_pacing{0};
int (*g_cap_fps)() = nullptr;
std::atomic<int> g_refresh_mhz{0};  // the display under the window, in mHz (0 unknown)
std::atomic<int> g_test_refresh_mhz{0};  // development: paced as if the display had this rate
LONGLONG g_qpc_freq = 0;
LONGLONG g_last_flip = 0;           // the presenting thread: when the last flip was asked for
HANDLE g_pace_timer = nullptr;      // presenting thread
bool g_pace_timer_tried = false;

// The frame boundary of each picture the game presents (dxgipresent_frame_presented, the game's draw thread), taken
// in order by DXVK's presents of them (one each).
std::mutex g_content_lock;
constexpr uint32_t kContentSlots = 8;
LONGLONG g_content[kContentSlots];
uint32_t g_content_first = 0, g_content_count = 0;
uint32_t g_content_backlog = 0;     // presents in a row that found four or more queued
uint32_t g_content_depth_sum = 0, g_content_takes = 0;  // how deep the queue was when DXVK presented
uint32_t g_content_dropped = 0;     // entries dropped to get back in step
uint32_t g_content_missing = 0;     // presents with none queued

// A frame for the presenting thread.
struct Job {
    Swap* swap = nullptr;
    uint32_t index = 0;
    uint64_t ready = 0;     // frame-ready value Vulkan signals
    uint64_t done = 0;      // copy-done value to signal (handed to the image's next acquire already)
    LONGLONG content = 0;   // its frame boundary (0 not known)
    UINT sync = 0, flags = 0;
};
std::mutex g_job_lock;
std::vector<Job> g_jobs;            // in order
HANDLE g_job_event = nullptr;
HANDLE g_presenter = nullptr;
std::atomic<int> g_jobs_open{0};    // queued, being copied, waiting to flip or being flipped
void drain_jobs();

// A copied frame for the flipping thread.
struct Flip {
    uint64_t seq = 0;
    Swap* swap = nullptr;
    uint32_t stage = 0;
    LONGLONG content = 0, ready = 0;
    UINT sync = 0, flags = 0;
};
std::mutex g_flip_lock;
std::vector<Flip> g_flips;
HANDLE g_flip_event = nullptr;      // a frame to flip
HANDLE g_stage_event = nullptr;     // a private texture came free
HANDLE g_flipper = nullptr;
uint64_t g_flip_seq = 0;            // the copying thread's
std::atomic<int> g_precopy{1};       // development (dxgiprecopy): the frame into the back buffer ahead, flip waits
uint64_t g_in_back = 0;             // the flipping thread's: the flip whose frame is in the back buffer already
uint64_t g_in_back_value = 0;       // its back_fence value
uint32_t g_flip_call_us[4096];      // how long Present took on the flipping thread
uint32_t g_flip_calls = 0;
uint32_t g_copy_waits = 0;          // flips that had to wait for their copy at their time

// DxvkHdrSwapChain (dxgipresent_set_hdr_chain): 1 = on a display Windows runs in HDR (or composes in FP16 for Auto
// Color Management) the DXGI swap chain is FP16
// scRGB and our pass writes the SDR picture into it as Windows itself would compose it (sRGB decoded, scaled to the
// SDR brightness setting). Windows composes an 8-bit swap chain on an HDR desktop, or shows it on an overlay plane
// (the logs say "hardware overlay"); a swap chain in the display's own kind of colour can be flipped straight to it,
// which is how variable refresh is meant to see the frames.
std::atomic<int> g_hdr_chain{0};

// Development: scanwatch's flip observer, and the mode mark (until this time).
std::atomic<void (*)(long long)> g_flip_observer{nullptr};
std::atomic<LONGLONG> g_mark_until{0};

// The flipping thread's timing.
double g_latency = 0.0;             // ticks: a frame flips this long after its boundary
double g_latency_target = 0.0;
uint32_t g_late_us[256];            // how long after its boundary each recent steady frame was ready
uint32_t g_late_count = 0, g_late_next = 0;
uint32_t g_steps_us[64];            // the recent steps (for what counts as steady)
uint32_t g_steps_count = 0, g_steps_next = 0;
uint32_t g_step_median_us = 0;
LONGLONG g_late_prev_content = 0;
LONGLONG g_prev_content = 0, g_prev_ready = 0, g_prev_flip = 0;

// Flip spacing and timing since the last report (the presenting thread adds, the report takes).
std::mutex g_spacing_lock;
constexpr uint32_t kSpacingSamples = 4096;
uint32_t g_spacing_us[kSpacingSamples];
uint32_t g_spacing_count = 0;
uint32_t g_spacing_close = 0;       // flips closer together than the display's fastest refresh
uint32_t g_err_flip_us[kSpacingSamples];   // |flip spacing - step|
uint32_t g_err_ready_us[kSpacingSamples];  // |ready spacing - step|: what flipping each frame when ready gives
uint32_t g_step_us[kSpacingSamples];       // the steps
uint32_t g_err_count = 0;
uint64_t g_after_boundary_us = 0;   // flip - boundary, summed
uint32_t g_after_count = 0;
uint32_t g_missed = 0;              // frames ready after their time
uint32_t g_over_us[kSpacingSamples];  // how late the presenting thread woke for a held flip
uint32_t g_overs = 0;
double g_latency_moved_us = 0;      // how far the delay moved (sum of changes)
uint32_t g_ready_timeouts = 0;

LONGLONG qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

uint32_t to_us(LONGLONG ticks) {
    if (ticks <= 0 || g_qpc_freq <= 0) return 0;
    const LONGLONG us = ticks * 1000000 / g_qpc_freq;
    return us > 60000000 ? 60000000u : static_cast<uint32_t>(us);
}

// The presenting thread until `target` (performance counter ticks): a high resolution timer, then spun. The timer
// can wake a few tenths of a millisecond late, so the spun part follows how late it has been: the latest wake seen,
// plus 0.1 ms, easing down by 0.1% a frame (between 0.3 and 2 ms).
double g_spin = 0.0;  // ticks

void pace_until(LONGLONG target) {
    if (!g_pace_timer_tried) {
        g_pace_timer_tried = true;
        g_pace_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */,
                                              SYNCHRONIZE | TIMER_MODIFY_STATE);
    }
    const double lo = 0.3e-3 * g_qpc_freq, hi = 2.0e-3 * g_qpc_freq;
    if (g_spin < lo) g_spin = lo;
    LONGLONG now = qpc_now();
    const LONGLONG spin = static_cast<LONGLONG>(g_spin);
    if (g_pace_timer && now + spin < target) {
        const LONGLONG due_at = target - spin;
        LARGE_INTEGER due{};
        due.QuadPart = -((due_at - now) * 10000000LL / g_qpc_freq);
        if (due.QuadPart < 0 && SetWaitableTimer(g_pace_timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(g_pace_timer, 50);
            now = qpc_now();
            const double want = static_cast<double>(now - due_at) + 0.1e-3 * g_qpc_freq;
            g_spin = (std::max)(g_spin * 0.999, want);
            if (g_spin < lo) g_spin = lo;
            if (g_spin > hi) g_spin = hi;
        }
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

void push_content(LONGLONG t) {
    std::lock_guard<std::mutex> g(g_content_lock);
    if (g_content_count == kContentSlots) {
        g_content_first = (g_content_first + 1) % kContentSlots;
        --g_content_count;
        ++g_content_dropped;
    }
    g_content[(g_content_first + g_content_count) % kContentSlots] = t;
    ++g_content_count;
}

// The boundary of the picture DXVK presents now: the oldest queued. DXVK presents each of the game's Presents once,
// in order, so the queue stays one to three deep (DXVK's threads can be a frame or two behind the game's); if it stays
// four or more deep for a whole second (a present that never came), the oldest goes. A queue one too deep only times
// the flips from the frame before (the spacing stays right; the delay grows by a frame).
LONGLONG take_content() {
    std::lock_guard<std::mutex> g(g_content_lock);
    if (!g_content_count) {
        ++g_content_missing;
        return 0;
    }
    if (g_content_count >= 4) {
        if (++g_content_backlog >= 240) {
            g_content_first = (g_content_first + 1) % kContentSlots;
            --g_content_count;
            ++g_content_dropped;
            g_content_backlog = 0;
        }
    } else {
        g_content_backlog = 0;
    }
    g_content_depth_sum += g_content_count;
    ++g_content_takes;
    const LONGLONG t = g_content[g_content_first];
    g_content_first = (g_content_first + 1) % kContentSlots;
    --g_content_count;
    return t;
}

// When to flip a frame whose boundary was `content` and which was ready at `ready_t` (flipping thread). The delay
// learns only from steady frames: a step more than 2.5 times the recent median (or over 50 ms: loading screens, hitches)
// says nothing about how long a frame takes, and once let in it held every later frame back. It stays under three
// median steps (40 ms at most).
LONGLONG flip_target(LONGLONG content, LONGLONG ready_t, int mode) {
    LONGLONG target = ready_t;
    if (mode != 1) return target;
    if (content > 0) {
        const uint32_t step = g_late_prev_content > 0 ? to_us(content - g_late_prev_content) : 0;
        g_late_prev_content = content;
        if (step > 0) {
            g_steps_us[g_steps_next] = step;
            g_steps_next = (g_steps_next + 1) & 63;
            if (g_steps_count < 64) ++g_steps_count;
            if ((g_steps_next & 7) == 0 || g_steps_count < 16) {
                uint32_t tmp[64];
                std::memcpy(tmp, g_steps_us, g_steps_count * sizeof(uint32_t));
                std::nth_element(tmp, tmp + g_steps_count / 2, tmp + g_steps_count);
                g_step_median_us = tmp[g_steps_count / 2];
            }
        }
        const bool steady = step > 0 && step <= 50000 && (!g_step_median_us || step * 2 <= g_step_median_us * 5);
        if (steady) {
            // a frame held up on its way (a hitch after its boundary) counts as three steps at most
            uint32_t late = to_us(ready_t - content);
            if (g_step_median_us && late > 3 * g_step_median_us) late = 3 * g_step_median_us;
            g_late_us[g_late_next] = late;
            g_late_next = (g_late_next + 1) & 255;
            if (g_late_count < 256) ++g_late_count;
            if ((g_late_next & 7) == 0 || g_late_count < 32) {
                uint32_t tmp[256];
                std::memcpy(tmp, g_late_us, g_late_count * sizeof(uint32_t));
                const uint32_t k = g_late_count * 99 / 100;
                std::nth_element(tmp, tmp + k, tmp + g_late_count);
                double cap_us = 3.0 * g_step_median_us + 250.0;
                if (cap_us > 40000.0 || !g_step_median_us) cap_us = 40000.0;
                g_latency_target = ((std::min)(tmp[k] + 250.0, cap_us)) * g_qpc_freq / 1e6;
            }
        }
        const double up = 0.25e-3 * g_qpc_freq, down = 0.01e-3 * g_qpc_freq;
        if (g_latency_target > g_latency) {
            g_latency = (std::min)(g_latency_target, g_latency + up);
        } else {
            // falls quickly when far above (a loading screen's left-over), gently near
            const double fall = (g_latency - g_latency_target > 2e-3 * g_qpc_freq) ? 0.25e-3 * g_qpc_freq : down;
            g_latency = (std::max)(g_latency_target, g_latency - fall);
        }
        const LONGLONG at = content + static_cast<LONGLONG>(g_latency);
        if (at > target) {
            target = at;
        } else {
            std::lock_guard<std::mutex> g(g_spacing_lock);
            ++g_missed;
        }
    }
    if (g_last_flip) {
        const int cap = g_cap_fps ? g_cap_fps() : 0;
        const int mhz = g_refresh_mhz.load();
        if (cap > 0 && mhz > 0 && cap * 1000 <= mhz) {
            const LONGLONG soonest = g_last_flip + g_qpc_freq * 1000 / mhz;
            if (soonest > target) target = soonest;
        }
    }
    return target;
}

// A flip asked for at `flip_t`: its spacing from the one before, and against the game's steps.
void note_flip(LONGLONG flip_t, LONGLONG content, LONGLONG ready_t) {
    if (g_qpc_freq <= 0) return;
    const int mhz = g_refresh_mhz.load();
    std::lock_guard<std::mutex> g(g_spacing_lock);
    if (g_last_flip) {
        const uint32_t us = to_us(flip_t - g_last_flip);
        if (g_spacing_count < kSpacingSamples) g_spacing_us[g_spacing_count++] = us;
        if (mhz > 0 && us * static_cast<uint64_t>(mhz) < 1000000000ull) ++g_spacing_close;
    }
    if (content > 0 && g_prev_content > 0 && ready_t > 0 && g_prev_ready > 0 && g_err_count < kSpacingSamples) {
        const LONGLONG step = content - g_prev_content;
        const LONGLONG flip_step = flip_t - g_prev_flip, ready_step = ready_t - g_prev_ready;
        g_step_us[g_err_count] = to_us(step);
        g_err_flip_us[g_err_count] = to_us(flip_step > step ? flip_step - step : step - flip_step);
        g_err_ready_us[g_err_count] = to_us(ready_step > step ? ready_step - step : step - ready_step);
        ++g_err_count;
    }
    if (content > 0) {
        g_after_boundary_us += to_us(flip_t - content);
        ++g_after_count;
    }
    g_prev_content = content;
    g_prev_ready = ready_t;
    g_prev_flip = flip_t;
    g_last_flip = flip_t;
    if (auto observer = g_flip_observer.load()) observer(flip_t);
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
        release(d->scrgb_vs);
        release(d->scrgb_ps);
        release(d->scrgb_cb);
        release(d->back_fence);
        if (d->back_event) CloseHandle(d->back_event);
        d->back_event = nullptr;
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
    drain_jobs();
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
        release(im.srv);
        release(im.tex);
    }
    for (uint32_t k = 0; k < Swap::kStages; ++k) {
        release(s->stage_srv[k]);
        release(s->stage[k]);
        s->stage_busy[k] = false;
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
        const HRESULT hr = w->chain->ResizeBuffers(0, s->extent.width, s->extent.height,
                                                   w->scrgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : fmt, w->flags);
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
    drain_jobs();
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
    if (s) drain_jobs();
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
        const uint64_t last = d->release_value.load();
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

// ---------------------------------------------------------------------------------------------------------------
// The scRGB swap chain (DxvkHdrSwapChain)

bool make_scrgb_shaders(Dev* d) {
    if (d->scrgb_ps) return true;
    if (d->scrgb_failed) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(d->d3d->CreateVertexShader(kDxgiScrgbVs, sizeof(kDxgiScrgbVs), nullptr, &d->scrgb_vs)) ||
        FAILED(d->d3d->CreatePixelShader(kDxgiScrgbPs, sizeof(kDxgiScrgbPs), nullptr, &d->scrgb_ps)) ||
        FAILED(d->d3d->CreateBuffer(&bd, nullptr, &d->scrgb_cb))) {
        release(d->scrgb_vs);
        release(d->scrgb_ps);
        release(d->scrgb_cb);
        d->scrgb_failed = true;
        LOG_ERROR("DxvkHdrSwapChain: the scRGB pass could not be made; the swap chain stays 8-bit");
        return false;
    }
    return true;
}

// Before a frame goes into the window's swap chain, outside g_d3d: with DxvkHdrSwapChain on, the display's colour mode
// and SDR brightness every 2 s (QueryDisplayConfig is slow and takes a lock the whole system shares).
void look_at_display(Window* w) {
    if (!g_hdr_chain.load()) return;
    const LONGLONG now = qpc_now();
    if (w->looked && now - w->looked < 2 * g_qpc_freq) return;
    w->looked = now;
    {
        float white = 1.0f;
        const int mode = display_hdr_white(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTOPRIMARY), &white);
        const bool hdr = mode == 1 || mode == 2;  // both composed in FP16 by Windows
        if (hdr != w->display_hdr || white != w->white) {
            w->display_hdr = hdr;
            w->white = white;
            if (g_hdr_chain.load()) {
                static const char* const kModes[] = {"in standard colour", "in HDR",
                                                     "in advanced colour without HDR (Auto Color Management)"};
                LOG_INFO("DxvkHdrSwapChain: the display is %s, SDR white %.0f nits",
                         kModes[mode >= 0 && mode <= 2 ? mode : 0], white * 80.0);
            }
        }
    }
}

// Under g_d3d, before a frame goes into the window's swap chain: its format for the display now.
void fit_format(Dev* d, Window* w) {
    const bool want = g_hdr_chain.load() == 1 && w->display_hdr && make_scrgb_shaders(d);
    if (want == w->scrgb) return;
    d->ctx->ClearState();
    d->ctx->Flush();
    const DXGI_FORMAT f = want ? DXGI_FORMAT_R16G16B16A16_FLOAT : w->format;
    HRESULT hr = w->chain->ResizeBuffers(0, w->width, w->height, f, w->flags);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_ERROR("DxvkHdrSwapChain: the swap chain could not change format (%08lX)", hr);
        }
        return;
    }
    IDXGISwapChain3* c3 = nullptr;
    if (SUCCEEDED(w->chain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&c3)))) {
        hr = c3->SetColorSpace1(want ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        c3->Release();
    }
    w->scrgb = want;
    LOG_INFO("DxvkHdrSwapChain: the DXGI swap chain is now %s (colour space %08lX)",
             want ? "FP16 scRGB, the SDR picture converted as Windows composes it" : "8-bit SDR", hr);
}

// Under g_d3d: the mode mark (dxgipresent_show_mark) in the top right corner of the back buffer.
void draw_mark(Dev* d, Window* w, ID3D11Texture2D* back) {
    const LONGLONG until = g_mark_until.load();
    if (!until || qpc_now() > until) return;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(d->d3d->CreateRenderTargetView(back, nullptr, &rtv)) || !rtv) return;
    const float k = w->scrgb ? w->white : 1.0f;
    const int pacing = g_pacing.load();
    const float colors[3][4] = {{k, 0, 0, 1}, {0, k, 0, 1}, {k, k, 0, 1}};
    const float* c = colors[pacing >= 0 && pacing <= 2 ? pacing : 0];
    const float chain[4] = {w->scrgb ? 0.0f : 0.5f * k, w->scrgb ? 0.3f * k : 0.5f * k, w->scrgb ? k : 0.5f * k, 1};
    const LONG size = static_cast<LONG>(w->height / 24 + 8);
    const D3D11_RECT r1{static_cast<LONG>(w->width) - 2 * size - 16, 16, static_cast<LONG>(w->width) - size - 16,
                        16 + size};
    const D3D11_RECT r2{static_cast<LONG>(w->width) - size - 8, 16, static_cast<LONG>(w->width) - 8, 16 + size};
    d->ctx->ClearView(rtv, c, &r1, 1);
    d->ctx->ClearView(rtv, chain, &r2, 1);
    rtv->Release();
}

// Under g_d3d: the frame (`src`, its view made in `*srv` when needed) into the back buffer.
void copy_out(Dev* d, Window* w, ID3D11Texture2D* src, ID3D11ShaderResourceView** srv, ID3D11Texture2D* back) {
    if (!w->scrgb) {
        d->ctx->CopyResource(back, src);
        draw_mark(d, w, back);
        return;
    }
    if (!*srv) d->d3d->CreateShaderResourceView(src, nullptr, srv);
    ID3D11RenderTargetView* rtv = nullptr;
    d->d3d->CreateRenderTargetView(back, nullptr, &rtv);
    if (!*srv || !rtv) {
        release(rtv);
        return;
    }
    if (d->scrgb_cb_white != w->white) {
        const float c[4] = {w->white, 0.0f, 0.0f, 0.0f};
        d->ctx->UpdateSubresource(d->scrgb_cb, 0, nullptr, c, 0, 0);
        d->scrgb_cb_white = w->white;
    }
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w->width), static_cast<float>(w->height), 0.0f, 1.0f};
    d->ctx->OMSetRenderTargets(1, &rtv, nullptr);
    d->ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    d->ctx->OMSetDepthStencilState(nullptr, 0);
    d->ctx->RSSetState(nullptr);
    d->ctx->RSSetViewports(1, &vp);
    d->ctx->IASetInputLayout(nullptr);
    d->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->VSSetShader(d->scrgb_vs, nullptr, 0);
    d->ctx->PSSetShader(d->scrgb_ps, nullptr, 0);
    d->ctx->PSSetShaderResources(0, 1, srv);
    d->ctx->PSSetConstantBuffers(0, 1, &d->scrgb_cb);
    d->ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    d->ctx->PSSetShaderResources(0, 1, &none);
    d->ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv->Release();
    draw_mark(d, w, back);
}

// ---------------------------------------------------------------------------------------------------------------
// The presenting thread (modes 1 and 2)

// Every queued frame presented (before a swap chain or the device goes, and before mode 0 presents again: the
// copy-done values must be signalled in order).
void drain_jobs() {
    for (int i = 0; g_jobs_open.load() > 0 && i < 4000; ++i) Sleep(1);
}

void sample_composition(IDXGISwapChain1* chain) {
    // How Windows shows the frames (composed by the desktop window manager, or flipped straight to the display:
    // what variable refresh needs), sampled now and then.
    if ((g_presents.load() & 255) != 255) return;
    IDXGISwapChainMedia* media = nullptr;
    if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChainMedia), reinterpret_cast<void**>(&media)))) {
        DXGI_FRAME_STATISTICS_MEDIA st{};
        if (SUCCEEDED(media->GetFrameStatisticsMedia(&st))) g_composition = static_cast<int>(st.CompositionMode);
        media->Release();
    }
}

void note_present_result(HRESULT hr) {
    g_last_hr = hr;
    if (FAILED(hr) && !g_logged_present_error) {
        g_logged_present_error = true;
        LOG_ERROR("DXGI present: Present failed (%08lX)", hr);
    }
}

// A free private texture of the swap chain (the copying thread waits for one if the flips are behind), made on
// first use in the frames' format.
bool take_stage(Swap* s, uint32_t* out) {
    for (int tries = 0; tries < 500; ++tries) {
        for (uint32_t k = 0; k < Swap::kStages; ++k) {
            bool expected = false;
            if (s->stage_busy[k].compare_exchange_strong(expected, true)) {
                if (!s->stage[k]) {
                    D3D11_TEXTURE2D_DESC td{};
                    s->images[0].tex->GetDesc(&td);
                    td.MiscFlags = 0;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    std::lock_guard<std::mutex> g(g_d3d);
                    if (FAILED(s->dev->d3d->CreateTexture2D(&td, nullptr, &s->stage[k]))) {
                        s->stage_busy[k] = false;
                        return false;
                    }
                }
                *out = k;
                return true;
            }
        }
        WaitForSingleObject(g_stage_event, 2);
    }
    return false;
}

// The copying thread: each frame, in order, as soon as Vulkan has finished it: into a private texture, the image
// handed back to DXVK (copy-done), then on to the flipping thread. Nothing here waits for a flip time.
void copy_job(const Job& j) {
    Swap* s = j.swap;
    Dev* d = s->dev;
    Image& im = s->images[j.index < s->count ? j.index : 0];
    // Vulkan's frame, on the CPU (a device that stops answering must not hang the game: after half a second the
    // GPU-side wait below still holds the copy back)
    if (d->ready_fence->GetCompletedValue() < j.ready) {
        if (!d->ready_event) d->ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool ok = d->ready_event && SUCCEEDED(d->ready_fence->SetEventOnCompletion(j.ready, d->ready_event)) &&
                        WaitForSingleObject(d->ready_event, 500) == WAIT_OBJECT_0;
        if (!ok) {
            std::lock_guard<std::mutex> g(g_spacing_lock);
            ++g_ready_timeouts;
        }
    }
    Flip f;
    f.seq = ++g_flip_seq;
    f.swap = s;
    f.content = j.content;
    f.ready = qpc_now();
    f.sync = j.sync;
    f.flags = j.flags;
    const bool staged = take_stage(s, &f.stage);
    {
        std::lock_guard<std::mutex> g(g_d3d);
        d->ctx->Wait(d->ready_fence, j.ready);
        if (staged) {
            d->ctx->CopyResource(s->stage[f.stage], im.tex);
        } else {
            // no private texture: straight into the back buffer and flipped at once (as mode 2)
            g_in_back = 0;
            look_at_display(s->window);
            fit_format(d, s->window);
            ID3D11Texture2D* back = nullptr;
            HRESULT hr = s->window->chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
            if (SUCCEEDED(hr)) {
                copy_out(d, s->window, im.tex, &im.srv, back);
                back->Release();
            }
            d->ctx->Signal(d->release_fence, j.done);
            note_flip(qpc_now(), j.content, f.ready);
            if (SUCCEEDED(hr)) hr = s->window->chain->Present(j.sync, j.flags);
            sample_composition(s->window->chain);
            note_present_result(hr);
            g_jobs_open.fetch_sub(1);
            return;
        }
        d->ctx->Signal(d->release_fence, j.done);
        d->ctx->Flush();
    }
    {
        std::lock_guard<std::mutex> g(g_flip_lock);
        g_flips.push_back(f);
    }
    SetEvent(g_flip_event);
}

// Under g_d3d: a copied frame into the back buffer ahead of its time, with a fence value for when the GPU is done.
void put_in_back_buffer(const Flip& f) {
    Swap* s = f.swap;
    Dev* d = s->dev;
    fit_format(d, s->window);
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(s->window->chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
        copy_out(d, s->window, s->stage[f.stage], &s->stage_srv[f.stage], back);
        back->Release();
    }
    if (!d->back_fence) d->d3d->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
                                            reinterpret_cast<void**>(&d->back_fence));
    if (d->back_fence) d->ctx->Signal(d->back_fence, ++d->back_value);
    d->ctx->Flush();
    g_in_back = f.seq;
    g_in_back_value = d->back_value;
}

// The flipping thread: each copied frame at its time. Normally the next frame or two are already copied while one is
// held (the delay is about a frame or more); with three waiting the flips have fallen behind, and it catches up
// without holding. The frame goes into the back buffer as soon as the flip before it is done, and its time waits for
// that copy to be finished, so the flip is the only thing Present asks of the GPU: copied at flip time, it waited for
// the GPU to get round to the copy between DXVK's work (the 2026-11-14 log at a 120 cap: scanout 0.5 to 2 ms after
// the Present call).
void flip_one(const Flip& f, bool behind) {
    Swap* s = f.swap;
    Dev* d = s->dev;
    look_at_display(s->window);
    const bool precopy = g_precopy.load() != 0;
    if (precopy) {
        std::lock_guard<std::mutex> g(g_d3d);
        if (g_in_back != f.seq) put_in_back_buffer(f);
    }
    const double latency_before = g_latency;
    LONGLONG target = flip_target(f.content, f.ready, g_pacing.load());
    if (behind) target = 0;
    const bool held = target > qpc_now();
    if (held) pace_until(target);
    if (!precopy) {
        std::lock_guard<std::mutex> g(g_d3d);
        put_in_back_buffer(f);  // copied now, just before its flip (the GPU does both in turn)
    } else if (d->back_fence && d->back_fence->GetCompletedValue() < g_in_back_value) {
        if (!d->back_event) d->back_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (d->back_event && SUCCEEDED(d->back_fence->SetEventOnCompletion(g_in_back_value, d->back_event))) {
            WaitForSingleObject(d->back_event, 100);
        }
        std::lock_guard<std::mutex> g(g_spacing_lock);
        ++g_copy_waits;
    }
    HRESULT hr = S_OK;
    {
        std::lock_guard<std::mutex> g(g_d3d);
        const LONGLONG flip_t = qpc_now();
        note_flip(flip_t, f.content, f.ready);
        hr = s->window->chain->Present(f.sync, f.flags);
        const LONGLONG after = qpc_now();
        sample_composition(s->window->chain);
        std::lock_guard<std::mutex> sg(g_spacing_lock);
        if (held && g_overs < kSpacingSamples) g_over_us[g_overs++] = to_us(flip_t - target);
        g_latency_moved_us += std::fabs(g_latency - latency_before) * 1e6 / g_qpc_freq;
        if (g_flip_calls < 4096) g_flip_call_us[g_flip_calls++] = to_us(after - flip_t);
    }
    s->stage_busy[f.stage] = false;
    SetEvent(g_stage_event);
    note_present_result(hr);
    // the next frame into the back buffer now, if it is here
    Flip next;
    bool have = false;
    {
        std::lock_guard<std::mutex> g(g_flip_lock);
        if (!g_flips.empty()) {
            next = g_flips.front();
            have = true;
        }
    }
    if (have && precopy && next.swap == s) {
        std::lock_guard<std::mutex> g(g_d3d);
        put_in_back_buffer(next);
    }
}

DWORD WINAPI flipper_main(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        WaitForSingleObject(g_flip_event, INFINITE);
        for (;;) {
            Flip f;
            bool more = false;
            {
                std::lock_guard<std::mutex> g(g_flip_lock);
                if (g_flips.empty()) break;
                f = g_flips.front();
                g_flips.erase(g_flips.begin());
                more = g_flips.size() >= 3;
            }
            flip_one(f, more);
            g_jobs_open.fetch_sub(1);
        }
    }
}

DWORD WINAPI presenter_main(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        WaitForSingleObject(g_job_event, INFINITE);
        for (;;) {
            Job j;
            {
                std::lock_guard<std::mutex> g(g_job_lock);
                if (g_jobs.empty()) break;
                j = g_jobs.front();
                g_jobs.erase(g_jobs.begin());
            }
            copy_job(j);
        }
    }
}

// False when the threads cannot be had (the caller presents the old way).
bool start_presenter() {
    if (g_presenter && g_flipper) return true;
    static bool failed = false;
    if (failed) return false;
    g_job_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_flip_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stage_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_job_event && g_flip_event && g_stage_event) {
        g_flipper = CreateThread(nullptr, 0, flipper_main, nullptr, 0, nullptr);
        g_presenter = g_flipper ? CreateThread(nullptr, 0, presenter_main, nullptr, 0, nullptr) : nullptr;
    }
    if (!g_presenter || !g_flipper) {
        failed = true;
        LOG_ERROR("DXGI present: no presenting threads (%lu); frames flip when the GPU is done", GetLastError());
        return false;
    }
    return true;
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

    g_last_hwnd = s->window->hwnd;
    if ((g_presents.load() & 255) == 0) sample_refresh(s->window->hwnd);
    const LONGLONG content = take_content();
    UINT sync = 1, flags = 0;
    if (s->mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
        sync = 0;
        flags = d->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    } else if (s->mode == VK_PRESENT_MODE_MAILBOX_KHR) {
        sync = 0;
    }

    if (g_pacing.load() != 0 && start_presenter()) {
        // The presenting thread copies and flips it; the image's next acquire waits for this copy-done value.
        Job j;
        j.swap = s;
        j.index = i < s->count ? i : 0;
        j.ready = ready;
        j.done = d->release_value.fetch_add(1) + 1;
        j.content = content;
        j.sync = sync;
        j.flags = flags;
        im.released = j.done;
        {
            std::lock_guard<std::mutex> g(g_job_lock);
            g_jobs.push_back(j);
        }
        g_jobs_open.fetch_add(1);
        SetEvent(g_job_event);
    } else {
        drain_jobs();
        look_at_display(s->window);
        // Direct3D 11: wait for it on the GPU, copy into the DXGI back buffer, signal copy-done, present.
        HRESULT hr;
        std::lock_guard<std::mutex> g(g_d3d);
        d->ctx->Wait(d->ready_fence, ready);
        fit_format(d, s->window);
        ID3D11Texture2D* back = nullptr;
        hr = s->window->chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
        if (SUCCEEDED(hr)) {
            copy_out(d, s->window, im.tex, &im.srv, back);
            back->Release();
        }
        const uint64_t done = d->release_value.fetch_add(1) + 1;
        d->ctx->Signal(d->release_fence, done);
        im.released = done;
        const LONGLONG now = qpc_now();
        note_flip(now, content, now);
        if (SUCCEEDED(hr)) hr = s->window->chain->Present(sync, flags);
        sample_composition(s->window->chain);
        note_present_result(hr);
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
    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f)) g_qpc_freq = f.QuadPart;
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

    static uint32_t sorted[kSpacingSamples], step[kSpacingSamples], err_flip[kSpacingSamples],
        err_ready[kSpacingSamples];
    uint32_t count, close_count, errs, after_n, missed, timeouts, missing, dropped, depth_sum, takes;
    uint64_t after_us;
    {
        std::lock_guard<std::mutex> g(g_spacing_lock);
        count = g_spacing_count;
        std::memcpy(sorted, g_spacing_us, count * sizeof(uint32_t));
        close_count = g_spacing_close;
        errs = g_err_count;
        std::memcpy(step, g_step_us, errs * sizeof(uint32_t));
        std::memcpy(err_flip, g_err_flip_us, errs * sizeof(uint32_t));
        std::memcpy(err_ready, g_err_ready_us, errs * sizeof(uint32_t));
        after_us = g_after_boundary_us;
        after_n = g_after_count;
        missed = g_missed;
        timeouts = g_ready_timeouts;
        g_spacing_count = g_spacing_close = g_err_count = g_after_count = g_missed = g_ready_timeouts = 0;
        g_after_boundary_us = 0;
        if (g_flip_calls >= 8) {
            std::sort(g_flip_call_us, g_flip_call_us + g_flip_calls);
            LOG_INFO("DXGI present: Present on the flipping thread took half %.3f, 99%% %.3f, most %.2f ms; %u flips "
                     "waited at their time for their copy", g_flip_call_us[g_flip_calls / 2] / 1000.0,
                     g_flip_call_us[g_flip_calls * 99 / 100] / 1000.0, g_flip_call_us[g_flip_calls - 1] / 1000.0,
                     g_copy_waits);
        }
        g_flip_calls = 0;
        g_copy_waits = 0;
        if (g_overs >= 8) {
            std::sort(g_over_us, g_over_us + g_overs);
            LOG_INFO("DXGI present: held flips went out late by half %.3f, 90%% %.3f, 99%% %.3f ms (%u held, the "
                     "last %.2f ms spun); the delay moved %.2f ms in all, now %.2f ms", g_over_us[g_overs / 2] / 1000.0,
                     g_over_us[g_overs * 9 / 10] / 1000.0, g_over_us[g_overs * 99 / 100] / 1000.0, g_overs,
                     g_spin * 1000.0 / g_qpc_freq, g_latency_moved_us / 1000.0, g_latency * 1000.0 / g_qpc_freq);
        }
        g_overs = 0;
        g_latency_moved_us = 0;
    }
    {
        std::lock_guard<std::mutex> g(g_content_lock);
        missing = g_content_missing;
        dropped = g_content_dropped;
        depth_sum = g_content_depth_sum;
        takes = g_content_takes;
        g_content_missing = g_content_dropped = g_content_depth_sum = g_content_takes = 0;
    }
    const int pacing = g_pacing.load();
    if (count >= 8) {
        std::sort(sorted, sorted + count);
        uint64_t sum = 0;
        for (uint32_t k = 0; k < count; ++k) sum += sorted[k];
        const int mhz = g_refresh_mhz.load();
        static const char* const kModes[] = {"flipped by the GPU when done (DxvkPacedPresent off)",
                                             "timed to the game's steps (DxvkPacedPresent)",
                                             "flipped as soon as ready (dxgipace 2)"};
        LOG_INFO("DXGI present: flips %.2f ms apart on average (1%% %.2f, 10%% %.2f, half %.2f, 90%% %.2f, 99%% %.2f, "
                 "longest %.2f ms); %u of %u closer than the display's fastest refresh (%.2f ms at %.0f Hz); %s",
                 sum / 1000.0 / count, sorted[count / 100] / 1000.0, sorted[count / 10] / 1000.0,
                 sorted[count / 2] / 1000.0, sorted[count * 9 / 10] / 1000.0, sorted[count * 99 / 100] / 1000.0,
                 sorted[count - 1] / 1000.0, close_count, count, mhz > 0 ? 1000000.0 / mhz : 0.0, mhz / 1000.0,
                 kModes[pacing >= 0 && pacing <= 2 ? pacing : 0]);
    }
    if (errs >= 8) {
        // How far each flip's spacing is from the game's step for that picture (what judders), and what flipping
        // each frame the moment it is ready would have given (in mode 0 the flip times are when the frames were
        // handed to Direct3D 11, which flips them later, when the GPU is done).
        std::sort(step, step + errs);
        std::sort(err_flip, err_flip + errs);
        std::sort(err_ready, err_ready + errs);
        char timing[200] = "";
        if (pacing == 1) {
            std::snprintf(timing, sizeof(timing), "; flips %.2f ms after their frame's boundary on average, %u of %u "
                          "frames ready after their time%s", after_n ? after_us / 1000.0 / after_n : 0.0, missed, errs,
                          timeouts ? ", SOME WAITS FOR A FRAME TIMED OUT" : "");
        }
        LOG_INFO("DXGI present: the game's steps half %.2f ms (1%% %.2f, 99%% %.2f); flip spacing off the steps by half "
                 "%.3f, 90%% %.3f, 99%% %.3f, most %.2f ms; flipping when ready would be off by half %.3f, 90%% %.3f, "
                 "99%% %.3f, most %.2f ms%s%s", step[errs / 2] / 1000.0, step[errs / 100] / 1000.0,
                 step[errs * 99 / 100] / 1000.0, err_flip[errs / 2] / 1000.0, err_flip[errs * 9 / 10] / 1000.0,
                 err_flip[errs * 99 / 100] / 1000.0, err_flip[errs - 1] / 1000.0, err_ready[errs / 2] / 1000.0,
                 err_ready[errs * 9 / 10] / 1000.0, err_ready[errs * 99 / 100] / 1000.0, err_ready[errs - 1] / 1000.0,
                 timing, "");
        LOG_INFO("DXGI present: frame boundaries queued %.2f deep on average when DXVK presented; %u presents found "
                 "none, %u dropped to get back in step", takes ? static_cast<double>(depth_sum) / takes : 0.0, missing,
                 dropped);
    }
    char nv[200];
    if (nvidia_vrr(nv, sizeof(nv))) LOG_INFO("DXGI present: %s", nv);
}

void dxgipresent_set_pacing(int mode, int (*cap_fps)()) {
    if (cap_fps) g_cap_fps = cap_fps;
    g_pacing = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

void dxgipresent_set_flip_observer(void (*observer)(long long qpc)) {
    g_flip_observer = observer;
}

void dxgipresent_show_mark(double seconds) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    g_mark_until = t.QuadPart + static_cast<LONGLONG>(seconds * f.QuadPart);
}

void dxgipresent_set_precopy(int on) {
    g_precopy = on ? 1 : 0;
}

void dxgipresent_set_hdr_chain(int on) {
    g_hdr_chain = on ? 1 : 0;
}

int dxgipresent_hdr_chain() {
    return g_hdr_chain.load();
}

void dxgipresent_frame_presented(long long boundary_qpc) {

    push_content(boundary_qpc > 0 ? boundary_qpc : qpc_now());
}

bool dxgipresent_in_use() {
    std::lock_guard<std::mutex> g(g_lock);
    for (const Dev* d : g_devs) {
        if (d->dxgi) return true;
    }
    return false;
}

int dxgipresent_pacing() {
    return g_pacing.load();
}

void dxgipresent_test_refresh(int hz) {
    g_test_refresh_mhz = hz > 0 ? hz * 1000 : 0;
    if (hz > 0) g_refresh_mhz = hz * 1000;
}
