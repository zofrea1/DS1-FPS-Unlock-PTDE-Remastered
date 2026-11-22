#include "vkprobe.h"

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

const char* format_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16F";
    default: return nullptr;
    }
}

}  // namespace

int vkprobe_10bit_surface(HWND window, char* text, size_t size) {
    if (text && size) text[0] = 0;
    HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib) {
        if (text) std::snprintf(text, size, "no vulkan-1.dll");
        return -1;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
    auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
    if (!create) {
        if (text) std::snprintf(text, size, "no vkCreateInstance");
        FreeLibrary(lib);
        return -1;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PTDE-FPS-Unlock surface probe";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = ext;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = create(&ici, nullptr, &inst);
    if (vr != VK_SUCCESS || !inst) {
        if (text) std::snprintf(text, size, "vkCreateInstance %d", vr);
        FreeLibrary(lib);
        return -1;
    }
#define VKFN(name) auto name = reinterpret_cast<PFN_##name>(gipa(inst, #name))
    VKFN(vkDestroyInstance);
    VKFN(vkEnumeratePhysicalDevices);
    VKFN(vkCreateWin32SurfaceKHR);
    VKFN(vkDestroySurfaceKHR);
    VKFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VKFN(vkGetPhysicalDeviceSurfaceSupportKHR);
#undef VKFN
    int result = -1;
    if (vkDestroyInstance && vkEnumeratePhysicalDevices && vkCreateWin32SurfaceKHR && vkDestroySurfaceKHR &&
        vkGetPhysicalDeviceSurfaceFormatsKHR) {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> pds(n);
        if (n) vkEnumeratePhysicalDevices(inst, &n, pds.data());
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = window;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        vr = n ? vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surface) : VK_ERROR_INITIALIZATION_FAILED;
        if (vr == VK_SUCCESS && surface) {
            // The first device that can present to the window (the one DXVK takes on a single-GPU machine).
            for (VkPhysicalDevice pd : pds) {
                VkBool32 ok = VK_TRUE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR) vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surface, &ok);
                if (!ok) continue;
                uint32_t fc = 0;
                if (vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fc, nullptr) != VK_SUCCESS || !fc) continue;
                std::vector<VkSurfaceFormatKHR> fmts(fc);
                if (vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fc, fmts.data()) != VK_SUCCESS) continue;
                result = 0;
                size_t used = 0;
                for (const VkSurfaceFormatKHR& f : fmts) {
                    const bool srgb = f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                    if (srgb && (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                                 f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) {
                        result = 1;
                    }
                    if (text && used + 40 < size) {
                        const char* name = format_name(f.format);
                        char unknown[16];
                        if (!name) {
                            std::snprintf(unknown, sizeof(unknown), "fmt %d", static_cast<int>(f.format));
                            name = unknown;
                        }
                        used += std::snprintf(text + used, size - used, "%s%s%s", used ? ", " : "", name,
                                              srgb ? "" : " (other colour space)");
                    }
                }
                break;
            }
            vkDestroySurfaceKHR(inst, surface, nullptr);
        } else if (text) {
            std::snprintf(text, size, "vkCreateWin32SurfaceKHR %d", vr);
        }
    }
    if (vkDestroyInstance) vkDestroyInstance(inst, nullptr);
    FreeLibrary(lib);
    return result;
}
