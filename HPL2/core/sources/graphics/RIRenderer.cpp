#include "graphics/RIRenderer.h"
#include "graphics/RIGPUPreset.h"
#include "graphics/RIProgram.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"
#include "graphics/XessVulkanSupport.h"
#include "graphics/RendererCapabilityPolicy.h"
#include "system/Hasher.h"
#include "system/QStr.h"
#include "system/Types.h"
#include "system/stb_ds.h"
#include <stddef.h>
#include <optional>
#include <vector>

// The single renderer instance (declared in RITypes.h). File-scope / internal
// linkage: the rest of the engine reaches it only through the top-level
// RI*Renderer functions below, never by referencing this object directly.
static RIRenderer g_renderer;

#if DEVICE_MULTI_BACKEND
// Backs the multi-backend path of RIIsTargetSelected (see RITypes.h). In
// single-backend builds the active backend is known at compile time, so this is
// not needed.
uint8_t RIActiveBackendApi() { return g_renderer.api; }
#endif

#if (DEVICE_IMPL_VULKAN)

#include "volk.h"

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

VkInstance RIGetVkInstance() { return g_renderer.vk.instance; }

static inline enum RIVendor_e VendorFromID(uint32_t vendorID) {
  switch (vendorID) {
  case 0x10DE:
    return RI_NVIDIA;
  case 0x1002:
    return RI_AMD;
  case 0x8086:
    return RI_INTEL;
  }
  return RI_UNKNOWN;
}

const static char *DefaultDeviceExtension[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME,
    VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME,
    VK_EXT_SHADER_SUBGROUP_VOTE_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,

    VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME,
    VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
    // Fragment shader interlock extension to be used for ROV type functionality
    // in Vulkan
    VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME,

    /************************************************************************/
    // AMD Specific Extensions
    /************************************************************************/
    VK_AMD_DRAW_INDIRECT_COUNT_EXTENSION_NAME,
    VK_AMD_SHADER_BALLOT_EXTENSION_NAME,
    VK_AMD_GCN_SHADER_EXTENSION_NAME,
    VK_AMD_BUFFER_MARKER_EXTENSION_NAME,
    VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME,
    /************************************************************************/
    // Multi GPU Extensions
    /************************************************************************/
    VK_KHR_DEVICE_GROUP_EXTENSION_NAME,
    /************************************************************************/
    // Bindless & Non Uniform access Extensions
    /************************************************************************/
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_KHR_MAINTENANCE3_EXTENSION_NAME,
    // Required by raytracing and the new bindless descriptor API if we use it
    // in future
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    /************************************************************************/
    // Shader Atomic Int 64 Extension
    /************************************************************************/
    VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME,
    /************************************************************************/
    // YCbCr format support
    /************************************************************************/
    // Requirement for VK_KHR_sampler_ycbcr_conversion
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME,
    /************************************************************************/
    // Dynamic rendering
    /************************************************************************/
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, // Required by
                                                 // VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME
    VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, // Required by
                                               // VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME
    VK_KHR_MULTIVIEW_EXTENSION_NAME, // Required by
                                     // VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME
    /************************************************************************/
    // Present ID / Present Wait (chained into vkQueuePresentKHR for frame
    // pacing)
    /************************************************************************/
    VK_KHR_PRESENT_ID_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    /************************************************************************/
    // Nsight Aftermath
    /************************************************************************/
    VK_EXT_ASTC_DECODE_MODE_EXTENSION_NAME,
    /************************************************************************/
    // Shader debug printf (required by GL_EXT_debug_printf in GLSL)
    /************************************************************************/
    VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
    /************************************************************************/
    // SV_Barycentrics support — VBufferRaster's psMain reads barycentric
    // coords directly off the fragment input to pack into the visibility
    // buffer (see amnesia/slang/VBuffer/VBufferRaster.3d.slang).
    /************************************************************************/
    VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
};

// Only enabled in RENDERER_CAPABILITY_RAYTRACED. Also the definition of a
// "ray tracing extension" when screening XeSS requirements in raster mode.
const static char *RayTracingDeviceExtension[] = {
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    // Required by VK_KHR_ray_tracing_pipeline
    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
    // Required by VK_KHR_spirv_1_4
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,

    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    // Required by VK_KHR_acceleration_structure
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

void VK_ConfigureBufferQueueFamilies(VkBufferCreateInfo *info,
                                     struct RIQueue *queues, size_t numQueues,
                                     uint32_t *queueFamilies,
                                     size_t reservedLen) {
  uint32_t uniqueQueue = 0;
  size_t queueFamilyIndexCount = 0;
  for (size_t i = 0; i < numQueues; i++) {
    if (queues[i].vk.queue) {
      const uint32_t queueBit = (1 << queues[i].vk.queueFamilyIdx);
      if ((uniqueQueue & queueBit) == 0) {
        queueFamilies[queueFamilyIndexCount++] =
            queues[i].vk.queueFamilyIdx; // dev->queues[i].vk.queueFamilyIdx;
      }
      uniqueQueue |= queueBit;
    }
  }
  info->queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndexCount);
  info->pQueueFamilyIndices = queueFamilies;
  info->sharingMode = (queueFamilyIndexCount > 1) ? VK_SHARING_MODE_CONCURRENT
                                                  : VK_SHARING_MODE_EXCLUSIVE;
}

void VK_ConfigureImageQueueFamilies(VkImageCreateInfo *info,
                                    struct RIQueue *queues, size_t numQueues,
                                    uint32_t *queueFamilies,
                                    size_t reservedLen) {
  uint32_t uniqueQueue = 0;
  size_t queueFamilyIndexCount = 0;
  for (size_t i = 0; i < numQueues; i++) {
    if (queues[i].vk.queue) {
      const uint32_t queueBit = (1 << queues[i].vk.queueFamilyIdx);
      if ((uniqueQueue & queueBit) == 0) {
        queueFamilies[queueFamilyIndexCount++] =
            queues[i].vk.queueFamilyIdx; // dev->queues[i].vk.queueFamilyIdx;
      }
      uniqueQueue |= queueBit;
    }
  }
  info->queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndexCount);
  info->pQueueFamilyIndices = queueFamilies;
  info->sharingMode = (queueFamilyIndexCount > 1) ? VK_SHARING_MODE_CONCURRENT
                                                  : VK_SHARING_MODE_EXCLUSIVE;
}

void VK_FillQueueFamilies(struct RIDevice *dev, uint32_t *queueFamilies,
                          uint32_t *queueFamiliesIdx, size_t reservedLen) {
  uint32_t uniqueQueue = 0;
  for (size_t i = 0; i < RI_QUEUE_LEN; i++) {
    if (dev->queues[i].vk.queue) {
      const uint32_t queueBit = (1 << dev->queues[i].vk.queueFamilyIdx);
      if ((uniqueQueue & queueBit) > 0) {
        assert((*queueFamiliesIdx) < reservedLen);
        queueFamilies[(*queueFamiliesIdx)++] = dev->queues[i].vk.queueFamilyIdx;
      }
      uniqueQueue |= queueBit;
    }
  }
}

VkBool32 VKAPI_PTR __VK_DebugUtilsMessenger(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *callbackData, void *userData) {
  switch (messageSeverity) {
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
    // GENERAL-only errors come from the LOADER, not from a validation layer:
    // stale implicit-layer registrations (an overlay/capture app uninstalled but
    // its HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers value left behind, so
    // loader_get_json fails to open the .json), missing optional ICDs, and
    // similar third-party debris. The loader logs these and carries on; they say
    // nothing about our own API usage, and taking the process down over another
    // program's leftover registry key is never right. Warn and continue.
    if ((messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) == 0 &&
        (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) == 0) {
      hpl::Warning("VK LOADER: %s\n", callbackData->pMessage);
      break;
    }
    // assert(callbackData->messageIdNumber ==0xc1c74a9c );
    hpl::FatalError("VK ERROR: %s\n", callbackData->pMessage);
    if (callbackData->messageIdNumber != 0xcc9c32be &&
        callbackData->messageIdNumber != 0x4DAE5635 &&
        callbackData->messageIdNumber != 0x2C8C6E7D)
      assert(false);
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
    hpl::Warning("VK WARNING: %s\n", callbackData->pMessage);
    break;
  default:

    printf("VK INFO: %s\n", callbackData->pMessage);
    hpl::Log("VK INFO: %s\n", callbackData->pMessage);
    break;
  }
  return VK_FALSE;
}

inline static bool __VK_isExtensionNamesSupported(struct QStrSpan extension,
                                                  const char **extensions,
                                                  size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (qStrCompare(qCToStrRef(extensions[i]), extension) == 0) {
      return true;
    }
  }
  return false;
}

inline static bool __VK_isExtensionSupported(const char *targetExt,
                                             VkExtensionProperties *properties,
                                             size_t numExtensions) {
  for (size_t i = 0; i < numExtensions; i++) {
    if (strcmp(properties[i].extensionName, targetExt) == 0) {
      return true;
    }
  }
  return false;
}

static bool __VK_SupportExtension(VkExtensionProperties *properties, size_t len,
                                  struct QStrSpan extension) {
  for (size_t i = 0; i < len; i++) {
    if (qStrCompare(qCToStrRef((properties + i)->extensionName), extension) ==
        0) {
      return true;
    }
  }
  return false;
}

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
static bool __VK_XessFeatureBitsSupported(const VkBool32 *requested,
                                           const VkBool32 *supported,
                                           size_t count, const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (requested[i] != VK_FALSE && supported[i] == VK_FALSE) {
      hpl::Log("XeSS: required device feature is unsupported in %s (field %u)\n",
               name, (unsigned)i);
      return false;
    }
  }
  return true;
}

static bool __VK_ValidateXessFeatureChain(
    const void *chain, const VkBaseOutStructure *const *engineNodes,
    size_t engineNodeCount, const char **reason) {
  const VkBaseOutStructure *node =
      reinterpret_cast<const VkBaseOutStructure *>(chain);
  const VkBaseOutStructure *seenNodes[32] = {};
  size_t seenNodeCount = 0;
  while (node) {
    if (seenNodeCount == sizeof(seenNodes) / sizeof(seenNodes[0])) {
      if (reason)
        *reason = "XeSS returned an excessively long or cyclic feature chain";
      return false;
    }
    for (size_t i = 0; i < seenNodeCount; i++) {
      if (seenNodes[i] == node) {
        if (reason)
          *reason = "XeSS returned a duplicate or cyclic feature chain";
        return false;
      }
    }
    seenNodes[seenNodeCount++] = node;

    bool isEngineNode = false;
    for (size_t i = 0; i < engineNodeCount; i++) {
      if (engineNodes[i] == node) {
        isEngineNode = true;
        break;
      }
    }
    if (!isEngineNode) {
      if (reason)
        *reason = "XeSS returned a feature structure not owned by the engine";
      return false;
    }
    node = node->pNext;
  }

  for (size_t i = 0; i < engineNodeCount; i++) {
    bool present = false;
    for (size_t j = 0; j < seenNodeCount; j++) {
      if (engineNodes[i] == seenNodes[j]) {
        present = true;
        break;
      }
    }
    if (!present) {
      if (reason)
        *reason = "XeSS removed an engine-owned feature structure";
      return false;
    }
  }
  return true;
}

#define VK_XESS_CHECK_FEATURES(type, firstMember, requested, supported)         \
  __VK_XessFeatureBitsSupported(                                               \
      &(requested).firstMember, &(supported).firstMember,                      \
      (sizeof(type) - offsetof(type, firstMember)) / sizeof(VkBool32), #type)
#endif

#endif

int EnumerateRIAdapters(struct RIPhysicalAdapter *adapters,
                        uint32_t *numAdapters) {
#if (DEVICE_IMPL_VULKAN)
  {
    uint32_t deviceGroupNum = 0;
    if (!VK_WrapResult(vkEnumeratePhysicalDeviceGroups(
            g_renderer.vk.instance, &deviceGroupNum, NULL))) {
      return RI_FAIL;
    }

    if (adapters) {
      VkPhysicalDeviceGroupProperties *physicalDeviceGroupProperties =
          (VkPhysicalDeviceGroupProperties *)calloc(
              deviceGroupNum, sizeof(VkPhysicalDeviceGroupProperties));
      for (size_t i = 0; i < deviceGroupNum; i++) {
        physicalDeviceGroupProperties[i].sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
      }
      if (!VK_WrapResult(vkEnumeratePhysicalDeviceGroups(
              g_renderer.vk.instance, &deviceGroupNum, physicalDeviceGroupProperties))) {
        free(physicalDeviceGroupProperties);
        return RI_FAIL;
      }
      assert((*numAdapters) >= deviceGroupNum);
      for (size_t i = 0; i < deviceGroupNum; i++) {
        struct RIPhysicalAdapter *physicalAdapter = &adapters[i];
        memset(physicalAdapter, 0, sizeof(struct RIPhysicalAdapter));
        physicalAdapter->vk.physicalDevice =
            physicalDeviceGroupProperties[i].physicalDevices[0];

        uint32_t extensionNum = 0;
        vkEnumerateDeviceExtensionProperties(physicalAdapter->vk.physicalDevice,
                                             NULL, &extensionNum, NULL);
        VkExtensionProperties *extensionProperties =
            (VkExtensionProperties *)malloc(extensionNum *
                                            sizeof(VkExtensionProperties));
        vkEnumerateDeviceExtensionProperties(physicalAdapter->vk.physicalDevice,
                                             NULL, &extensionNum,
                                             extensionProperties);

        VkPhysicalDeviceProperties2 properties = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceVulkan11Properties props11 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
        VkPhysicalDeviceVulkan12Properties props12 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceVulkan13Properties props13 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceIDProperties deviceIDProperties = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        R_VK_ADD_STRUCT(&properties, &props11);
        R_VK_ADD_STRUCT(&properties, &props12);
        R_VK_ADD_STRUCT(&properties, &props13);
        R_VK_ADD_STRUCT(&properties, &deviceIDProperties);

        VkPhysicalDeviceFeatures2 features = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan11Features features11 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features features12 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features features13 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

        R_VK_ADD_STRUCT(&features, &features11);
        R_VK_ADD_STRUCT(&features, &features12);
        R_VK_ADD_STRUCT(&features, &features13);

        VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
        if (__VK_SupportExtension(
                extensionProperties, extensionNum,
                qCToStrRef(VK_KHR_PRESENT_ID_EXTENSION_NAME))) {
          R_VK_ADD_STRUCT(&features, &presentIdFeatures);
        }

        const bool hasAccelStructExt = __VK_SupportExtension(
            extensionProperties, extensionNum,
            qCToStrRef(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME));
        const bool hasRayTracingPipelineExt = __VK_SupportExtension(
            extensionProperties, extensionNum,
            qCToStrRef(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));
        const bool hasRayQueryExt =
            __VK_SupportExtension(extensionProperties, extensionNum,
                                  qCToStrRef(VK_KHR_RAY_QUERY_EXTENSION_NAME));
        const bool hasDeferredHostOpsExt = __VK_SupportExtension(
            extensionProperties, extensionNum,
            qCToStrRef(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME));
        const bool hasSpirv14Ext = __VK_SupportExtension(
            extensionProperties, extensionNum,
            qCToStrRef(VK_KHR_SPIRV_1_4_EXTENSION_NAME));
        const bool hasShaderFloatControlsExt = __VK_SupportExtension(
            extensionProperties, extensionNum,
            qCToStrRef(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME));

        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelStructProps = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        if (hasAccelStructExt) {
          R_VK_ADD_STRUCT(&properties, &accelStructProps);
          R_VK_ADD_STRUCT(&features, &accelStructFeatures);
        }

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProps = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        if (hasRayTracingPipelineExt) {
          R_VK_ADD_STRUCT(&properties, &rayTracingProps);
          R_VK_ADD_STRUCT(&features, &rayTracingFeatures);
        }

        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        if (hasRayQueryExt) {
          R_VK_ADD_STRUCT(&features, &rayQueryFeatures);
        }

        VkPhysicalDeviceCoherentMemoryFeaturesAMD amdCoherentMemoryFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD};
        const bool hasAmdCoherentMemoryExt = __VK_SupportExtension(
            extensionProperties, extensionNum,
            qCToStrRef(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME));
        if (hasAmdCoherentMemoryExt) {
          R_VK_ADD_STRUCT(&features, &amdCoherentMemoryFeatures);
        }

        VkPhysicalDeviceMemoryProperties memoryProperties = {0};
        vkGetPhysicalDeviceMemoryProperties(physicalAdapter->vk.physicalDevice,
                                            &memoryProperties);
        vkGetPhysicalDeviceProperties2(physicalAdapter->vk.physicalDevice,
                                       &properties);
        vkGetPhysicalDeviceFeatures2(physicalAdapter->vk.physicalDevice,
                                     &features);

        // Fill desc
        physicalAdapter->luid = *(uint64_t *)&deviceIDProperties.deviceLUID[0];
        physicalAdapter->deviceId = properties.properties.deviceID;
        memcpy(physicalAdapter->name, properties.properties.deviceName,
               sizeof(properties.properties.deviceName));
        assert(sizeof(physicalAdapter->name) >=
               sizeof(properties.properties.deviceName));
        physicalAdapter->vendor = VendorFromID(properties.properties.vendorID);
        physicalAdapter->vk.apiVersion = properties.properties.apiVersion;
        physicalAdapter->presetLevel = RI_GPU_PRESET_NONE;
        // selected preset
        for (size_t i = 0; i < ARRAY_COUNT(gpuPCPresets); i++) {
          if (gpuPCPresets[i].vendorId == properties.properties.vendorID &&
              gpuPCPresets[i].modelId == properties.properties.deviceID) {
            physicalAdapter->presetLevel = gpuPCPresets[i].preset;
            break;
          }
        }

        switch (properties.properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
          physicalAdapter->type = RI_ADAPTER_TYPE_INTEGRATED_GPU;
          break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
          physicalAdapter->type = RI_ADAPTER_TYPE_DISCRETE_GPU;
          break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
          physicalAdapter->type = RI_ADAPTER_TYPE_VIRTUAL_GPU;
          break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
          physicalAdapter->type = RI_ADAPTER_TYPE_CPU;
          break;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        default:
          physicalAdapter->type = RI_ADAPTER_TYPE_OTHER;
          break;
        }

        physicalAdapter->vk.isSwapChainSupported =
            __VK_SupportExtension(extensionProperties, extensionNum,
                                  qCToStrRef(VK_KHR_SWAPCHAIN_EXTENSION_NAME));

        physicalAdapter->vk.isPresentIDSupported =
            presentIdFeatures.presentId > 0;
        physicalAdapter->vk.isBufferDeviceAddressSupported =
            physicalAdapter->vk.apiVersion >= VK_API_VERSION_1_2 ||
            __VK_SupportExtension(
                extensionProperties, extensionNum,
                qCToStrRef(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));
        physicalAdapter->vk.isAMDDeviceCoherentMemorySupported =
            hasAmdCoherentMemoryExt &&
            amdCoherentMemoryFeatures.deviceCoherentMemory > 0;

        const VkPhysicalDeviceLimits *limits = &properties.properties.limits;

        physicalAdapter->viewportMaxNum = limits->maxViewports;
        physicalAdapter->viewportBoundsRange[0] =
            static_cast<int32_t>(limits->viewportBoundsRange[0]);
        physicalAdapter->viewportBoundsRange[1] =
            static_cast<int32_t>(limits->viewportBoundsRange[1]);

        physicalAdapter->attachmentMaxDim =
            std::min(limits->maxFramebufferWidth, limits->maxFramebufferHeight);
        physicalAdapter->attachmentLayerMaxNum = limits->maxFramebufferLayers;
        physicalAdapter->colorAttachmentMaxNum = limits->maxColorAttachments;

        physicalAdapter->colorSampleMaxNum =
            limits->framebufferColorSampleCounts;
        physicalAdapter->depthSampleMaxNum =
            limits->framebufferDepthSampleCounts;
        physicalAdapter->stencilSampleMaxNum =
            limits->framebufferStencilSampleCounts;
        physicalAdapter->zeroAttachmentsSampleMaxNum =
            limits->framebufferNoAttachmentsSampleCounts;
        physicalAdapter->textureColorSampleMaxNum =
            limits->sampledImageColorSampleCounts;
        physicalAdapter->textureIntegerSampleMaxNum =
            limits->sampledImageIntegerSampleCounts;
        physicalAdapter->textureDepthSampleMaxNum =
            limits->sampledImageDepthSampleCounts;
        physicalAdapter->textureStencilSampleMaxNum =
            limits->sampledImageStencilSampleCounts;
        physicalAdapter->storageTextureSampleMaxNum =
            limits->storageImageSampleCounts;

        physicalAdapter->texture1DMaxDim = limits->maxImageDimension1D;
        physicalAdapter->texture2DMaxDim = limits->maxImageDimension2D;
        physicalAdapter->texture3DMaxDim = limits->maxImageDimension3D;
        physicalAdapter->textureArrayLayerMaxNum = limits->maxImageArrayLayers;
        physicalAdapter->typedBufferMaxDim = limits->maxTexelBufferElements;

        for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; i++) {
          if ((memoryProperties.memoryHeaps[i].flags &
               VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
              physicalAdapter->type != RI_ADAPTER_TYPE_INTEGRATED_GPU)
            physicalAdapter->videoMemorySize +=
                memoryProperties.memoryHeaps[i].size;
          else
            physicalAdapter->systemMemorySize +=
                memoryProperties.memoryHeaps[i].size;
        }

        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
          const uint32_t uploadHeapFlags =
              (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
          if ((memoryProperties.memoryTypes[i].propertyFlags &
               uploadHeapFlags) == uploadHeapFlags)
            physicalAdapter->deviceUploadHeapSize +=
                memoryProperties.memoryHeaps[i].size;
        }

        physicalAdapter->memoryAllocationMaxNum =
            limits->maxMemoryAllocationCount;
        physicalAdapter->samplerAllocationMaxNum =
            limits->maxSamplerAllocationCount;
        physicalAdapter->constantBufferMaxRange = limits->maxUniformBufferRange;
        physicalAdapter->storageBufferMaxRange = limits->maxStorageBufferRange;
        physicalAdapter->bufferTextureGranularity =
            (uint32_t)limits->bufferImageGranularity;
        physicalAdapter->bufferMaxSize = props13.maxBufferSize;

        physicalAdapter->uploadBufferTextureRowAlignment =
            (uint32_t)limits->optimalBufferCopyRowPitchAlignment;
        physicalAdapter->uploadBufferOffsetAlignment =
            (uint32_t)limits->optimalBufferCopyOffsetAlignment;
        physicalAdapter->bufferShaderResourceOffsetAlignment =
            (uint32_t)std::max(limits->minTexelBufferOffsetAlignment,
                               limits->minStorageBufferOffsetAlignment);
        physicalAdapter->constantBufferOffsetAlignment =
            (uint32_t)limits->minUniformBufferOffsetAlignment;
        if (hasAccelStructExt) {
          physicalAdapter->accelerationStructureScratchOffsetAlignment =
              accelStructProps.minAccelerationStructureScratchOffsetAlignment;
        }
        // physicalAdapter->shaderBindingTableAlignment =
        // rayTracingProps.shaderGroupBaseAlignment;

        physicalAdapter->pipelineLayoutDescriptorSetMaxNum =
            limits->maxBoundDescriptorSets;
        physicalAdapter->pipelineLayoutRootConstantMaxSize =
            limits->maxPushConstantsSize;
        // physicalAdapter->pipelineLayoutRootDescriptorMaxNum =
        // pushDescriptorProps.maxPushDescriptors;

        physicalAdapter->perStageDescriptorSamplerMaxNum =
            limits->maxPerStageDescriptorSamplers;
        physicalAdapter->perStageDescriptorConstantBufferMaxNum =
            limits->maxPerStageDescriptorUniformBuffers;
        physicalAdapter->perStageDescriptorStorageBufferMaxNum =
            limits->maxPerStageDescriptorStorageBuffers;
        physicalAdapter->perStageDescriptorTextureMaxNum =
            limits->maxPerStageDescriptorSampledImages;
        physicalAdapter->perStageDescriptorStorageTextureMaxNum =
            limits->maxPerStageDescriptorStorageImages;
        physicalAdapter->perStageResourceMaxNum = limits->maxPerStageResources;

        physicalAdapter->descriptorSetSamplerMaxNum =
            limits->maxDescriptorSetSamplers;
        physicalAdapter->descriptorSetConstantBufferMaxNum =
            limits->maxDescriptorSetUniformBuffers;
        physicalAdapter->descriptorSetStorageBufferMaxNum =
            limits->maxDescriptorSetStorageBuffers;
        physicalAdapter->descriptorSetTextureMaxNum =
            limits->maxDescriptorSetSampledImages;
        physicalAdapter->descriptorSetStorageTextureMaxNum =
            limits->maxDescriptorSetStorageImages;

        physicalAdapter->vertexShaderAttributeMaxNum =
            limits->maxVertexInputAttributes;
        physicalAdapter->vertexShaderStreamMaxNum =
            limits->maxVertexInputBindings;
        physicalAdapter->vertexShaderOutputComponentMaxNum =
            limits->maxVertexOutputComponents;

        physicalAdapter->tessControlShaderGenerationMaxLevel =
            (float)limits->maxTessellationGenerationLevel;
        physicalAdapter->tessControlShaderPatchPointMaxNum =
            limits->maxTessellationPatchSize;
        physicalAdapter->tessControlShaderPerVertexInputComponentMaxNum =
            limits->maxTessellationControlPerVertexInputComponents;
        physicalAdapter->tessControlShaderPerVertexOutputComponentMaxNum =
            limits->maxTessellationControlPerVertexOutputComponents;
        physicalAdapter->tessControlShaderPerPatchOutputComponentMaxNum =
            limits->maxTessellationControlPerPatchOutputComponents;
        physicalAdapter->tessControlShaderTotalOutputComponentMaxNum =
            limits->maxTessellationControlTotalOutputComponents;
        physicalAdapter->tessEvaluationShaderInputComponentMaxNum =
            limits->maxTessellationEvaluationInputComponents;
        physicalAdapter->tessEvaluationShaderOutputComponentMaxNum =
            limits->maxTessellationEvaluationOutputComponents;

        physicalAdapter->geometryShaderInvocationMaxNum =
            limits->maxGeometryShaderInvocations;
        physicalAdapter->geometryShaderInputComponentMaxNum =
            limits->maxGeometryInputComponents;
        physicalAdapter->geometryShaderOutputComponentMaxNum =
            limits->maxGeometryOutputComponents;
        physicalAdapter->geometryShaderOutputVertexMaxNum =
            limits->maxGeometryOutputVertices;
        physicalAdapter->geometryShaderTotalOutputComponentMaxNum =
            limits->maxGeometryTotalOutputComponents;

        physicalAdapter->fragmentShaderInputComponentMaxNum =
            limits->maxFragmentInputComponents;
        physicalAdapter->fragmentShaderOutputAttachmentMaxNum =
            limits->maxFragmentOutputAttachments;
        physicalAdapter->fragmentShaderDualSourceAttachmentMaxNum =
            limits->maxFragmentDualSrcAttachments;

        physicalAdapter->computeShaderSharedMemoryMaxSize =
            limits->maxComputeSharedMemorySize;
        physicalAdapter->computeShaderWorkGroupMaxNum[0] =
            limits->maxComputeWorkGroupCount[0];
        physicalAdapter->computeShaderWorkGroupMaxNum[1] =
            limits->maxComputeWorkGroupCount[1];
        physicalAdapter->computeShaderWorkGroupMaxNum[2] =
            limits->maxComputeWorkGroupCount[2];
        physicalAdapter->computeShaderWorkGroupInvocationMaxNum =
            limits->maxComputeWorkGroupInvocations;
        physicalAdapter->computeShaderWorkGroupMaxDim[0] =
            limits->maxComputeWorkGroupSize[0];
        physicalAdapter->computeShaderWorkGroupMaxDim[1] =
            limits->maxComputeWorkGroupSize[1];
        physicalAdapter->computeShaderWorkGroupMaxDim[2] =
            limits->maxComputeWorkGroupSize[2];

        if (hasRayTracingPipelineExt) {
          physicalAdapter->rayTracingShaderGroupIdentifierSize =
              rayTracingProps.shaderGroupHandleSize;
          physicalAdapter->rayTracingShaderTableMaxStride =
              rayTracingProps.maxShaderGroupStride;
          physicalAdapter->rayTracingShaderRecursionMaxDepth =
              rayTracingProps.maxRayRecursionDepth;
        }
        if (hasAccelStructExt) {
          physicalAdapter->rayTracingGeometryObjectMaxNum =
              (uint32_t)accelStructProps.maxGeometryCount;
        }

        // physicalAdapter->meshControlSharedMemoryMaxSize =
        // meshShaderProps.maxTaskSharedMemorySize;
        // physicalAdapter->meshControlWorkGroupInvocationMaxNum =
        // meshShaderProps.maxTaskWorkGroupInvocations;
        // physicalAdapter->meshControlPayloadMaxSize =
        // meshShaderProps.maxTaskPayloadSize;
        // physicalAdapter->meshEvaluationOutputVerticesMaxNum =
        // meshShaderProps.maxMeshOutputVertices;
        // physicalAdapter->meshEvaluationOutputPrimitiveMaxNum =
        // meshShaderProps.maxMeshOutputPrimitives;
        // physicalAdapter->meshEvaluationOutputComponentMaxNum =
        // meshShaderProps.maxMeshOutputComponents;
        // physicalAdapter->meshEvaluationSharedMemoryMaxSize =
        // meshShaderProps.maxMeshSharedMemorySize;
        // physicalAdapter->meshEvaluationWorkGroupInvocationMaxNum =
        // meshShaderProps.maxMeshWorkGroupInvocations;

        physicalAdapter->viewportPrecisionBits = limits->viewportSubPixelBits;
        physicalAdapter->subPixelPrecisionBits = limits->subPixelPrecisionBits;
        physicalAdapter->subTexelPrecisionBits = limits->subTexelPrecisionBits;
        physicalAdapter->mipmapPrecisionBits = limits->mipmapPrecisionBits;

        physicalAdapter->timestampFrequencyHz =
            (uint64_t)(1e9 / (double)limits->timestampPeriod + 0.5);
        physicalAdapter->drawIndirectMaxNum = limits->maxDrawIndirectCount;
        physicalAdapter->samplerLodBiasMin = -limits->maxSamplerLodBias;
        physicalAdapter->samplerLodBiasMax = limits->maxSamplerLodBias;
        physicalAdapter->samplerAnisotropyMax = limits->maxSamplerAnisotropy;
        physicalAdapter->texelOffsetMin = limits->minTexelOffset;
        physicalAdapter->texelOffsetMax = limits->maxTexelOffset;
        physicalAdapter->texelGatherOffsetMin = limits->minTexelGatherOffset;
        physicalAdapter->texelGatherOffsetMax = limits->maxTexelGatherOffset;
        physicalAdapter->clipDistanceMaxNum = limits->maxClipDistances;
        physicalAdapter->cullDistanceMaxNum = limits->maxCullDistances;
        physicalAdapter->combinedClipAndCullDistanceMaxNum =
            limits->maxCombinedClipAndCullDistances;
        // physicalAdapter->shadingRateAttachmentTileSize =
        // (uint8_t)shadingRateProps.minFragmentShadingRateAttachmentTexelSize.width;

        // Based on
        // https://docs.vulkan.org/guide/latest/hlsl.html#_shader_model_coverage
        // // TODO: code below needs to be improved physicalAdapter->shaderModel
        // = 51; if (physicalAdapter->isShaderNativeI64Supported)
        //    physicalAdapter->shaderModel = 60;
        // if (features11.multiview)
        //    physicalAdapter->shaderModel = 61;
        // if (physicalAdapter->isShaderNativeF16Supported ||
        // physicalAdapter->isShaderNativeI16Supported)
        //    physicalAdapter->shaderModel = 62;
        // if (physicalAdapter->shadingRateTier >= 2)
        //    physicalAdapter->shaderModel = 64;
        // if (physicalAdapter->isMeshShaderSupported ||
        // physicalAdapter->rayTracingTier >= 2)
        //    physicalAdapter->shaderModel = 65;
        // if (physicalAdapter->isShaderAtomicsI64Supported)
        //    physicalAdapter->shaderModel = 66;
        // if (features.features.shaderStorageImageMultisample)
        //    physicalAdapter->shaderModel = 67;

        // if (physicalAdapter->conservativeRasterTier) {
        //     if (conservativeRasterProps.primitiveOverestimationSize < 1.0f
        //     / 2.0f && conservativeRasterProps.degenerateTrianglesRasterized)
        //         physicalAdapter->conservativeRasterTier = 2;
        //     if (conservativeRasterProps.primitiveOverestimationSize <= 1.0 /
        //     256.0f && conservativeRasterProps.degenerateTrianglesRasterized)
        //         physicalAdapter->conservativeRasterTier = 3;
        // }

        // if (physicalAdapter->sampleLocationsTier) {
        //     if (sampleLocationsProps.variableSampleLocations) // TODO: it's
        //     weird...
        //         physicalAdapter->sampleLocationsTier = 2;
        // }

        physicalAdapter->vk.accelerationStructureExtension = hasAccelStructExt;
        physicalAdapter->vk.rayTracingPipelineExtension = hasRayTracingPipelineExt;
        physicalAdapter->vk.rayQueryExtension = hasRayQueryExt;
        physicalAdapter->vk.deferredHostOperationsExtension =
            (hasDeferredHostOpsExt) ? 1 : 0;

        physicalAdapter->isRayQuerySupported =
            (physicalAdapter->vk.accelerationStructureExtension &&
             physicalAdapter->vk.rayQueryExtension &&
             accelStructFeatures.accelerationStructure &&
             rayQueryFeatures.rayQuery &&
             hasDeferredHostOpsExt &&
             (properties.properties.apiVersion >= VK_API_VERSION_1_2 ||
              (hasSpirv14Ext && hasShaderFloatControlsExt)) &&
             physicalAdapter->vk.isBufferDeviceAddressSupported)
                ? 1
                : 0;

        RendererCapabilitySet tierCapabilities = {};
        tierCapabilities.accelerationStructureExtension = hasAccelStructExt;
        tierCapabilities.accelerationStructure =
            accelStructFeatures.accelerationStructure != VK_FALSE;
        tierCapabilities.rayQueryExtension = hasRayQueryExt;
        tierCapabilities.rayQuery = rayQueryFeatures.rayQuery != VK_FALSE;
        tierCapabilities.rayTracingPipelineExtension = hasRayTracingPipelineExt;
        tierCapabilities.rayTracingPipeline =
            rayTracingFeatures.rayTracingPipeline != VK_FALSE;
        tierCapabilities.bufferDeviceAddress =
            features12.bufferDeviceAddress != VK_FALSE;
        tierCapabilities.deferredHostOperationsExtension = hasDeferredHostOpsExt;
        tierCapabilities.deferredHostOperations = hasDeferredHostOpsExt;
        tierCapabilities.spirv14 =
            properties.properties.apiVersion >= VK_API_VERSION_1_2 ||
            hasSpirv14Ext;
        tierCapabilities.shaderFloatControls =
            properties.properties.apiVersion >= VK_API_VERSION_1_2 ||
            hasShaderFloatControlsExt;
        tierCapabilities.rayTracingPipelineTraceRaysIndirect =
            rayTracingFeatures.rayTracingPipelineTraceRaysIndirect != VK_FALSE;
        physicalAdapter->rayTracingTier =
            RendererEvaluateRayTracingTier(tierCapabilities);

        // if (physicalAdapter->shadingRateTier) {
        //     physicalAdapter->isAdditionalShadingRatesSupported =
        //     shadingRateProps.maxFragmentSize.height > 2 ||
        //     shadingRateProps.maxFragmentSize.width > 2; if
        //     (shadingRateFeatures.primitiveFragmentShadingRate &&
        //     shadingRateFeatures.attachmentFragmentShadingRate)
        //         physicalAdapter->shadingRateTier = 2;
        // }

        physicalAdapter->bindlessTier = features12.descriptorIndexing ? 1 : 0;

        physicalAdapter->isTextureFilterMinMaxSupported =
            features12.samplerFilterMinmax;
        physicalAdapter->isLogicFuncSupported = features.features.logicOp;
        physicalAdapter->isDepthBoundsTestSupported =
            features.features.depthBounds;
        physicalAdapter->isDrawIndirectCountSupported =
            features12.drawIndirectCount;
        physicalAdapter
            ->isIndependentFrontAndBackStencilReferenceAndMasksSupported = true;
        // physicalAdapter->isLineSmoothingSupported =
        // lineRasterizationFeatures.smoothLines;
        physicalAdapter->isCopyQueueTimestampSupported =
            limits->timestampComputeAndGraphics;
        // physicalAdapter->isMeshShaderPipelineStatsSupported =
        // meshShaderFeatures.meshShaderQueries == VK_TRUE;
        physicalAdapter->isEnchancedBarrierSupported = true;
        physicalAdapter->isMemoryTier2Supported =
            true; // TODO: seems to be the best match
        physicalAdapter->isDynamicDepthBiasSupported = true;
        physicalAdapter->isViewportOriginBottomLeftSupported = true;
        physicalAdapter->isRegionResolveSupported = true;

        physicalAdapter->isShaderNativeI16Supported =
            features.features.shaderInt16;
        physicalAdapter->isShaderNativeF16Supported = features12.shaderFloat16;
        physicalAdapter->isShaderNativeI32Supported = true;
        physicalAdapter->isShaderNativeF32Supported = true;
        physicalAdapter->isShaderNativeI64Supported =
            features.features.shaderInt64;
        physicalAdapter->isShaderNativeF64Supported =
            features.features.shaderFloat64;
        // physicalAdapter->isShaderAtomicsF16Supported =
        // (shaderAtomicFloat2Features.shaderBufferFloat16Atomics ||
        // shaderAtomicFloat2Features.shaderSharedFloat16Atomics) ? true :
        // false;
        physicalAdapter->isShaderAtomicsI32Supported = true;
        // physicalAdapter->isShaderAtomicsF32Supported =
        // (shaderAtomicFloatFeatures.shaderBufferFloat32Atomics ||
        // shaderAtomicFloatFeatures.shaderSharedFloat32Atomics) ? true : false;
        physicalAdapter->isShaderAtomicsI64Supported =
            (features12.shaderBufferInt64Atomics ||
             features12.shaderSharedInt64Atomics)
                ? true
                : false;
        // physicalAdapter->isShaderAtomicsF64Supported =
        // (shaderAtomicFloatFeatures.shaderBufferFloat64Atomics ||
        // shaderAtomicFloatFeatures.shaderSharedFloat64Atomics) ? true : false;

        free(extensionProperties);
      }
      free(physicalDeviceGroupProperties);
    } else {
      (*numAdapters) = deviceGroupNum;
    }
  }
#endif
  return RI_SUCCESS;
}

static inline VkDeviceQueueCreateInfo *
__VK_findQueueCreateInfo(VkDeviceQueueCreateInfo *queues, size_t numQueues,
                         uint32_t queueIndex) {
  for (size_t i = 0; i < numQueues; i++) {
    if (queues[i].queueFamilyIndex == queueIndex) {
      return queues + i;
    }
  }
  return NULL;
}

int RIDevice::init(struct RIDeviceDesc *init) {
  assert(init->physicalAdapter);
  memset(this, 0, sizeof(*this));
  struct RIDevice *device = this; // body below predates the method form

  int riResult = RI_SUCCESS;
  struct RIPhysicalAdapter *physicalAdapter = init->physicalAdapter;
  const RendererCapabilityMode_e capabilityMode =
      init->requestRayTracing ? RENDERER_CAPABILITY_RAYTRACED
                              : RENDERER_CAPABILITY_RASTER;
  hpl::cXessVulkanSupport &xessSupport =
      hpl::XessVulkanSupportInstance();
  auto setXessUnavailable = [&](const char *reason) {
    xessSupport.SetUnavailable(reason);
    strncpy(device->xessUnavailableReason, xessSupport.UnavailableReason(),
            sizeof(device->xessUnavailableReason) - 1);
    device->xessUnavailableReason[sizeof(device->xessUnavailableReason) - 1] =
        '\0';
    device->xessAvailable = false;
  };
  if (!xessSupport.IsAvailable()) {
    setXessUnavailable(xessSupport.UnavailableReason());
    hpl::Log("XeSS: device preflight unavailable: %s\n",
             device->xessUnavailableReason);
  }

  device->physicalAdapter = *init->physicalAdapter;
  // Physical support is not the same thing as a feature enabled on this device.
  device->rayTracingEnabled = false;

#if (DEVICE_IMPL_VULKAN)
  {
    const char **enabledExtensionNames = NULL;
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
    size_t xessPlainExtensionCount = 0;
    bool xessRequirementsMerged = false;
#endif

    uint32_t extensionNum = 0;
    vkEnumerateDeviceExtensionProperties(physicalAdapter->vk.physicalDevice,
                                         NULL, &extensionNum, NULL);
    VkExtensionProperties *extensionProperties =
        (VkExtensionProperties *)malloc(extensionNum *
                                        sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(physicalAdapter->vk.physicalDevice,
                                         NULL, &extensionNum,
                                         extensionProperties);

    const bool hasAccelStructExt = __VK_SupportExtension(
        extensionProperties, extensionNum,
        qCToStrRef(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME));
    const bool hasRayTracingPipelineExt = __VK_SupportExtension(
        extensionProperties, extensionNum,
        qCToStrRef(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));
    const bool hasRayQueryExt = __VK_SupportExtension(
        extensionProperties, extensionNum,
        qCToStrRef(VK_KHR_RAY_QUERY_EXTENSION_NAME));
    const bool hasDeferredHostOpsExt = __VK_SupportExtension(
        extensionProperties, extensionNum,
        qCToStrRef(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME));
    const bool hasShaderFloatControlsExt = __VK_SupportExtension(
        extensionProperties, extensionNum,
        qCToStrRef(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME));

    for (size_t i = 0; i < extensionNum; i++) {
      hpl::Log("VK Extension %s - %u\n", extensionProperties[i].extensionName,
               extensionProperties[i].specVersion);
    }

    uint32_t familyNum = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        init->physicalAdapter->vk.physicalDevice, &familyNum, NULL);

    VkQueueFamilyProperties *queueFamilyProps =
        (VkQueueFamilyProperties *)malloc(
            (familyNum * sizeof(VkQueueFamilyProperties)));
    vkGetPhysicalDeviceQueueFamilyProperties(
        init->physicalAdapter->vk.physicalDevice, &familyNum, queueFamilyProps);

    VkDeviceQueueCreateInfo deviceQueueCreateInfo[8] = {};
    VkDeviceCreateInfo deviceCreateInfo = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceCreateInfo.pQueueCreateInfos = deviceQueueCreateInfo;
    const float priorities[] = {1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f};

    {
      struct QStr str = {0};
      uint8_t numFeatures = 0;
      struct QStrSpan queueFeatures[9];
      for (size_t i = 0; i < familyNum; i++) {
        qStrSetLen(&str, 0);
        numFeatures = 0;
        if (queueFamilyProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
          queueFeatures[numFeatures++] = qCToStrRef("VK_QUEUE_GRAPHICS_BIT");
        if (queueFamilyProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
          queueFeatures[numFeatures++] = qCToStrRef("VK_QUEUE_COMPUTE_BIT");
        if (queueFamilyProps[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
          queueFeatures[numFeatures++] = qCToStrRef("VK_QUEUE_TRANSFER_BIT");
        if (queueFamilyProps[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
          queueFeatures[numFeatures++] =
              qCToStrRef("VK_QUEUE_SPARSE_BINDING_BIT");
        if (queueFamilyProps[i].queueFlags & VK_QUEUE_PROTECTED_BIT)
          queueFeatures[numFeatures++] = qCToStrRef("VK_QUEUE_PROTECTED_BIT");
        if (queueFamilyProps[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
          queueFeatures[numFeatures++] =
              qCToStrRef("VK_QUEUE_VIDEO_DECODE_BIT_KHR");
        if (queueFamilyProps[i].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR)
          queueFeatures[numFeatures++] =
              qCToStrRef("VK_QUEUE_VIDEO_ENCODE_BIT_KHR");
        if (queueFamilyProps[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV)
          queueFeatures[numFeatures++] =
              qCToStrRef("VK_QUEUE_OPTICAL_FLOW_BIT_NV");
        qstrcatprintf(&str, "VK Queue - %u: ", (unsigned)i);
        qstrcatjoin(&str, queueFeatures, numFeatures, qCToStrRef(","));
        hpl::Log("%.*s\n", (int)str.len, str.buf);
      }
      qStrFree(&str);
    }

    struct {
      uint32_t requiredBits;
      uint8_t queueType;
    } configureQueue[] = {
        {VK_QUEUE_GRAPHICS_BIT, RI_QUEUE_GRAPHICS},
        {VK_QUEUE_COMPUTE_BIT, RI_QUEUE_COMPUTE},
        {VK_QUEUE_TRANSFER_BIT, RI_QUEUE_COPY},
    };
    for (uint32_t configureIdx = 0; configureIdx < ARRAY_COUNT(configureQueue);
         configureIdx++) {
      // bool found = false;
      const uint32_t requiredBits = configureQueue[configureIdx].requiredBits;

      uint32_t minQueueFlag = UINT32_MAX;
      uint32_t bestQueueFamilyIdx = 0;
      for (size_t familyIdx = 0; familyIdx < familyNum; familyIdx++) {
        // for the graphics queue we select the first avaliable
        if (configureQueue[configureIdx].queueType == RI_QUEUE_GRAPHICS &&
            (configureQueue[configureIdx].requiredBits &
             queueFamilyProps[familyIdx].queueFlags) > 0) {
          bestQueueFamilyIdx = static_cast<uint32_t>(familyIdx);
          break;
        }
        VkDeviceQueueCreateInfo *createInfo = __VK_findQueueCreateInfo(
            deviceQueueCreateInfo, deviceCreateInfo.queueCreateInfoCount,
            static_cast<uint32_t>(familyIdx));
        if (queueFamilyProps[familyIdx].queueCount == 0) {
          continue;
        }

        const uint32_t matchingQueueFlags =
            (queueFamilyProps[familyIdx].queueFlags & requiredBits);
        // Example: Required flag is VK_QUEUE_TRANSFER_BIT and the queue family
        // has only VK_QUEUE_TRANSFER_BIT set
        if (matchingQueueFlags &&
            ((queueFamilyProps[familyIdx].queueFlags & ~requiredBits) == 0) &&
            (queueFamilyProps[familyIdx].queueCount -
             (createInfo ? createInfo->queueCount : 0)) > 0) {
          bestQueueFamilyIdx = static_cast<uint32_t>(familyIdx);
          break;
        }

        // Queue family 1 has VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT
        // Queue family 2 has VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT |
        // VK_QUEUE_SPARSE_BINDING_BIT Since 1 has less flags, we choose queue
        // family 1
        if (matchingQueueFlags && ((queueFamilyProps[familyIdx].queueFlags -
                                    matchingQueueFlags) < minQueueFlag)) {
          bestQueueFamilyIdx = static_cast<uint32_t>(familyIdx);
          minQueueFlag =
              (queueFamilyProps[familyIdx].queueFlags - matchingQueueFlags);
        }
      }

      VkDeviceQueueCreateInfo *createInfo = __VK_findQueueCreateInfo(
          deviceQueueCreateInfo, deviceCreateInfo.queueCreateInfoCount,
          bestQueueFamilyIdx);
      if (createInfo == NULL)
        createInfo =
            &deviceQueueCreateInfo[deviceCreateInfo.queueCreateInfoCount++];
      createInfo->queueFamilyIndex = bestQueueFamilyIdx;
      createInfo->pQueuePriorities = priorities;
      createInfo->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;

      struct RIQueue *queue =
          &device->queues[configureQueue[configureIdx].queueType];
      if (createInfo->queueCount >=
          queueFamilyProps[createInfo->queueFamilyIndex].queueCount) {
        struct RIQueue *dupQueue = NULL;
        minQueueFlag = UINT32_MAX;
        for (size_t i = 0; i < ARRAY_COUNT(device->queues); i++) {
          const uint32_t matchingQueueFlags =
              (device->queues[i].vk.queueFlags & requiredBits);
          if (matchingQueueFlags &&
              ((device->queues[i].vk.queueFlags & ~requiredBits) == 0)) {
            dupQueue = &device->queues[i];
            break;
          }

          if (matchingQueueFlags && ((device->queues[i].vk.queueFlags -
                                      matchingQueueFlags) < minQueueFlag)) {
            minQueueFlag =
                (device->queues[i].vk.queueFlags - matchingQueueFlags);
            dupQueue = &device->queues[i];
          }
        }
        if (dupQueue) {
          device->queues[configureQueue[configureIdx].queueType] = *dupQueue;
        }
      } else {
        queue->vk.queueFlags =
            queueFamilyProps[createInfo->queueFamilyIndex].queueFlags;
        queue->vk.slotIdx = createInfo->queueCount++;
        queue->vk.queueFamilyIdx = createInfo->queueFamilyIndex;
      }
    }

    // for( uint32_t initIdx = 0; initIdx < ARRAY_COUNT( configureQueue );
    // initIdx++ ) { 	VkDeviceQueueCreateInfo *selectedQueue = NULL; 	bool
    // found =
    // false; 	uint32_t minQueueFlag = UINT32_MAX; 	const uint32_t
    // requiredFlags = configureQueue[initIdx].requiredBits; 	for( size_t
    // familyIdx = 0; familyIdx < familyNum; familyIdx++ ) { 		uint32_t
    // avaliableQueues = 0; 		size_t createQueueIdx = 0;
    // for( ; createQueueIdx < ARRAY_COUNT( deviceQueueCreateInfo );
    // createQueueIdx++ ) { 			const bool foundQueueFamily =
    // deviceQueueCreateInfo[createQueueIdx].queueFamilyIndex == familyIdx;
    // const bool isQueueEmpty
    //=(deviceQueueCreateInfo[createQueueIdx].queueCount == 0);
    // if( foundQueueFamily || isQueueEmpty) {
    // selectedQueue = &deviceQueueCreateInfo[createQueueIdx];
    // if(isQueueEmpty) {
    // deviceCreateInfo.queueCreateInfoCount = Q_MAX(
    // deviceCreateInfo.queueCreateInfoCount, createQueueIdx + 1);
    //					selectedQueue->pQueuePriorities =
    // priorities; 					selectedQueue->sType =
    // VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    //				}
    //				selectedQueue->queueFamilyIndex = familyIdx;
    //				avaliableQueues =
    // queueFamilyProps[familyIdx].queueCount - selectedQueue->queueCount;
    // break;
    //			}
    //		}

    //		// for the graphics queue we select the first avaliable
    //		if( configureQueue[initIdx].queueType == RI_QUEUE_GRAPHICS && (
    // configureQueue[initIdx].requiredBits &
    // queueFamilyProps[familyIdx].queueFlags ) > 0 ) {
    // found = true; 			break;
    //		}

    //		assert( createQueueIdx < ARRAY_COUNT( deviceQueueCreateInfo ) );
    //		if( avaliableQueues == 0 ) {
    //			continue; // skip queue family there is no more
    // avaliable
    //		}
    //		const uint32_t matchingQueueFlags = (
    // queueFamilyProps[familyIdx].queueFlags & requiredFlags );

    //		// Example: Required flag is VK_QUEUE_TRANSFER_BIT and the queue
    // family has only VK_QUEUE_TRANSFER_BIT set 		if(
    // matchingQueueFlags && ( ( queueFamilyProps[familyIdx].queueFlags &
    // ~requiredFlags ) == 0 ) && avaliableQueues > 0 ) {
    // found = true; 			break;
    //		}

    //		// Queue family 1 has VK_QUEUE_TRANSFER_BIT |
    // VK_QUEUE_COMPUTE_BIT
    //		// Queue family 2 has VK_QUEUE_TRANSFER_BIT |
    // VK_QUEUE_COMPUTE_BIT | VK_QUEUE_SPARSE_BINDING_BIT
    //		// Since 1 has less flags, we choose queue family 1
    //		if( matchingQueueFlags && ( (
    // queueFamilyProps[familyIdx].queueFlags - matchingQueueFlags ) <
    // minQueueFlag ) ) { 			found = true;
    // minQueueFlag = ( queueFamilyProps[familyIdx].queueFlags -
    // matchingQueueFlags );
    //		}
    //	}

    //	if( found ) {
    //		struct RIQueue *queue =
    //&device->queues[configureQueue[initIdx].queueType];
    // queue->vk.queueFlags =
    // queueFamilyProps[selectedQueue->queueFamilyIndex].queueFlags;
    //		queue->vk.slotIdx = selectedQueue->queueCount++;
    //		queue->vk.queueFamilyIdx = selectedQueue->queueFamilyIndex;
    //	} else {
    //		struct RIQueue *dupQueue = NULL;
    //		minQueueFlag = UINT32_MAX;
    //		for( size_t i = 0; i < ARRAY_COUNT( device->queues ); i++ ) {
    //			const uint32_t matchingQueueFlags = (
    // device->queues[i].vk.queueFlags & requiredFlags ); if( matchingQueueFlags
    //&& ( ( device->queues[i].vk.queueFlags & ~requiredFlags ) == 0 ) ) {
    //				dupQueue = &device->queues[i];
    //				break;
    //			}

    //			if( matchingQueueFlags && ( (
    // device->queues[i].vk.queueFlags - matchingQueueFlags ) < minQueueFlag ) )
    //{ 				found = true;
    // minQueueFlag = ( device->queues[i].vk.queueFlags - matchingQueueFlags );
    // dupQueue = &device->queues[i];
    //			}
    //		}
    //		if( dupQueue ) {
    //			device->queues[configureQueue[initIdx].queueType] =
    //*dupQueue;
    //		}
    //	}
    //}

    for (size_t idx = 0; idx < ARRAY_COUNT(DefaultDeviceExtension); idx++) {
      if (__VK_SupportExtension(extensionProperties, extensionNum,
                                qCToStrRef(DefaultDeviceExtension[idx]))) {
        hpl::Log("Enabled Extension: %s\n", DefaultDeviceExtension[idx]);
        arrpush(enabledExtensionNames, DefaultDeviceExtension[idx]);
      }
    }
    if (capabilityMode == RENDERER_CAPABILITY_RAYTRACED) {
      for (size_t idx = 0; idx < ARRAY_COUNT(RayTracingDeviceExtension); idx++) {
        if (__VK_SupportExtension(extensionProperties, extensionNum,
                                  qCToStrRef(RayTracingDeviceExtension[idx]))) {
          hpl::Log("Enabled Extension: %s\n", RayTracingDeviceExtension[idx]);
          arrpush(enabledExtensionNames, RayTracingDeviceExtension[idx]);
        }
      }
    } else {
      hpl::Log("Ray tracing extensions skipped: raster capability mode\n");
    }

    VkPhysicalDeviceFeatures2 features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    VkPhysicalDeviceVulkan11Features features11 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    R_VK_ADD_STRUCT(&features, &features11);

    VkPhysicalDeviceVulkan12Features features12 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    R_VK_ADD_STRUCT(&features, &features12);

    VkPhysicalDeviceVulkan13Features features13 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    if (g_renderer.vk.apiVersion >= VK_API_VERSION_1_3) {
      R_VK_ADD_STRUCT(&features, &features13);
    }

    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5Features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    if (__VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_MAINTENANCE_5_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames))) {
      R_VK_ADD_STRUCT(&features, &maintenance5Features);
      device->vk.maintenance5Features = true;
    }

    // VkPhysicalDeviceFragmentShadingRateFeaturesKHR shadingRateFeatures = {
    // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    // if( __VK_isExtensionSupported(
    // VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, , extensionProperties,
    // extensionNum ) ) {
    //	APPEND_EXT( shadingRateFeatures );
    // }

    VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    if (__VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_PRESENT_ID_EXTENSION_NAME), enabledExtensionNames,
            arrlen(enabledExtensionNames))) {
      R_VK_ADD_STRUCT(&features, &presentIdFeatures);
    }

    VkPhysicalDeviceCoherentMemoryFeaturesAMD amdCoherentMemoryFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD};
    const bool amdCoherentMemoryExtensionEnabled =
        __VK_isExtensionNamesSupported(
            qCToStrRef(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames));
    if (amdCoherentMemoryExtensionEnabled) {
      R_VK_ADD_STRUCT(&features, &amdCoherentMemoryFeatures);
    }

    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    if (__VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_PRESENT_WAIT_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames))) {
      R_VK_ADD_STRUCT(&features, &presentWaitFeatures);
    }

    VkPhysicalDeviceLineRasterizationFeaturesKHR lineRasterizationFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_KHR};
    if (__VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_LINE_RASTERIZATION_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames))) {
      R_VK_ADD_STRUCT(&features, &lineRasterizationFeatures);
    }

    // VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures = {
    // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT }; if(
    // IsExtensionSupported( VK_EXT_MESH_SHADER_EXTENSION_NAME,
    // desiredDeviceExts ) ) { 	APPEND_EXT( meshShaderFeatures );
    // }

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures =
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    if (__VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames))) {
      R_VK_ADD_STRUCT(&features, &accelerationStructureFeatures);
    }

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    if (__VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames))) {
      R_VK_ADD_STRUCT(&features, &rayTracingPipelineFeatures);
    }

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    if (__VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_RAY_QUERY_EXTENSION_NAME), enabledExtensionNames,
            arrlen(enabledExtensionNames))) {
      R_VK_ADD_STRUCT(&features, &rayQueryFeatures);
    }

    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR
        fragmentBarycentricFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    if (__VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames))) {
      R_VK_ADD_STRUCT(&features, &fragmentBarycentricFeatures);
    }

    // VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR
    // rayTracingMaintenanceFeatures =
    // {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR};
    // if (IsExtensionSupported(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
    // desiredDeviceExts)) {
    //     APPEND_EXT(rayTracingMaintenanceFeatures);
    // }

    // VkPhysicalDeviceOpacityMicromapFeaturesEXT micromapFeatures =
    // {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT}; if
    // (IsExtensionSupported(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME,
    // desiredDeviceExts)) {
    //     APPEND_EXT(micromapFeatures);
    // }

    // VkPhysicalDeviceShaderAtomicFloatFeaturesEXT shaderAtomicFloatFeatures =
    // {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT}; if
    // (IsExtensionSupported(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
    // desiredDeviceExts)) {
    //     APPEND_EXT(shaderAtomicFloatFeatures);
    // }

    // VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT shaderAtomicFloat2Features
    // = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT};
    // if (IsExtensionSupported(VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME,
    // desiredDeviceExts)) {
    //     APPEND_EXT(shaderAtomicFloat2Features);
    // }

    // VkPhysicalDeviceMemoryPriorityFeaturesEXT memoryPriorityFeatures =
    // {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT}; if
    // (IsExtensionSupported(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME,
    // desiredDeviceExts)) {
    //     APPEND_EXT(memoryPriorityFeatures);
    // }

    // VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT slicedViewFeatures =
    // {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_SLICED_VIEW_OF_3D_FEATURES_EXT};
    // if (IsExtensionSupported(VK_EXT_IMAGE_SLICED_VIEW_OF_3D_EXTENSION_NAME,
    // desiredDeviceExts)) {
    //     APPEND_EXT(slicedViewFeatures);
    // }

    // VkPhysicalDeviceCustomBorderColorFeaturesEXT borderColorFeatures =
    // {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT}; if
    // (IsExtensionSupported(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    // desiredDeviceExts)) {
    //     APPEND_EXT(borderColorFeatures);
    // }

    vkGetPhysicalDeviceFeatures2(physicalAdapter->vk.physicalDevice, &features);

    RendererCapabilitySet capabilitySet = {};
    capabilitySet.swapchain = physicalAdapter->vk.isSwapChainSupported;
    capabilitySet.descriptorIndexing = features12.descriptorIndexing != 0;
    capabilitySet.bufferDeviceAddress = features12.bufferDeviceAddress != 0;
    capabilitySet.scalarBlockLayout = features12.scalarBlockLayout != 0;
    capabilitySet.shaderInt64 = features.features.shaderInt64 != 0;
    capabilitySet.shaderBufferInt64Atomics =
        features12.shaderBufferInt64Atomics != 0;
    capabilitySet.fragmentShaderBarycentric = __VK_isExtensionNamesSupported(
        qCToStrRef(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME),
        enabledExtensionNames, arrlen(enabledExtensionNames));
    capabilitySet.fragmentShaderInterlock = __VK_isExtensionNamesSupported(
        qCToStrRef(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME),
        enabledExtensionNames, arrlen(enabledExtensionNames));
    capabilitySet.dynamicRendering = features13.dynamicRendering != 0;
    capabilitySet.accelerationStructure = accelerationStructureFeatures.accelerationStructure != 0;
    capabilitySet.rayQuery = rayQueryFeatures.rayQuery != 0;
    capabilitySet.rayTracingPipeline = rayTracingPipelineFeatures.rayTracingPipeline != 0;
    capabilitySet.spirv14 = physicalAdapter->vk.apiVersion >= VK_API_VERSION_1_2 ||
                            __VK_isExtensionNamesSupported(
        qCToStrRef(VK_KHR_SPIRV_1_4_EXTENSION_NAME), enabledExtensionNames,
        arrlen(enabledExtensionNames));
    capabilitySet.shaderFloatControls =
        physicalAdapter->vk.apiVersion >= VK_API_VERSION_1_2 ||
        hasShaderFloatControlsExt;
    capabilitySet.deferredHostOperations =
        hasDeferredHostOpsExt;
    capabilitySet.accelerationStructureExtension = hasAccelStructExt;
    capabilitySet.rayQueryExtension = hasRayQueryExt;
    capabilitySet.rayTracingPipelineExtension = hasRayTracingPipelineExt;
    capabilitySet.deferredHostOperationsExtension = hasDeferredHostOpsExt;
    capabilitySet.spirv14Extension = __VK_isExtensionSupported(
        VK_KHR_SPIRV_1_4_EXTENSION_NAME, extensionProperties, extensionNum);
    capabilitySet.shaderFloatControlsExtension = __VK_isExtensionSupported(
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME, extensionProperties,
        extensionNum);
    capabilitySet.descriptorBindingPartiallyBound = features12.descriptorBindingPartiallyBound;
    capabilitySet.shaderSampledImageArrayNonUniformIndexing =
        features12.shaderSampledImageArrayNonUniformIndexing;
    capabilitySet.descriptorBindingSampledImageUpdateAfterBind =
        features12.descriptorBindingSampledImageUpdateAfterBind;
    capabilitySet.descriptorBindingStorageBufferUpdateAfterBind =
        features12.descriptorBindingStorageBufferUpdateAfterBind;
    capabilitySet.descriptorBindingStorageImageUpdateAfterBind =
        features12.descriptorBindingStorageImageUpdateAfterBind;
    capabilitySet.rayTracingPipelineTraceRaysIndirect =
        rayTracingPipelineFeatures.rayTracingPipelineTraceRaysIndirect;
    const RendererCapabilityDecision capabilityDecision =
        RendererEvaluateCapabilityPolicy(capabilityMode, capabilitySet);
    if (!capabilityDecision.supported) {
      hpl::Log("ERROR: renderer capability policy rejected adapter: %s\n",
               capabilityDecision.missingRequirement);
      riResult = RI_UNSUPPORTED;
      free(queueFamilyProps);
      free(extensionProperties);
      arrfree(enabledExtensionNames);
      return riResult;
    }

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
    // Keep copies of every engine-owned feature structure. XeSS may patch
    // these structures and append SDK-owned structures to the chain; the
    // copies let the ordinary Vulkan path be restored without querying the
    // GPU again and accidentally replacing requirements with support bits.
    xessPlainExtensionCount = arrlen(enabledExtensionNames);
    void *xessFeatureChain = &features;
    // Capture exact engine-owned nodes before XeSS mutates the chain. An SDK
    // node must not be accepted just because it uses a known sType.
    const VkBaseOutStructure *xessEngineFeatureNodes[32] = {};
    size_t xessEngineFeatureNodeCount = 0;
    for (const VkBaseOutStructure *node =
             reinterpret_cast<const VkBaseOutStructure *>(&features);
         node && xessEngineFeatureNodeCount <
                     sizeof(xessEngineFeatureNodes) /
                         sizeof(xessEngineFeatureNodes[0]);
         node = node->pNext) {
      xessEngineFeatureNodes[xessEngineFeatureNodeCount++] = node;
    }
    const VkPhysicalDeviceFeatures2 xessSupportedFeatures = features;
    const VkPhysicalDeviceVulkan11Features xessSupportedFeatures11 = features11;
    const VkPhysicalDeviceVulkan12Features xessSupportedFeatures12 = features12;
    const VkPhysicalDeviceVulkan13Features xessSupportedFeatures13 = features13;
    const VkPhysicalDeviceMaintenance5FeaturesKHR
        xessSupportedMaintenance5Features = maintenance5Features;
    const VkPhysicalDevicePresentIdFeaturesKHR xessSupportedPresentIdFeatures =
        presentIdFeatures;
    const VkPhysicalDevicePresentWaitFeaturesKHR
        xessSupportedPresentWaitFeatures = presentWaitFeatures;
    const VkPhysicalDeviceLineRasterizationFeaturesKHR
        xessSupportedLineRasterizationFeatures = lineRasterizationFeatures;
    const VkPhysicalDeviceAccelerationStructureFeaturesKHR
        xessSupportedAccelerationStructureFeatures =
            accelerationStructureFeatures;
    const VkPhysicalDeviceRayTracingPipelineFeaturesKHR
        xessSupportedRayTracingPipelineFeatures = rayTracingPipelineFeatures;
    const VkPhysicalDeviceRayQueryFeaturesKHR xessSupportedRayQueryFeatures =
        rayQueryFeatures;
    const VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR
        xessSupportedFragmentBarycentricFeatures = fragmentBarycentricFeatures;
    const VkPhysicalDeviceCoherentMemoryFeaturesAMD
        xessSupportedAmdCoherentMemoryFeatures = amdCoherentMemoryFeatures;

    auto restorePlainXessRequirements = [&]() {
      arrsetlen(enabledExtensionNames, xessPlainExtensionCount);
      features = xessSupportedFeatures;
      features11 = xessSupportedFeatures11;
      features12 = xessSupportedFeatures12;
      features13 = xessSupportedFeatures13;
      maintenance5Features = xessSupportedMaintenance5Features;
      presentIdFeatures = xessSupportedPresentIdFeatures;
      presentWaitFeatures = xessSupportedPresentWaitFeatures;
      lineRasterizationFeatures = xessSupportedLineRasterizationFeatures;
      accelerationStructureFeatures = xessSupportedAccelerationStructureFeatures;
      rayTracingPipelineFeatures = xessSupportedRayTracingPipelineFeatures;
      rayQueryFeatures = xessSupportedRayQueryFeatures;
      fragmentBarycentricFeatures = xessSupportedFragmentBarycentricFeatures;
      amdCoherentMemoryFeatures = xessSupportedAmdCoherentMemoryFeatures;
    };

    if (xessSupport.IsAvailable()) {
      const auto getRequiredDeviceExtensions =
          xessSupport.VKGetRequiredDeviceExtensions();
      const auto getRequiredDeviceFeatures =
          xessSupport.VKGetRequiredDeviceFeatures();
      if (!getRequiredDeviceExtensions || !getRequiredDeviceFeatures) {
        setXessUnavailable("required device query entry point is unavailable");
        hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
      } else {
      uint32_t requiredExtensionCount = 0;
      const char *const *requiredExtensionNames = NULL;
      xess_result_t xessResult =
          getRequiredDeviceExtensions(
              g_renderer.vk.instance, physicalAdapter->vk.physicalDevice,
              &requiredExtensionCount, &requiredExtensionNames);
      if (xessResult != XESS_RESULT_SUCCESS) {
        char reason[128];
        snprintf(reason, sizeof(reason),
                 "required device extension query failed (%d)",
                 (int)xessResult);
        setXessUnavailable(reason);
        hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
      } else if (requiredExtensionCount && !requiredExtensionNames) {
        setXessUnavailable("required device extension query returned no names");
        hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
      } else {
        bool deviceExtensionsSupported = true;
        for (uint32_t extensionIdx = 0; extensionIdx < requiredExtensionCount;
             extensionIdx++) {
          const char *requiredName = requiredExtensionNames[extensionIdx];
          if (capabilityMode == RENDERER_CAPABILITY_RASTER && requiredName &&
              __VK_isExtensionNamesSupported(
                  qCToStrRef(requiredName), RayTracingDeviceExtension,
                  ARRAY_COUNT(RayTracingDeviceExtension))) {
            setXessUnavailable(
                "XeSS requested a ray-tracing extension prohibited in raster mode");
            hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
            deviceExtensionsSupported = false;
            break;
          }
          if (!requiredName ||
              !__VK_isExtensionSupported(requiredName, extensionProperties,
                                          extensionNum)) {
            char reason[128];
            snprintf(reason, sizeof(reason),
                     "required device extension unsupported: %s",
                     requiredName ? requiredName : "<null>");
            setXessUnavailable(reason);
            hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
            deviceExtensionsSupported = false;
            break;
          }
        }
        if (deviceExtensionsSupported) {
          for (uint32_t extensionIdx = 0; extensionIdx < requiredExtensionCount;
               extensionIdx++) {
            const char *requiredName = requiredExtensionNames[extensionIdx];
            if (!__VK_isExtensionNamesSupported(
                    qCToStrRef(requiredName), enabledExtensionNames,
                    arrlen(enabledExtensionNames))) {
              arrpush(enabledExtensionNames, requiredName);
              hpl::Log("XeSS: enabled device extension %s\n", requiredName);
            }
          }

          xessResult = getRequiredDeviceFeatures(
              g_renderer.vk.instance, physicalAdapter->vk.physicalDevice,
              &xessFeatureChain);
          if (xessResult != XESS_RESULT_SUCCESS) {
            char reason[128];
            snprintf(reason, sizeof(reason),
                     "required device feature query failed (%d)",
                     (int)xessResult);
            setXessUnavailable(reason);
            hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
          } else if (!xessFeatureChain) {
            setXessUnavailable("required device feature query returned no chain");
            hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
          } else {
            const char *chainReason = nullptr;
            const bool chainValid = __VK_ValidateXessFeatureChain(
                xessFeatureChain, xessEngineFeatureNodes,
                xessEngineFeatureNodeCount, &chainReason);
            if (!chainValid) {
              setXessUnavailable(chainReason);
              hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
            }
            bool featuresSupported = __VK_XessFeatureBitsSupported(
                reinterpret_cast<const VkBool32 *>(&features.features),
                reinterpret_cast<const VkBool32 *>(&xessSupportedFeatures.features),
                sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32),
                "VkPhysicalDeviceFeatures");
            featuresSupported =
                VK_XESS_CHECK_FEATURES(VkPhysicalDeviceVulkan11Features,
                                       storageBuffer16BitAccess, features11,
                                       xessSupportedFeatures11) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(VkPhysicalDeviceVulkan12Features,
                                       samplerMirrorClampToEdge, features12,
                                       xessSupportedFeatures12) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(VkPhysicalDeviceVulkan13Features,
                                       robustImageAccess, features13,
                                       xessSupportedFeatures13) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(VkPhysicalDeviceMaintenance5FeaturesKHR,
                                       maintenance5, maintenance5Features,
                                       xessSupportedMaintenance5Features) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(VkPhysicalDevicePresentIdFeaturesKHR,
                                       presentId, presentIdFeatures,
                                       xessSupportedPresentIdFeatures) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(VkPhysicalDevicePresentWaitFeaturesKHR,
                                       presentWait, presentWaitFeatures,
                                       xessSupportedPresentWaitFeatures) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(
                    VkPhysicalDeviceLineRasterizationFeaturesKHR,
                    rectangularLines, lineRasterizationFeatures,
                    xessSupportedLineRasterizationFeatures) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(
                    VkPhysicalDeviceAccelerationStructureFeaturesKHR,
                    accelerationStructure, accelerationStructureFeatures,
                    xessSupportedAccelerationStructureFeatures) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(
                    VkPhysicalDeviceRayTracingPipelineFeaturesKHR,
                    rayTracingPipeline, rayTracingPipelineFeatures,
                    xessSupportedRayTracingPipelineFeatures) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(VkPhysicalDeviceRayQueryFeaturesKHR,
                                       rayQuery, rayQueryFeatures,
                                       xessSupportedRayQueryFeatures) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(
                    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR,
                    fragmentShaderBarycentric, fragmentBarycentricFeatures,
                    xessSupportedFragmentBarycentricFeatures) &&
                featuresSupported;
            featuresSupported =
                VK_XESS_CHECK_FEATURES(
                    VkPhysicalDeviceCoherentMemoryFeaturesAMD,
                    deviceCoherentMemory, amdCoherentMemoryFeatures,
                    xessSupportedAmdCoherentMemoryFeatures) &&
                featuresSupported;

            if (chainValid && featuresSupported) {
              xessRequirementsMerged = true;
              hpl::Log("XeSS: device extensions and feature requirements accepted\n");
            } else {
              setXessUnavailable("required device feature unsupported");
              hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
            }
          }
        }
      }
      }
      if (!xessRequirementsMerged)
        restorePlainXessRequirements();
    }
#endif

    // Declared up front so the scalar-block-layout `goto vk_done` below
    // doesn't skip an initialization (MSVC C2362). Reused by both the
    // vkCreateDevice and vmaCreateAllocator calls further down.
    VkResult result = VK_SUCCESS;

    // Scalar block layout is load-bearing: the Slang structs in
    // SceneTypes.slang are compiled with -fvk-use-scalar-layout and the host
    // C++ packing matches that layout (no _pad fields). Without this feature,
    // SSBO/UBO reads land at the wrong offsets and validation layers emit
    // VUID-...-Layout errors.
    if (!features12.scalarBlockLayout) {
      hpl::Log("ERROR: Vulkan device does not advertise scalarBlockLayout — "
               "required by SceneTypes.slang scalar layout\n");
      riResult = RI_FAIL;
      goto vk_done;
    }

    deviceCreateInfo.pNext = &features;
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
    if (xessRequirementsMerged)
      deviceCreateInfo.pNext = xessFeatureChain;
#endif
    deviceCreateInfo.pQueueCreateInfos = deviceQueueCreateInfo;
    deviceCreateInfo.enabledExtensionCount =
        (uint32_t)arrlen(enabledExtensionNames);
    deviceCreateInfo.ppEnabledExtensionNames = enabledExtensionNames;

    result = vkCreateDevice(physicalAdapter->vk.physicalDevice,
                            &deviceCreateInfo, NULL, &device->vk.device);
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
    if (!VK_WrapResult(result) && xessRequirementsMerged) {
      char reason[128];
      snprintf(reason, sizeof(reason),
               "device creation with XeSS requirements failed (%d); fell back to plain device",
               (int)result);
      setXessUnavailable(reason);
      hpl::Log("XeSS: %s\n", device->xessUnavailableReason);
      restorePlainXessRequirements();
      xessRequirementsMerged = false;
      deviceCreateInfo.pNext = &features;
      deviceCreateInfo.enabledExtensionCount =
          (uint32_t)arrlen(enabledExtensionNames);
      deviceCreateInfo.ppEnabledExtensionNames = enabledExtensionNames;
      result = vkCreateDevice(physicalAdapter->vk.physicalDevice,
                              &deviceCreateInfo, NULL, &device->vk.device);
    }
#endif
    if (!VK_WrapResult(result)) {
      riResult = RI_FAIL;
      goto vk_done;
    }
    // Publish logical capability state only after vkCreateDevice succeeds.
    // These flags describe the feature bits actually submitted to Vulkan,
    // rather than physical-adapter advertisements or a failed attempt.
    device->accelerationStructureEnabled =
        capabilityMode == RENDERER_CAPABILITY_RAYTRACED &&
        accelerationStructureFeatures.accelerationStructure != VK_FALSE;
    device->rayTracingPipelineEnabled =
        capabilityMode == RENDERER_CAPABILITY_RAYTRACED &&
        rayTracingPipelineFeatures.rayTracingPipeline != VK_FALSE;
    device->rayQueryEnabled =
        capabilityMode == RENDERER_CAPABILITY_RAYTRACED &&
        rayQueryFeatures.rayQuery != VK_FALSE;
    device->fragmentShaderBarycentricEnabled =
        __VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames)) &&
        fragmentBarycentricFeatures.fragmentShaderBarycentric != VK_FALSE;
    device->shaderInt16Enabled = features.features.shaderInt16 != VK_FALSE;
    device->shaderFloat16Enabled = features12.shaderFloat16 != VK_FALSE;
    device->geometryShaderEnabled = features.features.geometryShader != VK_FALSE;
    device->occlusionQueryPreciseEnabled =
        features.features.occlusionQueryPrecise != VK_FALSE;
    device->rayTracingEnabled = device->accelerationStructureEnabled &&
                                device->rayTracingPipelineEnabled;
    device->vk.accelerationStructureExtensionEnabled =
        __VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames));
    device->vk.rayTracingPipelineExtensionEnabled =
        __VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames));
    device->vk.rayQueryExtensionEnabled = __VK_isExtensionNamesSupported(
        qCToStrRef(VK_KHR_RAY_QUERY_EXTENSION_NAME), enabledExtensionNames,
        arrlen(enabledExtensionNames));
    device->vk.deferredHostOperationsExtensionEnabled =
        __VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames));
    device->vk.spirv14ExtensionEnabled = __VK_isExtensionNamesSupported(
        qCToStrRef(VK_KHR_SPIRV_1_4_EXTENSION_NAME), enabledExtensionNames,
        arrlen(enabledExtensionNames));
    device->vk.shaderFloatControlsExtensionEnabled =
        __VK_isExtensionNamesSupported(
            qCToStrRef(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME),
            enabledExtensionNames, arrlen(enabledExtensionNames));
    device->vk.deviceCoherentMemoryEnabled =
        amdCoherentMemoryExtensionEnabled &&
        amdCoherentMemoryFeatures.deviceCoherentMemory != 0;
    hpl::Log("Device coherent memory enabled: %u\n",
             (unsigned)device->vk.deviceCoherentMemoryEnabled);
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
    if (xessRequirementsMerged) {
      device->xessAvailable = true;
      device->xessUnavailableReason[0] = '\0';
      hpl::Log("XeSS: Vulkan device created with XeSS requirements\n");
    }
#endif

    // Load device-direct entrypoints for the device we actually use. Without
    // this, volkLoadInstance left device functions dispatching through the
    // instance trampoline, which can leave GPU-assisted validation unable to
    // track acceleration-structure addresses (false VUID-12281 rejections).
    volkLoadDevice(device->vk.device);

    // the request size
    for (size_t q = 0; q < ARRAY_COUNT(device->queues); q++) {
      // the queue
      if (device->queues[q].vk.queueFlags == 0)
        continue;
      vkGetDeviceQueue(device->vk.device, device->queues[q].vk.queueFamilyIdx,
                       device->queues[q].vk.slotIdx,
                       &device->queues[q].vk.queue);
    }

    {
      VmaVulkanFunctions vulkanFunctions = {0};
      vulkanFunctions.vkGetPhysicalDeviceProperties =
          vkGetPhysicalDeviceProperties;
      vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
      vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
      vulkanFunctions.vkGetPhysicalDeviceProperties =
          vkGetPhysicalDeviceProperties;
      vulkanFunctions.vkGetPhysicalDeviceMemoryProperties =
          vkGetPhysicalDeviceMemoryProperties;
      vulkanFunctions.vkAllocateMemory = vkAllocateMemory;
      vulkanFunctions.vkFreeMemory = vkFreeMemory;
      vulkanFunctions.vkMapMemory = vkMapMemory;
      vulkanFunctions.vkUnmapMemory = vkUnmapMemory;
      vulkanFunctions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
      vulkanFunctions.vkInvalidateMappedMemoryRanges =
          vkInvalidateMappedMemoryRanges;
      vulkanFunctions.vkBindBufferMemory = vkBindBufferMemory;
      vulkanFunctions.vkBindImageMemory = vkBindImageMemory;
      vulkanFunctions.vkGetBufferMemoryRequirements =
          vkGetBufferMemoryRequirements;
      vulkanFunctions.vkGetImageMemoryRequirements =
          vkGetImageMemoryRequirements;
      vulkanFunctions.vkCreateBuffer = vkCreateBuffer;
      vulkanFunctions.vkDestroyBuffer = vkDestroyBuffer;
      vulkanFunctions.vkCreateImage = vkCreateImage;
      vulkanFunctions.vkDestroyImage = vkDestroyImage;
      vulkanFunctions.vkCmdCopyBuffer = vkCmdCopyBuffer;
      /// Fetch "vkGetBufferMemoryRequirements2" on Vulkan >= 1.1, fetch
      /// "vkGetBufferMemoryRequirements2KHR" when using
      /// VK_KHR_dedicated_allocation extension.
      vulkanFunctions.vkGetBufferMemoryRequirements2KHR =
          vkGetBufferMemoryRequirements2KHR;
      /// Fetch "vkGetImageMemoryRequirements2" on Vulkan >= 1.1, fetch
      /// "vkGetImageMemoryRequirements2KHR" when using
      /// VK_KHR_dedicated_allocation extension.
      vulkanFunctions.vkGetImageMemoryRequirements2KHR =
          vkGetImageMemoryRequirements2KHR;
      /// Fetch "vkBindBufferMemory2" on Vulkan >= 1.1, fetch
      /// "vkBindBufferMemory2KHR" when using VK_KHR_bind_memory2 extension.
      vulkanFunctions.vkBindBufferMemory2KHR = vkBindBufferMemory2KHR;
      /// Fetch "vkBindImageMemory2" on Vulkan >= 1.1, fetch
      /// "vkBindImageMemory2KHR" when using VK_KHR_bind_memory2 extension.
      vulkanFunctions.vkBindImageMemory2KHR = vkBindImageMemory2KHR;
      /// Fetch from "vkGetPhysicalDeviceMemoryProperties2" on Vulkan >= 1.1,
      /// but you can also fetch it from
      /// "vkGetPhysicalDeviceMemoryProperties2KHR" if you enabled extension
      /// VK_KHR_get_physical_device_properties2.
      vulkanFunctions.vkGetPhysicalDeviceMemoryProperties2KHR =
          vkGetPhysicalDeviceMemoryProperties2KHR;
      /// Fetch from "vkGetDeviceBufferMemoryRequirements" on Vulkan >= 1.3, but
      /// you can also fetch it from "vkGetDeviceBufferMemoryRequirementsKHR" if
      /// you enabled extension VK_KHR_maintenance4.
      vulkanFunctions.vkGetDeviceBufferMemoryRequirements =
          vkGetDeviceBufferMemoryRequirements;
      /// Fetch from "vkGetDeviceImageMemoryRequirements" on Vulkan >= 1.3, but
      /// you can also fetch it from "vkGetDeviceImageMemoryRequirementsKHR" if
      /// you enabled extension VK_KHR_maintenance4.
      vulkanFunctions.vkGetDeviceImageMemoryRequirements =
          vkGetDeviceImageMemoryRequirements;

      VmaAllocatorCreateInfo createInfo = {0};
      createInfo.physicalDevice = device->physicalAdapter.vk.physicalDevice;
      createInfo.device = device->vk.device;
      createInfo.instance = g_renderer.vk.instance;
      createInfo.pVulkanFunctions = &vulkanFunctions;
      createInfo.vulkanApiVersion = VK_API_VERSION_1_3;

      if (device->physicalAdapter.vk.isBufferDeviceAddressSupported) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
      }

      if (device->vk.deviceCoherentMemoryEnabled) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_AMD_DEVICE_COHERENT_MEMORY_BIT;
      }

      result = vmaCreateAllocator(&createInfo, &device->vk.vmaAllocator);
      if (!VK_WrapResult(result)) {
        riResult = RI_FAIL;
        goto vk_done;
      }
    }

  vk_done:
    free(queueFamilyProps);
    free(extensionProperties);
    arrfree(enabledExtensionNames);
  }
#endif
  return riResult;
}

int InitRIRenderer(const struct RIBackendInit *init) {
  memset(&g_renderer, 0, sizeof(g_renderer));
  g_renderer.api = init->api;
#if (DEVICE_IMPL_VULKAN)
  {
    volkInitialize();

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pNext = NULL;
    appInfo.pApplicationName = init->applicationName;
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "HPL2";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    g_renderer.vk.apiVersion = appInfo.apiVersion;

    // GPU-Assisted Validation was originally enabled here to localize a GPUVM
    // read fault on the since-retired surfel GI branch (TCP client, RW:0, high
    // BDA-range address); the setting outlived that branch. With GPU-AV
    // on, the Khronos layer instruments shader buffer access — including
    // buffer_reference (BDA) derefs and descriptor-indexed reads — and emits a
    // precise ERROR naming the faulting shader + access BEFORE it becomes a
    // hardware page fault. RESERVE_BINDING_SLOT keeps GPU-AV's internal
    // descriptor set from colliding with the bindless set. Drop GPU_ASSISTED +
    // RESERVE_BINDING_SLOT once the offending access is fixed (it adds notable
    // per-dispatch overhead).
    // NOTE: DEBUG_PRINTF is intentionally omitted while GPU-AV is on — on most
    // Khronos layer versions the two are mutually exclusive and enabling both
    // makes the layer silently disable GPU-AV. debugPrintfEXT calls in shaders
    // still work under unified GPU-AV and no-op harmlessly otherwise. Restore
    // DEBUG_PRINTF (and drop the two GPU_ASSISTED entries) when done debugging.
    // GPU-assisted validation DISABLED: its TLAS-instance check false-rejects a
    // valid BLAS device address as an invalid acceleration-structure reference
    // (VUID-12281) and aborts, even though the address is a real AS device
    // address (verified: it equals the BLAS storage buffer's device address,
    // and BLAS→TLAS ordering was ruled out via a dedicated, semaphore-synced
    // BLAS command buffer). Core CPU validation stays on. Re-enable by
    // restoring the two GPU_ASSISTED entries and the count below.
    // const VkValidationFeatureEnableEXT enabledValidationFeatures[] = {
    // 	VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
    // 	VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
    // };

    VkValidationFeaturesEXT validationFeatures = {
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 0;
    validationFeatures.pEnabledValidationFeatures = NULL;

    // Chained into VkInstanceCreateInfo::pNext so the validation layer has
    // an INFO-severity-capable messenger during vkCreateInstance itself.
    // Without this, debug-printf output emitted during instance creation
    // (and the validation layer's own setup messages) is lost, and the
    // layer prints a hint warning that INFO logging is not enabled.
    VkDebugUtilsMessengerCreateInfoEXT instanceDebugCreateInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    instanceDebugCreateInfo.pfnUserCallback = __VK_DebugUtilsMessenger;
    instanceDebugCreateInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    instanceDebugCreateInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

    VkInstanceCreateInfo instanceCreateInfo = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceCreateInfo.pApplicationInfo = &appInfo;
    const char *enabledLayerNames[8] = {0};
    const char **enabledExtensionNames = NULL;
    instanceCreateInfo.ppEnabledLayerNames = enabledLayerNames;
    instanceCreateInfo.enabledLayerCount = 0;
    instanceCreateInfo.ppEnabledExtensionNames = enabledExtensionNames;
    instanceCreateInfo.enabledExtensionCount = 0;

    VkLayerProperties *layerProperties = NULL;
    VkExtensionProperties *extProperties = NULL;
    uint32_t extensionNum = 0;
    {
      assert(1 <= ARRAY_COUNT(enabledLayerNames));
      uint32_t enumInstanceLayers = 0;
      vkEnumerateInstanceLayerProperties(&enumInstanceLayers, NULL);
      layerProperties = (VkLayerProperties *)malloc(enumInstanceLayers *
                                                    sizeof(VkLayerProperties));
      vkEnumerateInstanceLayerProperties(&enumInstanceLayers, layerProperties);
      for (size_t i = 0; i < enumInstanceLayers; i++) {
        bool useLayer = false;
        useLayer |= (init->vk.enableValidationLayer &&
                     strcmp(layerProperties[i].layerName,
                            "VK_LAYER_KHRONOS_validation") == 0);
        hpl::Log("Instance Layer: %s(%d): %s\n", layerProperties[i].layerName,
                 layerProperties[i].specVersion,
                 useLayer ? "ENABLED" : "DISABLED");
        if (useLayer) {
          assert(instanceCreateInfo.enabledLayerCount <
                 ARRAY_COUNT(enabledLayerNames));
          enabledLayerNames[instanceCreateInfo.enabledLayerCount++] =
              layerProperties[i].layerName;
        }
      }
    }
    {
      vkEnumerateInstanceExtensionProperties(NULL, &extensionNum, NULL);
      extProperties = (VkExtensionProperties *)malloc(
          extensionNum * sizeof(VkExtensionProperties));
      vkEnumerateInstanceExtensionProperties(NULL, &extensionNum,
                                             extProperties);

      const bool supportSurfaceExtension = __VK_isExtensionSupported(
          VK_KHR_SURFACE_EXTENSION_NAME, extProperties, extensionNum);
      for (size_t i = 0; i < extensionNum; i++) {
        bool useExtension = false;

        if (supportSurfaceExtension) {
#ifdef VK_USE_PLATFORM_WIN32_KHR
          useExtension |= (strcmp(extProperties[i].extensionName,
                                  VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0);
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
          useExtension |= (strcmp(extProperties[i].extensionName,
                                  VK_EXT_METAL_SURFACE_EXTENSION_NAME) == 0);
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
          useExtension |= (strcmp(extProperties[i].extensionName,
                                  VK_KHR_XLIB_SURFACE_EXTENSION_NAME) == 0);
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
          useExtension |= (strcmp(extProperties[i].extensionName,
                                  VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0);
#endif
        }
        useExtension |= (strcmp(extProperties[i].extensionName,
                                VK_KHR_SURFACE_EXTENSION_NAME) == 0);
        useExtension |=
            (strcmp(extProperties[i].extensionName,
                    VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0);
        useExtension |= (strcmp(extProperties[i].extensionName,
                                VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0);
        hpl::Log("Instance Extensions: %s(%d): %s\n",
                 extProperties[i].extensionName, extProperties[i].specVersion,
                 useExtension ? "ENABLED" : "DISABLED");
        if (useExtension) {
          arrpush(enabledExtensionNames, extProperties[i].extensionName);
          instanceCreateInfo.enabledExtensionCount++;
        }
      }
    }

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
    {
      hpl::cXessVulkanSupport &xessSupport =
          hpl::XessVulkanSupportInstance();
      if (!xessSupport.IsAvailable()) {
        hpl::Log("XeSS: instance preflight unavailable: %s\n",
                 xessSupport.UnavailableReason());
      } else {
        const auto getRequiredInstanceExtensions =
            xessSupport.VKGetRequiredInstanceExtensions();
        if (!getRequiredInstanceExtensions) {
          xessSupport.SetUnavailable(
              "required instance query entry point is unavailable");
          hpl::Log("XeSS: %s\n", xessSupport.UnavailableReason());
        } else {
        uint32_t requiredExtensionCount = 0;
        uint32_t minVkApiVersion = 0;
        const char *const *requiredExtensionNames = NULL;
        xess_result_t xessResult =
            getRequiredInstanceExtensions(
                &requiredExtensionCount, &requiredExtensionNames,
                &minVkApiVersion);
        if (xessResult != XESS_RESULT_SUCCESS) {
          char reason[128];
          snprintf(reason, sizeof(reason),
                   "required instance extension query failed (%d)",
                   (int)xessResult);
          xessSupport.SetUnavailable(reason);
          hpl::Log("XeSS: %s\n", xessSupport.UnavailableReason());
        } else if (requiredExtensionCount && !requiredExtensionNames) {
          xessSupport.SetUnavailable(
              "required instance extension query returned no names");
          hpl::Log("XeSS: %s\n", xessSupport.UnavailableReason());
        } else if (minVkApiVersion > appInfo.apiVersion) {
          char reason[128];
          snprintf(reason, sizeof(reason),
                   "required Vulkan API version %u exceeds requested %u",
                   minVkApiVersion, appInfo.apiVersion);
          xessSupport.SetUnavailable(reason);
          hpl::Log("XeSS: %s\n", xessSupport.UnavailableReason());
        } else {
          bool instanceExtensionsSupported = true;
          for (uint32_t extensionIdx = 0;
               extensionIdx < requiredExtensionCount; extensionIdx++) {
            const char *requiredName = requiredExtensionNames[extensionIdx];
            if (!requiredName ||
                !__VK_isExtensionSupported(requiredName, extProperties,
                                            extensionNum)) {
              char reason[128];
              snprintf(reason, sizeof(reason),
                       "required instance extension unsupported: %s",
                       requiredName ? requiredName : "<null>");
              xessSupport.SetUnavailable(reason);
              hpl::Log("XeSS: %s\n", xessSupport.UnavailableReason());
              instanceExtensionsSupported = false;
              break;
            }
          }
          if (instanceExtensionsSupported) {
            for (uint32_t extensionIdx = 0;
                 extensionIdx < requiredExtensionCount; extensionIdx++) {
              const char *requiredName = requiredExtensionNames[extensionIdx];
              if (!__VK_isExtensionNamesSupported(
                      qCToStrRef(requiredName), enabledExtensionNames,
                      arrlen(enabledExtensionNames))) {
                arrpush(enabledExtensionNames, requiredName);
                instanceCreateInfo.enabledExtensionCount++;
                hpl::Log("XeSS: enabled instance extension %s\n", requiredName);
              }
            }
            hpl::Log("XeSS: instance requirements accepted\n");
          }
        }
        }
      }
    }
#endif

    // stb_ds may relocate the extension array while the instance extensions
    // are being enumerated and while XeSS requirements are merged.
    instanceCreateInfo.ppEnabledExtensionNames = enabledExtensionNames;
    if (init->vk.enableValidationLayer) {
      R_VK_ADD_STRUCT(&instanceCreateInfo, &validationFeatures);
      R_VK_ADD_STRUCT(&instanceCreateInfo, &instanceDebugCreateInfo);
    }

    VkResult result = vkCreateInstance(&instanceCreateInfo, NULL, &g_renderer.vk.instance);
    free(layerProperties);
    free(extProperties);
    arrfree(enabledExtensionNames);
    if (!VK_WrapResult(result)) {
      return RI_FAIL;
    }
    volkLoadInstance(g_renderer.vk.instance);
    if (init->vk.enableValidationLayer && vkCreateDebugUtilsMessengerEXT) {
      VkDebugUtilsMessengerCreateInfoEXT createInfo = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      createInfo.pUserData = &g_renderer;
      createInfo.pfnUserCallback = __VK_DebugUtilsMessenger;

      createInfo.messageSeverity =
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
      createInfo.messageSeverity |=
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

      createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
      createInfo.messageType |= VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      vkCreateDebugUtilsMessengerEXT(g_renderer.vk.instance, &createInfo, NULL,
                                     &g_renderer.vk.debugMessageUtils);
    }
  }
#endif
  return RI_SUCCESS;
}

// ---- Owned sampler --------------------------------------------------------
void RISampler::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vk.sampler)
      vkDestroySampler(device->vk.device, vk.sampler, NULL);
    vk.sampler = VK_NULL_HANDLE;
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    if (mtl.sampler)
      mtl.sampler->release();
    mtl.sampler = nullptr;
    return;
  }
#endif
}

// ---- RIDescriptor backend handle accessors --------------------------------
// Read the handle resolved into the inline vk union at build time.
#if (DEVICE_IMPL_VULKAN)
VkImageView RIDescriptor::vkImageView() const { return vk.image.imageView; }
VkBuffer RIDescriptor::vkBuffer() const { return vk.buffer.buffer; }
VkSampler RIDescriptor::vkSampler() const { return vk.image.sampler; }
VkAccelerationStructureKHR RIDescriptor::vkAccel() const {
  return vk.accelStructure;
}
VkImageLayout RIDescriptor::vkLayout() const { return vk.image.imageLayout; }
#endif

// ---- Idiomatic descriptor builders ----------------------------------------
// Each references the RI object + sets the binding params, then assigns the
// caller-fed `cookie` 1:1. No resolution / ownership: the bind paths pull the
// backend handle from the referenced RI object via the accessors above. The
// `device` param is unused (kept for call-site stability).
// Fold a resource's identity cookie with the binding parameters into the
// descriptor's cache key. A zero resource cookie (uncreated) stays zero so the
// descriptor reads as empty.
static inline hash_t ri_descriptor_cookie(hash_t resourceCookie, uint8_t type) {
  if (resourceCookie == 0)
    return 0;
  return hash_u64(resourceCookie, type);
}

RIDescriptor RIDescriptor::uniformBuffer(struct RIDevice *device,
                                         struct RIBuffer *buffer,
                                         uint64_t offset, uint64_t range) {
  (void)device;
  RIDescriptor d{};
  d.type = RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  d.vk.buffer = {buffer ? buffer->vk.buffer : VK_NULL_HANDLE, offset, range};
  if (buffer && buffer->cookie)
    d.cookie =
        hash_u64(hash_u64(hash_u64(buffer->cookie, d.type), offset), range);
  return d;
}

RIDescriptor RIDescriptor::storageBuffer(struct RIDevice *device,
                                         struct RIBuffer *buffer,
                                         uint64_t offset, uint64_t range) {
  (void)device;
  RIDescriptor d{};
  d.type = RI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  d.vk.buffer = {buffer ? buffer->vk.buffer : VK_NULL_HANDLE, offset, range};
  if (buffer && buffer->cookie)
    d.cookie =
        hash_u64(hash_u64(hash_u64(buffer->cookie, d.type), offset), range);
  return d;
}

RIDescriptor RIDescriptor::sampledImage(struct RIDevice *device,
                                        struct RITextureView *view,
                                        enum RIResourceState_e state) {
  (void)device;
  RIDescriptor d{};
  d.type = RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  d.vk.image = {VK_NULL_HANDLE, view ? view->vk.image : VK_NULL_HANDLE,
                ri_vk_RIResourceStateToImageLayout(state)};
  if (view && view->cookie)
    d.cookie = hash_u64(hash_u64(view->cookie, d.type), (uint64_t)state);
  return d;
}

RIDescriptor RIDescriptor::storageImage(struct RIDevice *device,
                                        struct RITextureView *view) {
  (void)device;
  RIDescriptor d{};
  d.type = RI_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  d.vk.image = {VK_NULL_HANDLE, view ? view->vk.image : VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_GENERAL};
  d.cookie = ri_descriptor_cookie(view ? view->cookie : 0, d.type);
  return d;
}

RIDescriptor RIDescriptor::accelerationStructure(struct RIDevice *device,
                                                 struct RIAccelStructure *as) {
  (void)device;
  RIDescriptor d{};
  d.type = RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
  d.vk.accelStructure = as ? as->vk.handle : VK_NULL_HANDLE;
  d.cookie = ri_descriptor_cookie(as ? as->cookie : 0, d.type);
  return d;
}

RIDescriptor RIDescriptor::sampler(struct RIDevice *device,
                                   struct RISampler *sampler) {
  (void)device;
  RIDescriptor d{};
  d.type = RI_DESCRIPTOR_TYPE_SAMPLER;
  d.vk.image.sampler = sampler ? sampler->vk.sampler : VK_NULL_HANDLE;
  d.cookie = ri_descriptor_cookie(sampler ? sampler->cookie : 0, d.type);
  return d;
}

void RITexture::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vk.image) {
      if (vk.allocation) {
        vmaDestroyImage(device->vk.vmaAllocator, vk.image, vk.allocation);
        vk.allocation = NULL;
      } else {
        vkDestroyImage(device->vk.device, vk.image, NULL);
      }
      vk.image = NULL;
    }
  }
#endif
}

struct RITexture RITexture::create(struct RIDevice *device,
                                   const struct RITextureDesc &desc,
                                   std::optional<hash_t> hash) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (desc.flags & RI_TEXTURE_FLAG_CUBE_COMPATIBLE)
      info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (desc.flags & RI_TEXTURE_FLAG_BLOCK_TEXEL_VIEW_COMPATIBLE)
      info.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    info.imageType = ri_vk_RITextureTypeToVKImageType(desc.type);
    info.format = RIFormatToVK(desc.format);
    info.extent = {desc.width, desc.height, desc.depth ? desc.depth : 1};
    info.mipLevels = desc.mipNum ? desc.mipNum : 1;
    info.arrayLayers = desc.layerNum ? desc.layerNum : 1;
    info.samples =
        (VkSampleCountFlagBits)(desc.sampleCount ? desc.sampleCount : 1);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.pQueueFamilyIndices = queueFamilies;
    VK_ConfigureImageQueueFamilies(&info, device->queues, RI_QUEUE_LEN,
                                   queueFamilies, RI_QUEUE_LEN);
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = ri_vk_RITextureUsageToVK(desc.usage);
    VmaAllocationCreateInfo memReqs = {};
    memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    RITexture tex = {};
    VK_WrapResult(vmaCreateImage(device->vk.vmaAllocator, &info, &memReqs,
                                 &tex.vk.image, &tex.vk.allocation, NULL));
    tex.cookie = hash.value_or(hash_random());
    return tex;
  }
#endif
  assert(false && "unhandled backend");
  return RITexture{};
}

struct RITextureView RITextureView::create(struct RIDevice *device,
                                           const struct RITexture *tex,
                                           const struct RITextureViewDesc &desc,
                                           std::optional<hash_t> hash) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    const struct RIFormatProps *props = GetRIFormatProps(desc.format);
    // A sampled (shader-resource) view of a depth/stencil image must select a
    // SINGLE aspect — Vulkan forbids sampling a combined DEPTH|STENCIL view. So
    // an SRV over a D32S8 image (e.g. the soft-particle scene-depth read) gets
    // the depth aspect only; attachment views keep the combined aspect (the
    // LuxEffect outline pass binds the depth view as a stencil attachment).
    const bool isShaderResourceView =
        desc.viewType < RI_VIEWTYPE_COLOR_ATTACHMENT;
    VkImageAspectFlags aspect;
    if (props->isDepth) {
      aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
      if (props->isStencil && !isShaderResourceView)
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    } else {
      aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = tex->vk.image;
    ci.viewType = ri_vk_RITextureViewTypeToVK(desc.viewType);
    ci.format = RIFormatToVK(desc.format);
    ci.subresourceRange.aspectMask = aspect;
    ci.subresourceRange.baseMipLevel = desc.baseMip;
    ci.subresourceRange.levelCount =
        desc.mipNum ? desc.mipNum : VK_REMAINING_MIP_LEVELS;
    ci.subresourceRange.baseArrayLayer = desc.baseLayer;
    ci.subresourceRange.layerCount =
        desc.layerNum ? desc.layerNum : VK_REMAINING_ARRAY_LAYERS;
    RITextureView view = {};
    VK_WrapResult(
        vkCreateImageView(device->vk.device, &ci, NULL, &view.vk.image));
    view.cookie = hash.value_or(hash_random());
    return view;
  }
#endif
  assert(false && "unhandled backend");
  return RITextureView{};
}

void RITextureView::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vk.image) {
      vkDestroyImageView(device->vk.device, vk.image, NULL);
      vk.image = VK_NULL_HANDLE;
    }
  }
#endif
  memset(this, 0, sizeof(*this));
}

void RIPool::init(struct RIDevice *device, struct RIQueue *queue) {
#if (DEVICE_IMPL_VULKAN)
  {
    VkCommandPoolCreateInfo cmdPoolCreateInfo = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmdPoolCreateInfo.queueFamilyIndex = queue->vk.queueFamilyIdx;
    cmdPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_WrapResult(vkCreateCommandPool(device->vk.device, &cmdPoolCreateInfo,
                                      NULL, &vk.pool));
    vk.queue = queue->vk.queue;
    return;
  }
#endif
  assert(false);
}

void RIPool::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkDestroyCommandPool(device->vk.device, vk.pool, NULL);
    vk.pool = VK_NULL_HANDLE;
    return;
  }
#endif
  assert(false);
}

void RIPool::reset(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  {
    VK_WrapResult(vkResetCommandPool(device->vk.device, vk.pool, 0));
  }
#endif
}

void RICmd::init(struct RIDevice *device, struct RIPool *pool) {
  barrierCapabilities.rayTracingPipelineEnabled =
      device->rayTracingPipelineEnabled;
  barrierCapabilities.accelerationStructureEnabled =
      device->accelerationStructureEnabled;
#if (DEVICE_IMPL_VULKAN)
  {
    VkCommandBufferAllocateInfo command_allocate_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_allocate_info.commandPool = pool->vk.pool;
    command_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate_info.commandBufferCount = 1;
    VK_WrapResult(vkAllocateCommandBuffers(device->vk.device,
                                           &command_allocate_info, &vk.cmd));
    vk.pool = pool->vk.pool;
    return;
  }
#endif
}

void RICmd::begin(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  {
    VkCommandBufferBeginInfo info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_WrapResult(vkBeginCommandBuffer(vk.cmd, &info));
    return;
  }
#endif
}

void RICmd::end(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  {
    VK_WrapResult(vkEndCommandBuffer(vk.cmd));
    return;
  }
#endif
}

void RICommandRingElement::wait(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (vk.fence) {
    VK_WrapResult(
        vkWaitForFences(device->vk.device, 1, &vk.fence, VK_TRUE, UINT64_MAX));
  }
#endif
}

void RICmd::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vk.cmd) {
      vkFreeCommandBuffers(device->vk.device, vk.pool, 1, &vk.cmd);
    }
    vk.cmd = VK_NULL_HANDLE;
    vk.pool = VK_NULL_HANDLE;
  }
#endif
  barrierCapabilities = RIBarrierCapabilities{};
}

void ShutdownRIRenderer() {
#if (DEVICE_IMPL_VULKAN)
  if (g_renderer.vk.debugMessageUtils)
    vkDestroyDebugUtilsMessengerEXT(g_renderer.vk.instance,
                                    g_renderer.vk.debugMessageUtils, NULL);
  vkDestroyInstance(g_renderer.vk.instance, NULL);
  volkFinalize();
#endif
}

void RIQueue::waitIdle(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  VK_WrapResult(vkQueueWaitIdle(vk.queue));
#endif
}

void RIDevice::dispose() {
#if (DEVICE_IMPL_VULKAN)
  if (vk.vmaAllocator) {
    // Leak diagnostic: vmaDestroyAllocator asserts if any allocation survives.
    // Dump what is still live (count/bytes + the detailed per-block JSON, whose
    // sizes identify the offending resource) before the assert fires.
    VmaTotalStatistics stats = {};
    vmaCalculateStatistics(vk.vmaAllocator, &stats);
    if (stats.total.statistics.allocationCount > 0) {
      fprintf(stderr,
              "[VMA LEAK] %u allocation(s) still live at device teardown, "
              "%llu bytes\n",
              stats.total.statistics.allocationCount,
              (unsigned long long)stats.total.statistics.allocationBytes);
      char *json = nullptr;
      vmaBuildStatsString(vk.vmaAllocator, &json, VK_TRUE);
      if (json) {
        fprintf(stderr, "[VMA LEAK] %s\n", json);
        vmaFreeStatsString(vk.vmaAllocator, json);
      }
      fflush(stderr);
    }
    vmaDestroyAllocator(vk.vmaAllocator);
  }
  if (vk.device)
    vkDestroyDevice(vk.device, NULL);

  vk.device = NULL;
  vk.vmaAllocator = NULL;
#endif
}

// =================================================================================================
// Acceleration structures (VK_KHR_acceleration_structure)
// =================================================================================================

#if (DEVICE_IMPL_VULKAN)

// Returns 0 if buffer is NULL. Callers ORing this with an offset get a
// device-side pointer suitable for VkDeviceOrHostAddressConstKHR /
// VkDeviceOrHostAddressKHR.
static VkDeviceAddress RI_VK_BufferDeviceAddress(struct RIDevice *dev,
                                                 struct RIBuffer *buf) {
  if (!buf || buf->vk.buffer == VK_NULL_HANDLE)
    return 0;
  VkBufferDeviceAddressInfo info = {
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = buf->vk.buffer;
  return vkGetBufferDeviceAddress(dev->vk.device, &info);
}

// Fill VkAccelerationStructureGeometryKHR + maxPrimitiveCount from an
// RIAccelGeometryDesc. resolveAddresses=false skips reading buffer device
// addresses (used by the size query, which only needs the geometry layout /
// formats / counts).
static void RI_VK_FillGeometry(struct RIDevice *dev,
                               const struct RIAccelGeometryDesc *src,
                               VkAccelerationStructureGeometryKHR *outGeom,
                               uint32_t *outMaxPrimitiveCount,
                               bool resolveAddresses) {
  memset(outGeom, 0, sizeof(*outGeom));
  outGeom->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  outGeom->flags = RI_VK_AccelGeometryFlags(src->flags);

  switch (src->type) {
  case RI_ACCEL_GEOMETRY_TYPE_TRIANGLES: {
    const struct RIAccelTrianglesDesc *tri = &src->triangles;
    outGeom->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR *t =
        &outGeom->geometry.triangles;
    t->sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t->vertexFormat = RIFormatToVK((uint32_t)tri->vertexFormat);
    t->vertexStride = tri->vertexStride;
    t->maxVertex = tri->vertexNum ? tri->vertexNum - 1 : 0;
    t->indexType = (tri->indexBuffer) ? ri_vk_RIIndexTypeToVK(tri->indexType)
                                      : VK_INDEX_TYPE_NONE_KHR;
    if (resolveAddresses) {
      t->vertexData.deviceAddress =
          tri->vertexBuffer->GetDeviceHandle(dev) + tri->vertexOffset;
      // indexBuffer/transformBuffer are optional: unindexed geometry
      // passes a null indexBuffer (indexType already set to NONE_KHR
      // above), and per-triangle transforms are opt-in (identity if
      // transformData.deviceAddress == 0).
      t->indexData.deviceAddress =
          tri->indexBuffer
              ? tri->indexBuffer->GetDeviceHandle(dev) + tri->indexOffset
              : 0;
      t->transformData.deviceAddress =
          tri->transformBuffer ? tri->transformBuffer->GetDeviceHandle(dev) +
                                     tri->transformOffset
                               : 0;
    }
    // Build range covers triangleCount = indexNum/3 (indexed) or vertexNum/3
    // (unindexed).
    const uint32_t indexCount =
        tri->indexBuffer ? tri->indexNum : tri->vertexNum;
    *outMaxPrimitiveCount = indexCount / 3;
    break;
  }
  case RI_ACCEL_GEOMETRY_TYPE_AABBS: {
    const struct RIAccelAabbsDesc *aab = &src->aabbs;
    outGeom->geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    VkAccelerationStructureGeometryAabbsDataKHR *a = &outGeom->geometry.aabbs;
    a->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    a->stride = aab->stride ? aab->stride : sizeof(struct RIAccelAabb);
    if (resolveAddresses) {
      a->data.deviceAddress =
          RI_VK_BufferDeviceAddress(dev, aab->buffer) + aab->offset;
    }
    *outMaxPrimitiveCount = aab->num;
    break;
  }
  }
}

#endif // DEVICE_IMPL_VULKAN

void RIAccelStructureDesc::getMemoryReqs(struct RIDevice *dev,
                                         uint64_t *outStorageSize,
                                         uint64_t *outBuildScratchSize,
                                         uint64_t *outUpdateScratchSize) const {
#if (DEVICE_IMPL_VULKAN)
  assert(dev);
  const struct RIAccelStructureDesc *desc = this;

  VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  buildInfo.type = RI_VK_AccelStructureType(desc->type);
  buildInfo.flags = RI_VK_AccelBuildFlags(desc->flags);
  buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

  // VkAccelerationStructureBuildSizesInfoKHR scales with maxPrimitiveCounts[];
  // addresses ignored.
  std::vector<VkAccelerationStructureGeometryKHR> geoms;
  std::vector<uint32_t> maxPrims;
  if (desc->type == RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL) {
    geoms.resize(desc->geometryOrInstanceNum);
    maxPrims.resize(desc->geometryOrInstanceNum);
    for (uint32_t i = 0; i < desc->geometryOrInstanceNum; ++i) {
      RI_VK_FillGeometry(dev, &desc->geometries[i], &geoms[i], &maxPrims[i],
                         false);
    }
    buildInfo.geometryCount = (uint32_t)geoms.size();
    buildInfo.pGeometries = geoms.data();
  } else {
    // TLAS: a single instances geometry with maxPrim = instance count.
    geoms.resize(1);
    maxPrims.resize(1);
    memset(&geoms[0], 0, sizeof(geoms[0]));
    geoms[0].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geoms[0].geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geoms[0].geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    maxPrims[0] = desc->geometryOrInstanceNum;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = geoms.data();
  }

  VkAccelerationStructureBuildSizesInfoKHR sizesInfo = {
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(
      dev->vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
      &buildInfo, maxPrims.data(), &sizesInfo);

  //// CmdBuildRI{Blas,Tlas} rounds scratchData.deviceAddress up to
  //// minAccelerationStructureScratchOffsetAlignment, consuming up to
  //// (alignment - 1) bytes off the front of the buffer. Pad the reported
  //// scratch sizes so callers allocate enough headroom to absorb that
  //// round-up regardless of where vmaCreateBuffer landed the base address.
  // const uint64_t scratchPad =
  //	dev->physicalAdapter.accelerationStructureScratchOffsetAlignment > 1
  //		?
  //(uint64_t)dev->physicalAdapter.accelerationStructureScratchOffsetAlignment -
  // 1 		: 0;

  if (outStorageSize)
    *outStorageSize = sizesInfo.accelerationStructureSize;
  if (outBuildScratchSize)
    *outBuildScratchSize = sizesInfo.buildScratchSize;
  if (outUpdateScratchSize)
    *outUpdateScratchSize = sizesInfo.updateScratchSize;
#endif
}

int RIAccelStructure::init(struct RIDevice *device,
                           const struct RIAccelStructureDesc *desc) {
#if (DEVICE_IMPL_VULKAN)
  assert(device);
  assert(desc);
  assert(desc->storage);
  assert(!desc->storage->isEmpty());
  assert(desc->storageSize > 0);

  type = desc->type;
  flags = desc->flags;
  // storageOffset = desc->storageOffset;

  VkAccelerationStructureCreateInfoKHR createInfo = {
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  createInfo.buffer = desc->storage->vk.buffer;
  createInfo.offset = desc->storageOffset;
  createInfo.size = desc->storageSize;
  createInfo.type = RI_VK_AccelStructureType(desc->type);

  VkResult res = vkCreateAccelerationStructureKHR(
      device->vk.device, &createInfo, NULL, &vk.handle);
  if (!VK_WrapResult(res))
    return RI_FAIL;

  VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  addrInfo.accelerationStructure = vk.handle;
  vk.deviceAddress =
      vkGetAccelerationStructureDeviceAddressKHR(device->vk.device, &addrInfo);
  // Globally-unique identity: the backend handle can be reused by a later
  // allocation, which would collide in the descriptor-set cache.
  cookie = hash_random();

  return RI_SUCCESS;
#else
  return RI_FAIL;
#endif
}

uint64_t RIAccelStructure::getDeviceAddress(struct RIDevice *device) const {
#if (DEVICE_IMPL_VULKAN)
  (void)device;
  return vk.deviceAddress;
#else
  return 0;
#endif
}

void RICmd::buildBlas(struct RIDevice *device,
                      const struct RIBuildBlasDesc *descs, uint32_t numDescs) {
  struct RIDevice *dev = device;
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (numDescs == 0)
      return;
    assert(dev);
    assert(descs);

    // Each build needs: a geometry array (one entry per BLAS geometry), a range
    // array (one entry per geometry) and a build-geometry-info that points to
    // both. Vulkan takes parallel arrays: one
    // VkAccelerationStructureBuildGeometryInfoKHR per build, one
    // VkAccelerationStructureBuildRangeInfoKHR* per build.
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(
        numDescs);
    std::vector<std::vector<VkAccelerationStructureGeometryKHR>> geomStorage(
        numDescs);
    std::vector<std::vector<VkAccelerationStructureBuildRangeInfoKHR>>
        rangeStorage(numDescs);
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangePtrs(
        numDescs);

    for (uint32_t i = 0; i < numDescs; ++i) {
      const struct RIBuildBlasDesc *d = &descs[i];
      assert(d->dst);
      assert(
          d->dst->vk.handle !=
          VK_NULL_HANDLE); // dst BLAS must be created (RIAccelStructure::init)
      assert(d->scratchBuffer);
      assert(d->geometryNum > 0);
      assert(d->geometries);

      geomStorage[i].resize(d->geometryNum);
      rangeStorage[i].resize(d->geometryNum);
      for (uint32_t g = 0; g < d->geometryNum; ++g) {
        uint32_t maxPrims = 0;
        RI_VK_FillGeometry(dev, &d->geometries[g], &geomStorage[i][g],
                           &maxPrims, true);
        // A BLAS built over unbound/freed geometry (zero vertex device address
        // or zero primitives) produces an invalid acceleration structure whose
        // device address later trips vkCmdBuildAccelerationStructures when a
        // TLAS instance references it. Catch it at the source instead.
        assert(maxPrims > 0);
        if (geomStorage[i][g].geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR)
          assert(
              geomStorage[i][g].geometry.triangles.vertexData.deviceAddress !=
              0);
        rangeStorage[i][g].primitiveCount = maxPrims;
        rangeStorage[i][g].primitiveOffset = 0;
        rangeStorage[i][g].firstVertex = 0;
        rangeStorage[i][g].transformOffset = 0;
      }
      rangePtrs[i] = rangeStorage[i].data();

      VkAccelerationStructureBuildGeometryInfoKHR *bi = &buildInfos[i];
      bi->sType =
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
      bi->type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      bi->flags = RI_VK_AccelBuildFlags(d->dst->flags);
      bi->mode = (d->mode == RI_ACCEL_BUILD_MODE_UPDATE)
                     ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                     : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      bi->srcAccelerationStructure =
          (d->src ? d->src->vk.handle : VK_NULL_HANDLE);
      bi->dstAccelerationStructure = d->dst->vk.handle;
      bi->geometryCount = d->geometryNum;
      bi->pGeometries = geomStorage[i].data();
      // VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03710:
      // scratchData.deviceAddress must be a multiple of
      // minAccelerationStructureScratchOffsetAlignment. The buffer base address
      // VMA hands back is not guaranteed to satisfy that, so round up.
      {
        const uint64_t scratchAddr =
            RI_VK_BufferDeviceAddress(dev, d->scratchBuffer) + d->scratchOffset;
        assert((scratchAddr %
                dev->physicalAdapter
                    .accelerationStructureScratchOffsetAlignment) == 0);
        bi->scratchData.deviceAddress = scratchAddr;
      }
    }

    vkCmdBuildAccelerationStructuresKHR(vk.cmd, numDescs, buildInfos.data(),
                                        rangePtrs.data());
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    if (numDescs == 0)
      return;
    assert(dev);
    assert(descs);
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    mtl_encoderEnd(); // close any open render/compute/blit encoder first
    mtl_encoderAccel();
    for (uint32_t i = 0; i < numDescs; ++i) {
      const struct RIBuildBlasDesc *d = &descs[i];
      assert(d->dst && d->dst->mtl.handle);
      assert(d->scratchBuffer && d->scratchBuffer->mtl.buffer);
      assert(d->geometryNum > 0 && d->geometries);
      MTL::PrimitiveAccelerationStructureDescriptor *p =
          MTL::PrimitiveAccelerationStructureDescriptor::descriptor();
      p->setGeometryDescriptors(
          RI_MTL_BuildGeometryArray(d->geometries, d->geometryNum));
      p->setUsage(RIToMTLAccelUsage(d->dst->flags));
      mtl.accel->buildAccelerationStructure(d->dst->mtl.handle, p,
                                            d->scratchBuffer->mtl.buffer,
                                            d->scratchOffset);
    }
    mtl_encoderEnd();
    pool->release();
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::buildTlas(struct RIDevice *device,
                      const struct RIBuildTlasDesc *descs, uint32_t numDescs) {
  struct RIDevice *dev = device;
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (numDescs == 0)
      return;
    assert(dev);
    assert(descs);

    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(
        numDescs);
    std::vector<VkAccelerationStructureGeometryKHR> geoms(numDescs);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(numDescs);
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangePtrs(
        numDescs);

    for (uint32_t i = 0; i < numDescs; ++i) {
      const struct RIBuildTlasDesc *d = &descs[i];
      assert(d->dst);
      assert(d->dst->vk.handle != VK_NULL_HANDLE); // dst TLAS must be created
      assert(d->scratchBuffer);
      assert(d->instanceBuffer);
      // instanceNum == 0 is a legal build (HybridRenderer emits an empty
      // TLAS for worlds with no RT geometry — e.g. a fresh editor scene —
      // so the RT descriptor pushes always have a valid handle).

      VkAccelerationStructureGeometryKHR *g = &geoms[i];
      memset(g, 0, sizeof(*g));
      g->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
      g->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
      g->geometry.instances.sType =
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
      g->geometry.instances.arrayOfPointers = VK_FALSE;
      VkDeviceAddress instanceAddress =
          RI_VK_BufferDeviceAddress(dev, d->instanceBuffer);
      assert(instanceAddress != 0);
      g->geometry.instances.data.deviceAddress =
          instanceAddress + d->instanceOffset;

      ranges[i].primitiveCount = d->instanceNum;
      ranges[i].primitiveOffset = 0;
      ranges[i].firstVertex = 0;
      ranges[i].transformOffset = 0;
      rangePtrs[i] = &ranges[i];

      VkAccelerationStructureBuildGeometryInfoKHR *bi = &buildInfos[i];
      bi->sType =
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
      bi->type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
      bi->flags = RI_VK_AccelBuildFlags(d->dst->flags);
      bi->mode = (d->mode == RI_ACCEL_BUILD_MODE_UPDATE)
                     ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                     : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      bi->srcAccelerationStructure =
          (d->src ? d->src->vk.handle : VK_NULL_HANDLE);
      bi->dstAccelerationStructure = d->dst->vk.handle;
      bi->geometryCount = 1;
      bi->pGeometries = g;
      // VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03710 (same as BLAS).
      {
        const uint64_t scratchAlign =
            dev->physicalAdapter.accelerationStructureScratchOffsetAlignment;
        const uint64_t scratchAddr =
            RI_VK_BufferDeviceAddress(dev, d->scratchBuffer) + d->scratchOffset;
        bi->scratchData.deviceAddress =
            (scratchAlign > 1)
                ? ((scratchAddr + scratchAlign - 1) & ~(scratchAlign - 1))
                : scratchAddr;
        // VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03710 (matches the
        // BLAS assert).
        assert(scratchAlign == 0 ||
               (bi->scratchData.deviceAddress % scratchAlign) == 0);
      }
    }

    vkCmdBuildAccelerationStructuresKHR(vk.cmd, numDescs, buildInfos.data(),
                                        rangePtrs.data());
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    if (numDescs == 0)
      return;
    assert(dev);
    assert(descs);
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    mtl_encoderEnd();
    mtl_encoderAccel();
    for (uint32_t i = 0; i < numDescs; ++i) {
      const struct RIBuildTlasDesc *d = &descs[i];
      assert(d->dst && d->dst->mtl.handle);
      assert(d->scratchBuffer && d->scratchBuffer->mtl.buffer);
      assert(d->instanceBuffer && d->instanceBuffer->mtl.buffer);

      // instancedAccelerationStructures: the BLASes that instances index into
      // via their accelerationStructureIndex (set by RI_WriteAccelInstance).
      NS::Array *blasArr = nullptr;
      if (d->instanceBlasNum) {
        std::vector<NS::Object *> blasObjs(d->instanceBlasNum);
        for (uint32_t b = 0; b < d->instanceBlasNum; ++b)
          blasObjs[b] = (NS::Object *)d->instanceBlases[b]->mtl.handle;
        blasArr = NS::Array::array((const NS::Object *const *)blasObjs.data(),
                                   d->instanceBlasNum);
      }

      MTL::InstanceAccelerationStructureDescriptor *in =
          MTL::InstanceAccelerationStructureDescriptor::descriptor();
      in->setInstanceCount(d->instanceNum);
      in->setInstanceDescriptorType(
          MTL::AccelerationStructureInstanceDescriptorTypeUserID);
      in->setInstanceDescriptorBuffer(d->instanceBuffer->mtl.buffer);
      in->setInstanceDescriptorBufferOffset(d->instanceOffset);
      in->setInstanceDescriptorStride(
          sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor));
      in->setInstancedAccelerationStructures(blasArr);
      in->setUsage(RIToMTLAccelUsage(d->dst->flags));
      mtl.accel->buildAccelerationStructure(d->dst->mtl.handle, in,
                                            d->scratchBuffer->mtl.buffer,
                                            d->scratchOffset);
    }
    mtl_encoderEnd();
    pool->release();
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::dispatch(struct RIDevice *device, uint32_t groupCountX,
                     uint32_t groupCountY, uint32_t groupCountZ) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdDispatch(vk.cmd, groupCountX, groupCountY, groupCountZ);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    assert(mtl.compute);
    // groupCount* are threadgroup counts (Vulkan semantics).
    // threadsPerThreadgroup is the shader's [numthreads], stashed by
    // bindComputePipeline from RIComputePipelineDesc::numThreads (0 => legacy
    // 8x8x1 fallback).
    const uint16_t tx = mtl.threadsPerThreadgroup[0];
    const uint16_t ty = mtl.threadsPerThreadgroup[1];
    const uint16_t tz = mtl.threadsPerThreadgroup[2];
    MTL::Size groups = MTL::Size::Make(groupCountX, groupCountY, groupCountZ);
    MTL::Size threadsPerGroup =
        (tx == 0 && ty == 0 && tz == 0)
            ? MTL::Size::Make(8, 8, 1)
            : MTL::Size::Make(tx ? tx : 1, ty ? ty : 1, tz ? tz : 1);
    mtl.compute->dispatchThreadgroups(groups, threadsPerGroup);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::dispatchIndirect(struct RIDevice *device, struct RIBuffer *buffer,
                             RIDeviceSize offset) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdDispatchIndirect(vk.cmd, buffer->vk.buffer, offset);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    assert(mtl.compute);
    // threadsPerThreadgroup is the bound pipeline's [numthreads] (set by
    // bindComputePipeline); the threadgroup count comes from the indirect
    // buffer. Same 0 => 8x8x1 fallback as dispatch().
    const uint16_t tx = mtl.threadsPerThreadgroup[0];
    const uint16_t ty = mtl.threadsPerThreadgroup[1];
    const uint16_t tz = mtl.threadsPerThreadgroup[2];
    MTL::Size threadsPerGroup =
        (tx == 0 && ty == 0 && tz == 0)
            ? MTL::Size::Make(8, 8, 1)
            : MTL::Size::Make(tx ? tx : 1, ty ? ty : 1, tz ? tz : 1);
    mtl.compute->dispatchThreadgroups(buffer->mtl.buffer, offset,
                                      threadsPerGroup);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::draw(struct RIDevice *device, uint32_t vertexCount,
                 uint32_t instanceCount, uint32_t firstVertex,
                 uint32_t firstInstance) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdDraw(vk.cmd, vertexCount, instanceCount, firstVertex, firstInstance);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    // Goes through the open render encoder; primitiveType is set by
    // RIProgram::bindPipeline.
    assert(mtl.render);
    mtl.render->drawPrimitives(
        mtl.primitiveType, (NS::UInteger)firstVertex, (NS::UInteger)vertexCount,
        (NS::UInteger)instanceCount, (NS::UInteger)firstInstance);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::drawIndexed(struct RIDevice *device, uint32_t indexCount,
                        uint32_t instanceCount, uint32_t firstIndex,
                        int32_t vertexOffset, uint32_t firstInstance) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdDrawIndexed(vk.cmd, indexCount, instanceCount, firstIndex,
                     vertexOffset, firstInstance);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    // Metal binds the index buffer at draw time; bindIndexBuffer stashed it.
    // firstIndex is folded into the buffer offset (Metal has no firstIndex
    // arg).
    assert(mtl.render && mtl.indexBuffer);
    const NS::UInteger stride = (mtl.indexType == MTL::IndexTypeUInt16) ? 2 : 4;
    mtl.render->drawIndexedPrimitives(
        mtl.primitiveType, (NS::UInteger)indexCount, mtl.indexType,
        mtl.indexBuffer,
        mtl.indexBufferOffset + (NS::UInteger)firstIndex * stride,
        (NS::UInteger)instanceCount, (NS::Integer)vertexOffset,
        (NS::UInteger)firstInstance);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::drawIndirect(struct RIDevice *device, struct RIBuffer *buffer,
                         RIDeviceSize offset, uint32_t drawCount,
                         uint32_t stride) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdDrawIndirect(vk.cmd, buffer->vk.buffer, offset, drawCount, stride);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    // Metal issues one indirect draw per command; replay drawCount times,
    // advancing by stride (Vulkan's multi-draw semantics).
    assert(mtl.render);
    for (uint32_t i = 0; i < drawCount; ++i)
      mtl.render->drawPrimitives(
          mtl.primitiveType, buffer->mtl.buffer,
          (NS::UInteger)(offset + (RIDeviceSize)i * stride));
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::drawIndirectCount(struct RIDevice *device, struct RIBuffer *buffer,
                             RIDeviceSize offset, struct RIBuffer *countBuffer,
                             RIDeviceSize countOffset, uint32_t maxDrawCount,
                             uint32_t stride) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    assert(device->physicalAdapter.isDrawIndirectCountSupported &&
           "drawIndirectCount used on a device that does not support it");
    vkCmdDrawIndirectCount(vk.cmd, buffer->vk.buffer, offset,
                           countBuffer->vk.buffer, countOffset, maxDrawCount,
                           stride);
    return;
  }
#endif
  // Deliberately no Metal path: a GPU-sourced draw count needs an indirect
  // command buffer there, which this layer does not model. The capability bit
  // is only ever set on the Vulkan path, so a caller that honours it never
  // reaches here.
  (void)buffer;
  (void)offset;
  (void)countBuffer;
  (void)countOffset;
  (void)maxDrawCount;
  (void)stride;
  assert(false && "drawIndirectCount unsupported on this backend");
}

void RICmd::drawIndexedIndirect(struct RIDevice *device,
                                struct RIBuffer *buffer, RIDeviceSize offset,
                                uint32_t drawCount, uint32_t stride) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdDrawIndexedIndirect(vk.cmd, buffer->vk.buffer, offset, drawCount,
                             stride);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    assert(mtl.render && mtl.indexBuffer);
    for (uint32_t i = 0; i < drawCount; ++i)
      mtl.render->drawIndexedPrimitives(
          mtl.primitiveType, mtl.indexType, mtl.indexBuffer,
          mtl.indexBufferOffset, buffer->mtl.buffer,
          (NS::UInteger)(offset + (RIDeviceSize)i * stride));
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::bindIndexBuffer(struct RIDevice *device, struct RIBuffer *buffer,
                            RIDeviceSize offset, enum RIIndexType_e indexType) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdBindIndexBuffer(vk.cmd, buffer->vk.buffer, offset,
                         ri_vk_RIIndexTypeToVK(indexType));
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    // No Metal bind call; stash for the next drawIndexed/drawIndexedIndirect.
    mtl.indexBuffer = buffer->mtl.buffer;
    mtl.indexBufferOffset = offset;
    mtl.indexType = RIToMTLIndexType(indexType);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::copyBuffer(struct RIDevice *device, struct RIBuffer *src,
                       RIDeviceSize srcOffset, struct RIBuffer *dst,
                       RIDeviceSize dstOffset, RIDeviceSize size) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkBufferCopy region = {};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(vk.cmd, src->vk.buffer, dst->vk.buffer, 1, &region);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    mtl_encoderBlit();
    mtl.blit->copyFromBuffer(src->mtl.buffer, (NS::UInteger)srcOffset,
                             dst->mtl.buffer, (NS::UInteger)dstOffset,
                             (NS::UInteger)size);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::copyBufferToTexture(struct RIDevice *device, struct RIBuffer *src,
                                struct RITexture *dst,
                                const struct RIBufferTextureCopyDesc &desc) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkBufferImageCopy region = {};
    region.bufferOffset = desc.bufferOffset;
    region.bufferRowLength = desc.bufferRowLength;
    region.bufferImageHeight = desc.bufferImageHeight;
    region.imageOffset.x = desc.x;
    region.imageOffset.y = desc.y;
    region.imageOffset.z = desc.z;
    region.imageExtent.width = desc.width;
    region.imageExtent.height = desc.height;
    region.imageExtent.depth = desc.depth;
    region.imageSubresource.mipLevel = desc.mipLevel;
    region.imageSubresource.baseArrayLayer = desc.arrayLayer;
    region.imageSubresource.layerCount = 1;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(vk.cmd, src->vk.buffer, dst->vk.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    mtl_encoderBlit();
    mtl.blit->copyFromBuffer(
        src->mtl.buffer, (NS::UInteger)desc.bufferOffset,
        (NS::UInteger)desc.bytesPerRow, (NS::UInteger)desc.bytesPerImage,
        MTL::Size::Make(desc.width, desc.height, desc.depth), dst->mtl.texture,
        (NS::UInteger)desc.arrayLayer, (NS::UInteger)desc.mipLevel,
        MTL::Origin::Make(desc.x, desc.y, desc.z));
    return;
  }
#endif
  assert(false && "unhandled backend");
}

// [vk/mtl] Image-to-image 1:1 region copy. On Metal the caller must have closed
// any conflicting render/compute encoder via mtl_encoderEnd() first.
void RICmd::copyImage(struct RIDevice *device, struct RITexture *src,
                      struct RITexture *dst,
                      const struct RIImageCopyDesc &desc) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, desc.srcMipLevel,
                             desc.srcArrayLayer, 1};
    region.srcOffset = {desc.srcX, desc.srcY, desc.srcZ};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, desc.dstMipLevel,
                             desc.dstArrayLayer, 1};
    region.dstOffset = {desc.dstX, desc.dstY, desc.dstZ};
    region.extent = {desc.width, desc.height, desc.depth};
    vkCmdCopyImage(vk.cmd, src->vk.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->vk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    mtl_encoderBlit();
    mtl.blit->copyFromTexture(
        src->mtl.texture, (NS::UInteger)desc.srcArrayLayer,
        (NS::UInteger)desc.srcMipLevel,
        MTL::Origin::Make(desc.srcX, desc.srcY, desc.srcZ),
        MTL::Size::Make(desc.width, desc.height, desc.depth), dst->mtl.texture,
        (NS::UInteger)desc.dstArrayLayer, (NS::UInteger)desc.dstMipLevel,
        MTL::Origin::Make(desc.dstX, desc.dstY, desc.dstZ));
    return;
  }
#endif
  assert(false && "unhandled backend");
}

// [vk/mtl] Clear a storage image (full color subresource, GENERAL layout).
void RICmd::clearStorageImage(struct RIDevice *device, struct RITexture *image,
                              const float color[4]) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkClearColorValue clr = {};
    memcpy(clr.float32, color, sizeof(float) * 4);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(vk.cmd, image->vk.image, VK_IMAGE_LAYOUT_GENERAL, &clr,
                         1, &range);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    // Metal has no direct storage-image clear; fill via a tiny compute kernel
    // built once, lazily, from the command buffer's device.
    assert(mtl.cmd && image->mtl.texture);
    static MTL::ComputePipelineState *clearPipeline = nullptr;
    if (!clearPipeline) {
      const char *kSrc =
          "#include <metal_stdlib>\n"
          "using namespace metal;\n"
          "kernel void ri_clear_storage(\n"
          "    texture2d<float, access::write> img [[texture(0)]],\n"
          "    constant float4 &color [[buffer(0)]],\n"
          "    uint2 gid [[thread_position_in_grid]]) {\n"
          "  if (gid.x >= img.get_width() || gid.y >= img.get_height()) "
          "return;\n"
          "  img.write(color, gid);\n"
          "}\n";
      MTL::Device *dev = mtl.cmd->device();
      NS::Error *err = nullptr;
      MTL::Library *lib = dev->newLibrary(
          NS::String::string(kSrc, NS::UTF8StringEncoding), nullptr, &err);
      if (!lib) {
        hpl::Error("clearStorageImage: clear kernel compile failed: %s\n",
                   err ? err->localizedDescription()->utf8String() : "unknown");
        assert(false);
        return;
      }
      MTL::Function *fn = lib->newFunction(
          NS::String::string("ri_clear_storage", NS::UTF8StringEncoding));
      clearPipeline = dev->newComputePipelineState(fn, &err);
      fn->release();
      lib->release();
      assert(clearPipeline && "clearStorageImage: pipeline build failed");
    }
    mtl_encoderCompute();
    assert(mtl.compute);
    mtl.compute->setComputePipelineState(clearPipeline);
    mtl.compute->setTexture(image->mtl.texture, 0);
    mtl.compute->setBytes(color, sizeof(float) * 4, 0);
    const NS::UInteger w = image->mtl.texture->width();
    const NS::UInteger h = image->mtl.texture->height();
    MTL::Size groups = MTL::Size::Make((w + 15) / 16, (h + 15) / 16, 1);
    MTL::Size threadsPerGroup = MTL::Size::Make(16, 16, 1);
    mtl.compute->dispatchThreadgroups(groups, threadsPerGroup);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

#if (DEVICE_IMPL_VULKAN)
static inline VkAttachmentLoadOp ri_vk_LoadOp(uint8_t op) {
  switch ((enum RIAttachmentLoadOp_e)op) {
  case RI_ATTACHMENT_LOAD_OP_LOAD:
    return VK_ATTACHMENT_LOAD_OP_LOAD;
  case RI_ATTACHMENT_LOAD_OP_CLEAR:
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
  default:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
}
static inline VkAttachmentStoreOp ri_vk_StoreOp(uint8_t op) {
  return op == RI_ATTACHMENT_STORE_OP_STORE ? VK_ATTACHMENT_STORE_OP_STORE
                                            : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}
#endif
#if (DEVICE_IMPL_MTL)
static inline MTL::LoadAction ri_mtl_LoadOp(uint8_t op) {
  switch ((enum RIAttachmentLoadOp_e)op) {
  case RI_ATTACHMENT_LOAD_OP_LOAD:
    return MTL::LoadActionLoad;
  case RI_ATTACHMENT_LOAD_OP_CLEAR:
    return MTL::LoadActionClear;
  default:
    return MTL::LoadActionDontCare;
  }
}
static inline MTL::StoreAction ri_mtl_StoreOp(uint8_t op) {
  return op == RI_ATTACHMENT_STORE_OP_STORE ? MTL::StoreActionStore
                                            : MTL::StoreActionDontCare;
}
#endif

// [vk/d3d12] Dynamic-rendering scope. Metal uses mtl_encoderDraw /
// mtl_encoderEnd.
void RICmd::vk_d3d12_beginRendering(struct RIDevice *device,
                                    const struct RIBeginRenderingDesc &desc) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkRenderingAttachmentInfo colors[8] = {};
    assert(desc.colorCount <= 8);
    for (uint32_t i = 0; i < desc.colorCount; i++) {
      const struct RIRenderingAttachment &src = desc.colors[i];
      colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      colors[i].imageView = src.view.vk.image;
      colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      colors[i].loadOp = ri_vk_LoadOp(src.loadOp);
      colors[i].storeOp = ri_vk_StoreOp(src.storeOp);
      memcpy(colors[i].clearValue.color.float32, src.clearValue.color,
             sizeof(float) * 4);
    }
    VkRenderingAttachmentInfo depth = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    const bool hasStencil = desc.depthStencil && desc.depthStencil->hasStencil;
    if (desc.depthStencil) {
      depth.imageView = desc.depthStencil->view.vk.image;
      // Writable depth always binds as DEPTH_ATTACHMENT_OPTIMAL (the stencil
      // aspect binds separately below when hasStencil). This matches the
      // RI_RESOURCE_STATE_DEPTH_WRITE barrier mapping and
      // RI_VK_FillDepthAttachment, so the declared layout agrees with the
      // image's barriered layout.
      depth.imageLayout = desc.depthStencil->readOnly
                              ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
      depth.loadOp = ri_vk_LoadOp(desc.depthStencil->loadOp);
      depth.storeOp = ri_vk_StoreOp(desc.depthStencil->storeOp);
      depth.clearValue.depthStencil.depth = desc.depthStencil->clearValue.depth;
      depth.clearValue.depthStencil.stencil =
          desc.depthStencil->clearValue.stencil;
      if (hasStencil) {
        stencil.imageView = depth.imageView;
        stencil.imageLayout = desc.depthStencil->readOnly
                                  ? VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        stencil.loadOp = ri_vk_LoadOp(desc.depthStencil->stencilLoadOp);
        stencil.storeOp = ri_vk_StoreOp(desc.depthStencil->stencilStoreOp);
        stencil.clearValue.depthStencil.stencil =
            desc.depthStencil->clearValue.stencil;
      }
    }
    VkRenderingInfo render = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    render.renderArea.offset = {desc.renderArea.x, desc.renderArea.y};
    render.renderArea.extent = {(uint32_t)desc.renderArea.width,
                                (uint32_t)desc.renderArea.height};
    render.layerCount = 1;
    render.colorAttachmentCount = desc.colorCount;
    render.pColorAttachments = desc.colorCount ? colors : NULL;
    render.pDepthAttachment = desc.depthStencil ? &depth : NULL;
    render.pStencilAttachment = hasStencil ? &stencil : NULL;
    vkCmdBeginRendering(vk.cmd, &render);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    return; // render encoder opened by mtl_encoderDraw defines the scope
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::vk_d3d12_endRendering(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdEndRendering(vk.cmd);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    return; // scope ends with mtl_encoderEnd
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::setViewport(struct RIDevice *device,
                        const struct RIViewport &viewport) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkViewport vp = {viewport.x,      viewport.y,        viewport.width,
                     viewport.height, viewport.depthMin, viewport.depthMax};
    vkCmdSetViewport(vk.cmd, 0, 1, &vp);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    assert(mtl.render);
    MTL::Viewport vp = {viewport.x,      viewport.y,        viewport.width,
                        viewport.height, viewport.depthMin, viewport.depthMax};
    mtl.render->setViewport(vp);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::setScissor(struct RIDevice *device, const struct RIRect &scissor) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkRect2D rect = {{scissor.x, scissor.y},
                     {(uint32_t)scissor.width, (uint32_t)scissor.height}};
    vkCmdSetScissor(vk.cmd, 0, 1, &rect);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (RIIsTargetSelected(RI_DEVICE_API_MTL)) {
    assert(mtl.render);
    MTL::ScissorRect rect = {(NS::UInteger)scissor.x, (NS::UInteger)scissor.y,
                             (NS::UInteger)scissor.width,
                             (NS::UInteger)scissor.height};
    mtl.render->setScissorRect(rect);
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RICmd::vk_d3d12_setPushConstants(struct RIDevice *device,
                                      hpl::RIProgram &program, uint32_t offset,
                                      uint32_t size, const void *data) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vkCmdPushConstants(vk.cmd, program.getPipelineLayout(),
                       program.getPushConstantStageFlags(), offset, size, data);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  // Metal binds the push-constant block at [[buffer(0)]] (setBytes) when the
  // pipeline/draw is recorded — no discrete push command here.
  if (RIIsTargetSelected(RI_DEVICE_API_MTL))
    return;
#endif
  assert(false && "unhandled backend");
}
