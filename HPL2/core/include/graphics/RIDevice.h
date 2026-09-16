#ifndef RI_DEVICE_H
#define RI_DEVICE_H

// Renderer, physical adapter and logical device — the top domain layer
// (mirrors ref_nri/ri_device.h). Owns the device-capability enums, the
// backend-selection helpers (RIIsTargetSelected / RIGetVkInstance) and the
// out-of-line resource isEmpty() definitions (which need RIIsTargetSelected).
// Depends on the prelude + resource leaves + RICommand.h (RIDevice embeds
// RIQueue[] / RIPhysicalAdapter by value) + RIDescriptor.h (the RISampler /
// RIAccelStructure isEmpty() bodies). RIDeviceDesc is used by pointer only
// (defined in RIRenderer.h) so a forward declaration is enough.
#include "graphics/RIPreamble.h"
#include "graphics/RIBuffer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "graphics/RICommand.h"
#include "graphics/RIDescriptor.h"
#include <cassert>
#include <cstring>

struct RIDeviceDesc;

enum RIPresetLevel_e {
  RI_GPU_PRESET_NONE = 0,
  RI_GPU_PRESET_OFFICE,  // This means unsupported
  RI_GPU_PRESET_VERYLOW, // Mostly for mobile GPU
  RI_GPU_PRESET_LOW,
  RI_GPU_PRESET_MEDIUM,
  RI_GPU_PRESET_HIGH,
  RI_GPU_PRESET_ULTRA,
  RI_GPU_PRESET_COUNT
};

enum RIDeviceAPI_e {
  RI_DEVICE_API_UNKNOWN,
  RI_DEVICE_API_VK,
  RI_DEVICE_API_D3D11,
  RI_DEVICE_API_D3D12,
  RI_DEVICE_API_MTL
};

enum RIAdapterType_e {
  RI_ADAPTER_TYPE_OTHER,
  RI_ADAPTER_TYPE_CPU,
  RI_ADAPTER_TYPE_VIRTUAL_GPU,
  RI_ADAPTER_TYPE_INTEGRATED_GPU,
  RI_ADAPTER_TYPE_DISCRETE_GPU,
};

enum RIVendor_e { RI_UNKNOWN, RI_NVIDIA, RI_AMD, RI_INTEL };

struct RIRenderer {
  RIRenderer() { memset(this, 0, sizeof(*this)); }
  uint8_t api; // RIDeviceAPI_e
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t apiVersion;
      VkInstance instance;
      VkDebugUtilsMessengerEXT debugMessageUtils;
    } vk;
#endif
  };
};

// There is only ever one renderer per process. Rather than threading a
// RIRenderer through the API (or exposing a global object), the single instance
// lives at file scope in RIRenderer.cpp and is reached through these top-level
// functions — they are the application's highest-level entry points and assume
// the renderer has been initialized.
int InitRIRenderer(const struct RIBackendInit *init);
int EnumerateRIAdapters(struct RIPhysicalAdapter *adapters,
                        uint32_t *numAdapters);
// Destroys the debug messenger + instance and tears down the loader
// (volkFinalize). Instance-level — the device is normally gone by the time
// this runs.
void ShutdownRIRenderer();

#if DEVICE_MULTI_BACKEND
// Active backend (RIDeviceAPI_e); defined in RIRenderer.cpp. Only needed when
// more than one backend is compiled in (otherwise it is known at compile time).
uint8_t RIActiveBackendApi();
#elif DEVICE_IMPL_VULKAN
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_VK
#elif DEVICE_IMPL_MTL
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_MTL
#elif DEVICE_IMPL_D3D12
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_D3D12
#elif DEVICE_IMPL_D3D11
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_D3D11
#endif

// True when the renderer's active backend matches `targetApi` (RIDeviceAPI_e).
// static inline so single-backend builds fold this to a compile-time constant,
// letting the optimizer drop the dead backend branches at every call site
// (the RICmd Vulkan/Metal paths). Multi-backend builds read the active backend.
static inline bool RIIsTargetSelected(uint8_t targetApi) {
#if DEVICE_MULTI_BACKEND
  return targetApi == RIActiveBackendApi();
#else
  assert(targetApi == RI_ACTIVE_BACKEND_API); // single backend: must match
  (void)targetApi;
  return true;
#endif
}

#if (DEVICE_IMPL_VULKAN)
VkInstance RIGetVkInstance();
#endif

struct RIBackendInit {
  uint8_t api; // RIDeviceAPI_e
  const char *applicationName;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t enableValidationLayer : 1;
      size_t numFilterLayers;
      // Was `const char* filterLayers[];` — a C99 flexible-array member.
      // MSVC rejects FAMs inside an anonymous union when the enclosing
      // struct is stack-allocated (error C2466 in Graphics.cpp::Init).
      // No call site currently writes to this field; if filter-layer
      // support is added later, allocate the array externally and point
      // here.
      const char *const *filterLayers;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
#endif
  };
};

struct RIPhysicalAdapter {
  RIPhysicalAdapter() { memset(this, 0, sizeof(*this)); }
  char name[256];
  uint64_t luid;
  uint64_t videoMemorySize;
  uint64_t systemMemorySize;
  uint32_t deviceId;
  uint8_t vendor;      // RIVendor_e
  uint8_t presetLevel; // RIPresetLevel_e
  uint8_t type;        // RIAdapterType_e

  // Viewports
  uint32_t viewportMaxNum;
  int32_t viewportBoundsRange[2];

  // Attachments
  uint16_t attachmentMaxDim;
  uint16_t attachmentLayerMaxNum;
  uint16_t colorAttachmentMaxNum;

  // Multi-sampling
  uint8_t colorSampleMaxNum;
  uint8_t depthSampleMaxNum;
  uint8_t stencilSampleMaxNum;
  uint8_t zeroAttachmentsSampleMaxNum;
  uint8_t textureColorSampleMaxNum;
  uint8_t textureIntegerSampleMaxNum;
  uint8_t textureDepthSampleMaxNum;
  uint8_t textureStencilSampleMaxNum;
  uint8_t storageTextureSampleMaxNum;

  // Resource dimensions
  uint16_t texture1DMaxDim;
  uint16_t texture2DMaxDim;
  uint16_t texture3DMaxDim;
  uint16_t textureArrayLayerMaxNum;
  uint32_t typedBufferMaxDim;

  // Memory
  uint64_t deviceUploadHeapSize; // ReBAR
  uint32_t memoryAllocationMaxNum;
  uint32_t samplerAllocationMaxNum;
  uint32_t constantBufferMaxRange;
  uint32_t storageBufferMaxRange;
  uint32_t bufferTextureGranularity;
  uint64_t bufferMaxSize;

  // Memory alignment
  uint32_t uploadBufferTextureRowAlignment;
  uint32_t uploadBufferOffsetAlignment;
  uint32_t bufferShaderResourceOffsetAlignment;
  uint32_t constantBufferOffsetAlignment;
  // uint32_t scratchBufferOffsetAlignment;
  // uint32_t shaderBindingTableAlignment;

  // Pipeline layout
  // D3D12 only: rootConstantSize + descriptorSetNum * 4 + rootDescriptorNum * 8
  // <= 256 (see "FitPipelineLayoutSettingsIntoDeviceLimits")
  uint32_t pipelineLayoutDescriptorSetMaxNum;
  uint32_t pipelineLayoutRootConstantMaxSize;
  uint32_t pipelineLayoutRootDescriptorMaxNum;

  // Descriptor set
  uint32_t descriptorSetSamplerMaxNum;
  uint32_t descriptorSetConstantBufferMaxNum;
  uint32_t descriptorSetStorageBufferMaxNum;
  uint32_t descriptorSetTextureMaxNum;
  uint32_t descriptorSetStorageTextureMaxNum;

  // Shader resources
  uint32_t perStageDescriptorSamplerMaxNum;
  uint32_t perStageDescriptorConstantBufferMaxNum;
  uint32_t perStageDescriptorStorageBufferMaxNum;
  uint32_t perStageDescriptorTextureMaxNum;
  uint32_t perStageDescriptorStorageTextureMaxNum;
  uint32_t perStageResourceMaxNum;

  // Vertex shader
  uint32_t vertexShaderAttributeMaxNum;
  uint32_t vertexShaderStreamMaxNum;
  uint32_t vertexShaderOutputComponentMaxNum;

  // Tessellation shaders
  float tessControlShaderGenerationMaxLevel;
  uint32_t tessControlShaderPatchPointMaxNum;
  uint32_t tessControlShaderPerVertexInputComponentMaxNum;
  uint32_t tessControlShaderPerVertexOutputComponentMaxNum;
  uint32_t tessControlShaderPerPatchOutputComponentMaxNum;
  uint32_t tessControlShaderTotalOutputComponentMaxNum;
  uint32_t tessEvaluationShaderInputComponentMaxNum;
  uint32_t tessEvaluationShaderOutputComponentMaxNum;

  // Geometry shader
  uint32_t geometryShaderInvocationMaxNum;
  uint32_t geometryShaderInputComponentMaxNum;
  uint32_t geometryShaderOutputComponentMaxNum;
  uint32_t geometryShaderOutputVertexMaxNum;
  uint32_t geometryShaderTotalOutputComponentMaxNum;

  // Fragment shader
  uint32_t fragmentShaderInputComponentMaxNum;
  uint32_t fragmentShaderOutputAttachmentMaxNum;
  uint32_t fragmentShaderDualSourceAttachmentMaxNum;

  // Compute shader
  uint32_t computeShaderSharedMemoryMaxSize;
  uint32_t computeShaderWorkGroupMaxNum[3];
  uint32_t computeShaderWorkGroupInvocationMaxNum;
  uint32_t computeShaderWorkGroupMaxDim[3];

  // Ray tracing
  uint32_t rayTracingShaderGroupIdentifierSize;
  uint32_t rayTracingShaderTableMaxStride;
  uint32_t rayTracingShaderRecursionMaxDepth;
  uint32_t rayTracingGeometryObjectMaxNum;
  uint32_t accelerationStructureScratchOffsetAlignment;

  // Mesh shaders
  // uint32_t meshControlSharedMemoryMaxSize;
  // uint32_t meshControlWorkGroupInvocationMaxNum;
  // uint32_t meshControlPayloadMaxSize;
  // uint32_t meshEvaluationOutputVerticesMaxNum;
  // uint32_t meshEvaluationOutputPrimitiveMaxNum;
  // uint32_t meshEvaluationOutputComponentMaxNum;
  // uint32_t meshEvaluationSharedMemoryMaxSize;
  // uint32_t meshEvaluationWorkGroupInvocationMaxNum;

  // Precision bits
  uint32_t viewportPrecisionBits;
  uint32_t subPixelPrecisionBits;
  uint32_t subTexelPrecisionBits;
  uint32_t mipmapPrecisionBits;

  // Other
  uint64_t timestampFrequencyHz;
  uint32_t drawIndirectMaxNum;
  float samplerLodBiasMin;
  float samplerLodBiasMax;
  float samplerAnisotropyMax;
  int32_t texelOffsetMin;
  uint32_t texelOffsetMax;
  int32_t texelGatherOffsetMin;
  uint32_t texelGatherOffsetMax;
  uint32_t clipDistanceMaxNum;
  uint32_t cullDistanceMaxNum;
  uint32_t combinedClipAndCullDistanceMaxNum;
  // uint8_t shadingRateAttachmentTileSize;
  // uint8_t shaderModel; // major * 10 + minor

  // Tiers (0 - unsupported)
  // 1 - 1/2 pixel uncertainty region and does not support post-snap degenerates
  // 2 - reduces the maximum uncertainty region to 1/256 and requires post-snap
  // degenerates not be culled 3 - maintains a maximum 1/256 uncertainty region
  // and adds support for inner input coverage, aka "SV_InnerCoverage"
  // uint8_t conservativeRasterTier;

  // 1 - a single sample pattern can be specified to repeat for every pixel
  // ("locationNum / sampleNum" must be 1 in "CmdSetSampleLocations") 2 - four
  // separate sample patterns can be specified for each pixel in a 2x2 grid
  // ("locationNum / sampleNum" can be up to 4 in "CmdSetSampleLocations")
  // uint8_t sampleLocationsTier;

  // 1 - DXR 1.0: full raytracing functionality, except features below
  // 2 - DXR 1.1: adds - ray query, "CmdDispatchRaysIndirect", "GeometryIndex()"
  // intrinsic, additional ray flags & vertex formats
  uint8_t rayTracingTier;

  // 1 - shading rate can be specified only per draw
  // 2 - adds: per primitive shading rate, per "shadingRateAttachmentTileSize"
  // shading rate, combiners, "SV_ShadingRate" support
  // uint8_t shadingRateTier;

  // 1 - unbound arrays with dynamic indexing
  // 2 - D3D12 dynamic resources:
  // https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
  uint8_t bindlessTier;

  // Features
  uint32_t isTextureFilterMinMaxSupported : 1;
  uint32_t isLogicFuncSupported : 1;
  uint32_t isDepthBoundsTestSupported : 1;
  uint32_t isDrawIndirectCountSupported : 1;
  uint32_t isIndependentFrontAndBackStencilReferenceAndMasksSupported : 1;
  // uint32_t isLineSmoothingSupported : 1;
  uint32_t isCopyQueueTimestampSupported : 1;
  // uint32_t isMeshShaderPipelineStatsSupported : 1;
  uint32_t isEnchancedBarrierSupported : 1; // aka - can "Layout" be ignored?
  uint32_t isMemoryTier2Supported
      : 1; // a memory object can support resources from all 3 categories
           // (buffers, attachments, all other textures)
  uint32_t isDynamicDepthBiasSupported : 1;
  // uint32_t isAdditionalShadingRatesSupported : 1;
  uint32_t isViewportOriginBottomLeftSupported : 1;
  uint32_t isRegionResolveSupported : 1;

  // Shader features
  uint32_t isShaderNativeI16Supported : 1;
  uint32_t isShaderNativeF16Supported : 1;
  uint32_t isShaderNativeI32Supported : 1;
  uint32_t isShaderNativeF32Supported : 1;
  uint32_t isShaderNativeI64Supported : 1;
  uint32_t isShaderNativeF64Supported : 1;
  uint32_t isShaderAtomicsI16Supported : 1;
  // uint32_t isShaderAtomicsF16Supported : 1;
  uint32_t isShaderAtomicsI32Supported : 1;
  // uint32_t isShaderAtomicsF32Supported : 1;
  uint32_t isShaderAtomicsI64Supported : 1;
  // uint32_t isShaderAtomicsF64Supported : 1;

  // Emulated features
  uint32_t isDrawParametersEmulationEnabled : 1;

  //// Extensions (unexposed are always supported)
  // uint32_t isSwapChainSupported : 1;	// swapchain Support
  uint32_t isRayQuerySupported
      : 1; // VK_KHR_ray_query / DXR 1.1 inline ray queries
  // uint32_t isMeshShaderSupported : 1; // meshshader support

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t apiVersion;
      VkPhysicalDevice physicalDevice;

      uint32_t isSwapChainSupported : 1; // swapchain Support
      uint32_t isBufferDeviceAddressSupported : 1;
      uint32_t isAMDDeviceCoherentMemorySupported
          : 1; // PHYSICAL support: extension advertised and feature reported
      uint32_t isPresentIDSupported : 1;
      // uint32_t YCbCrExtension : 1;
      // uint32_t FillModeNonSolid : 1;
      // uint32_t KHRRayQueryExtension : 1;
      // uint32_t AMDGCNShaderExtension : 1;
      // uint32_t AMDDrawIndirectCountExtension : 1;
      // uint32_t AMDShaderInfoExtension : 1;
      // uint32_t DescriptorIndexingExtension : 1;
      // uint32_t DynamicRenderingExtension : 1;
      // uint32_t ShaderSampledImageArrayDynamicIndexingSupported : 1;
      // uint32_t BufferDeviceAddressSupported : 1;
      // uint32_t DrawIndirectCountExtension : 1;
      // uint32_t DedicatedAllocationExtension : 1;
      // uint32_t DebugMarkerExtension : 1;
      // uint32_t MemoryReq2Extension : 1;
      // uint32_t FragmentShaderInterlockExtension : 1;
      // uint32_t BufferDeviceAddressExtension : 1;
      uint32_t accelerationStructureExtension : 1;
      uint32_t rayTracingPipelineExtension : 1;
      uint32_t rayQueryExtension : 1;
      // uint32_t ShaderAtomicInt64Extension : 1;
      // uint32_t BufferDeviceAddressFeature : 1;
      // uint32_t ShaderFloatControlsExtension : 1;
      // uint32_t Spirv14Extension : 1;
      uint32_t deferredHostOperationsExtension : 1;
      // uint32_t DeviceFaultExtension : 1;
      // uint32_t DeviceFaultSupported : 1;
      // uint32_t ASTCDecodeModeExtension : 1;
      // uint32_t DeviceMemoryReportExtension : 1;
      // uint32_t AMDBufferMarkerExtension : 1;
      // uint32_t AMDDeviceCoherentMemoryExtension : 1;
      // uint32_t AMDDeviceCoherentMemorySupported : 1;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {

    } mtl;
#endif
  };
};

struct RIDevice {
  RIDevice() { memset(this, 0, sizeof(*this)); }
  // Creates the logical device, queues and VMA allocator on the adapter
  // selected in init->physicalAdapter (RIDeviceDesc lives in RIRenderer.h).
  int init(struct RIDeviceDesc *init);
  void dispose();
  struct RIPhysicalAdapter physicalAdapter;
  struct RIQueue queues[RI_QUEUE_LEN];
  // Provider query consumed by the XeSS upscaler adapter.
  bool xessAvailable;
  char xessUnavailableReason[128];
  // Logical-device state; distinct from physicalAdapter.isRayQuerySupported.
  bool rayTracingEnabled;
  bool accelerationStructureEnabled;
  bool rayTracingPipelineEnabled;
  bool rayQueryEnabled;
  bool fragmentShaderBarycentricEnabled;
  bool shaderInt16Enabled;
  bool shaderFloat16Enabled;
  bool geometryShaderEnabled;
  // Precise occlusion sample counts (Standard billboard halos).
  bool occlusionQueryPreciseEnabled;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t maintenance5Features : 1;
      uint32_t conservaitveRasterTier : 1;
      uint32_t swapchainMutableFormat : 1;
      uint32_t memoryBudget : 1;
      uint32_t deviceCoherentMemoryEnabled
          : 1; // Feature enabled on the logical device, never mere physical availability
      // Extensions submitted to vkCreateDevice (physical advertisement lives
      // in physicalAdapter.vk.*Extension). Raster mode never enables these.
      uint32_t accelerationStructureExtensionEnabled : 1;
      uint32_t rayTracingPipelineExtensionEnabled : 1;
      uint32_t rayQueryExtensionEnabled : 1;
      uint32_t deferredHostOperationsExtensionEnabled : 1;
      uint32_t spirv14ExtensionEnabled : 1;
      uint32_t shaderFloatControlsExtensionEnabled : 1;
      VkDevice device;
      VmaAllocator vmaAllocator;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
#endif
  };
};

inline bool RITexture::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.image == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RIBuffer::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.buffer == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RITextureView::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.image == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RICmd::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.cmd == VK_NULL_HANDLE || vk.pool == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RISampler::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.sampler == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RIAccelStructure::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.handle == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

#endif // RI_DEVICE_H
