// Minimal Vulkan entry-point loader for the shared FidelityFX Vulkan module.
//
// The static archive borrows volk's pointer variables from the executable (see
// the note at the bottom of cmake/fsr/CMakeLists.txt). A shared module cannot:
// it resolves its own imports, and the engine's volk lives in a different
// binary. Linking the Vulkan SDK's import library instead would add a build-time
// dependency this repository deliberately avoids -- only Vulkan-Headers and volk
// are vendored, and BUILD.md keeps the Vulkan SDK out of the toolchain list.
//
// So this header reproduces volk's arrangement locally: VK_NO_PROTOTYPES turns
// the Vulkan API into function pointers, and ffx_vk_loader.cpp defines the seven
// the SDK backend calls by plain name. Everything else ffx_vk.cpp touches goes
// through its own vkFunctionTable, populated from the vkGetDeviceProcAddr the
// engine hands across in ffxCreateBackendVKDesc.
//
// All seven are core Vulkan 1.0/1.1 and are exported as real symbols by
// vulkan-1.dll / libvulkan.so.1, so no VkInstance is needed to resolve them.
// This header is force-included into the module's SDK sources, so it must come
// before any other vulkan.h inclusion.

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

extern PFN_vkCreateBuffer                       vkCreateBuffer;
extern PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
extern PFN_vkGetDeviceProcAddr                  vkGetDeviceProcAddr;
extern PFN_vkGetPhysicalDeviceFeatures          vkGetPhysicalDeviceFeatures;
extern PFN_vkGetPhysicalDeviceFeatures2         vkGetPhysicalDeviceFeatures2;
extern PFN_vkGetPhysicalDeviceMemoryProperties  vkGetPhysicalDeviceMemoryProperties;
extern PFN_vkGetPhysicalDeviceProperties        vkGetPhysicalDeviceProperties;
extern PFN_vkGetPhysicalDeviceProperties2       vkGetPhysicalDeviceProperties2;

/// Resolve the entry points above from the system Vulkan loader.
///
/// Idempotent and safe to call from several threads. Returns false when the
/// loader is missing or any entry point is unresolved, which the caller must
/// treat as "this backend is unavailable" rather than continuing into the SDK
/// with null pointers.
bool ffxVkLoaderEnsureInitialized(void);

#ifdef __cplusplus
}
#endif
