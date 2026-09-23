// Definitions for the entry points declared in ffx_vk_loader.h.
//
// ffx_vk_loader.h is force-included into every source of the shared Vulkan
// module, so including it here also supplies VK_NO_PROTOTYPES and the
// declarations these definitions have to match.

#include "ffx_vk_loader.h"

#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
PFN_vkCreateBuffer                       vkCreateBuffer                       = nullptr;
PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkGetDeviceProcAddr                  vkGetDeviceProcAddr                  = nullptr;
PFN_vkGetPhysicalDeviceFeatures          vkGetPhysicalDeviceFeatures          = nullptr;
PFN_vkGetPhysicalDeviceFeatures2         vkGetPhysicalDeviceFeatures2         = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties  vkGetPhysicalDeviceMemoryProperties  = nullptr;
PFN_vkGetPhysicalDeviceProperties        vkGetPhysicalDeviceProperties        = nullptr;
PFN_vkGetPhysicalDeviceProperties2       vkGetPhysicalDeviceProperties2       = nullptr;
}

namespace {

// The loader is never unloaded. These pointers stay live for the life of the
// module, and releasing it while the SDK still holds Vulkan objects would be
// worse than leaking one handle.
#if defined(_WIN32)
using ModuleHandle = HMODULE;
constexpr const char *kVulkanLoaderName = "vulkan-1.dll";

ModuleHandle OpenLoader(const char *name)
{
    return ::LoadLibraryA(name);
}

void *ResolveSymbol(ModuleHandle module, const char *name)
{
    return reinterpret_cast<void *>(::GetProcAddress(module, name));
}
#else
using ModuleHandle = void *;
// libvulkan.so.1 is the versioned SONAME every loader ships; the unversioned
// libvulkan.so only exists when a development package is installed.
constexpr const char *kVulkanLoaderName = "libvulkan.so.1";

ModuleHandle OpenLoader(const char *name)
{
    return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
}

void *ResolveSymbol(ModuleHandle module, const char *name)
{
    return ::dlsym(module, name);
}
#endif

bool LoadEntryPoints()
{
    const ModuleHandle loader = OpenLoader(kVulkanLoaderName);
    if (loader == nullptr)
        return false;

    // Resolve into locals first so a partial failure cannot leave some globals
    // populated and others null; the SDK has no way to recheck them.
    const struct
    {
        const char *name;
        void      **target;
    } entryPoints[] = {
        {"vkCreateBuffer", reinterpret_cast<void **>(&vkCreateBuffer)},
        {"vkEnumerateDeviceExtensionProperties", reinterpret_cast<void **>(&vkEnumerateDeviceExtensionProperties)},
        {"vkGetDeviceProcAddr", reinterpret_cast<void **>(&vkGetDeviceProcAddr)},
        {"vkGetPhysicalDeviceFeatures", reinterpret_cast<void **>(&vkGetPhysicalDeviceFeatures)},
        {"vkGetPhysicalDeviceFeatures2", reinterpret_cast<void **>(&vkGetPhysicalDeviceFeatures2)},
        {"vkGetPhysicalDeviceMemoryProperties", reinterpret_cast<void **>(&vkGetPhysicalDeviceMemoryProperties)},
        {"vkGetPhysicalDeviceProperties", reinterpret_cast<void **>(&vkGetPhysicalDeviceProperties)},
        {"vkGetPhysicalDeviceProperties2", reinterpret_cast<void **>(&vkGetPhysicalDeviceProperties2)},
    };

    void *resolved[sizeof(entryPoints) / sizeof(entryPoints[0])] = {};
    for (size_t i = 0; i < sizeof(entryPoints) / sizeof(entryPoints[0]); ++i)
    {
        resolved[i] = ResolveSymbol(loader, entryPoints[i].name);
        if (resolved[i] == nullptr)
            return false;
    }

    for (size_t i = 0; i < sizeof(entryPoints) / sizeof(entryPoints[0]); ++i)
        *entryPoints[i].target = resolved[i];

    return true;
}

} // namespace

extern "C" bool ffxVkLoaderEnsureInitialized(void)
{
    static std::once_flag onceFlag;
    static bool           succeeded = false;
    std::call_once(onceFlag, []() { succeeded = LoadEntryPoints(); });
    return succeeded;
}
