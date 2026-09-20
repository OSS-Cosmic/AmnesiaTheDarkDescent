#ifndef RI_DEVICE_H
#define RI_DEVICE_H

// Renderer, physical adapter and logical device — the top domain layer. Owns
// the device-capability enums, the backend-selection helpers
// (RIIsTargetSelected / RIGetVkInstance) and the out-of-line resource
// isEmpty() definitions. Pulls in RICommand.h (RIDevice embeds RIQueue[] /
// RIPhysicalAdapter by value) and RIDescriptor.h (the RISampler /
// RIAccelStructure isEmpty() bodies); RIDeviceDesc is used by pointer only.
#include "graphics/RIPreamble.h"
#include "graphics/RIBuffer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "graphics/RICommand.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RID3D12.h"
#include <cassert>
#include <cstring>

struct RIDeviceDesc;
#if (DEVICE_IMPL_D3D12)
namespace D3D12MA { class Allocator; }
#endif

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
#if (DEVICE_IMPL_D3D12)
    struct {
      IDXGIFactory6 *factory;
      ID3D12Debug *debug;
      uint32_t enableDebugLayer : 1;
      uint32_t enableGpuValidation : 1;
    } d3d12;
#endif
  };
};

// There is only ever one renderer per process: it lives at file scope in
// RIRenderer.cpp and is reached through these top-level entry points rather
// than being threaded through the API or exposed as a global.
int InitRIRenderer(const struct RIBackendInit *init);
int EnumerateRIAdapters(struct RIPhysicalAdapter *adapters,
                        uint32_t *numAdapters);
// Instance-level teardown (VK: debug messenger, instance, volkFinalize). The
// device is normally gone by the time this runs.
void ShutdownRIRenderer();

// True when the renderer has selected a backend and the device pointer for the active
// backend is non-null. Cheap check used by the smoke test.
bool RIDeviceIsValid(const struct RIDevice *device);

#if DEVICE_MULTI_BACKEND
// Active backend (RIDeviceAPI_e); defined in RIRenderer.cpp. Only needed when
// more than one backend is compiled in (otherwise it is known at compile time).
uint8_t RIActiveBackendApi();
#elif DEVICE_IMPL_VULKAN
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_VK
#elif DEVICE_IMPL_D3D12
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_D3D12
#endif

// True when the renderer's active backend matches `targetApi` (RIDeviceAPI_e).
// static inline so single-backend builds fold it to a compile-time constant and
// the optimizer drops the dead backend branch at every call site.
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
      // Externally allocated; no call site writes this today.
      const char *const *filterLayers;
    } vk;
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

  // Pipeline layout. D3D12 root-signature budget:
  // rootConstantSize + descriptorSetNum * 4 + rootDescriptorNum * 8 <= 256.
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

  // Tiers (0 - unsupported)
  // 1 - DXR 1.0: full raytracing functionality, except features below
  // 2 - DXR 1.1: adds ray query, indirect dispatch, "GeometryIndex()"
  // intrinsic, additional ray flags & vertex formats
  uint8_t rayTracingTier;

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
  uint32_t isCopyQueueTimestampSupported : 1;
  uint32_t isEnchancedBarrierSupported : 1; // aka - can "Layout" be ignored?
  uint32_t isMemoryTier2Supported
      : 1; // one memory object can back buffers, attachments and all other
           // textures alike
  uint32_t isDynamicDepthBiasSupported : 1;
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
  uint32_t isShaderAtomicsI32Supported : 1;
  uint32_t isShaderAtomicsI64Supported : 1;

  // Emulated features
  uint32_t isDrawParametersEmulationEnabled : 1;

  // Extensions, in backend-neutral terms. Each backend folds its own extension
  // / feature / SDK prerequisites into these bits while enumerating. Check them
  // before asking for anything in RIDeviceDesc, rather than re-deriving
  // availability from the tier fields above.
  uint32_t isSwapChainSupported : 1; // swapchain Support
  uint32_t isBufferDeviceAddressSupported : 1;
  uint32_t isShaderStorageScalarLayoutSupported
      : 1; // VK scalarBlockLayout / HLSL native packing
  uint32_t isDynamicRenderingSupported
      : 1; // VK 1.3 dynamicRendering / D3D12 OMSetRenderTargets
  uint32_t isRayTracingSupported
      : 1; // acceleration structures + ray tracing pipelines; DXR tier 1
  uint32_t isRayQuerySupported
      : 1; // VK_KHR_ray_query / DXR 1.1 inline ray queries

  // The renderer owns the enumerated IDXGIAdapter4 array for its whole lifetime;
  // RIPhysicalAdapter values only borrow those pointers.
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t apiVersion;
      VkPhysicalDevice physicalDevice;

      uint32_t isAMDDeviceCoherentMemorySupported
          : 1; // PHYSICAL support: extension advertised and feature reported
      uint32_t isPresentIDSupported : 1;
      uint32_t accelerationStructureExtension : 1;
      uint32_t rayTracingPipelineExtension : 1;
      uint32_t rayQueryExtension : 1;
      uint32_t deferredHostOperationsExtension : 1;
    } vk;
#endif
#if (DEVICE_IMPL_D3D12)
    struct {
      IDXGIAdapter4 *adapter; // borrowed from renderer-owned list; released by ShutdownRIRenderer
      uint64_t dedicatedVideoMemory;
      uint64_t dedicatedSystemMemory;
      uint64_t sharedSystemMemory;
      uint32_t vendorId;
      uint32_t deviceId;
      uint8_t highestFeatureLevelMajor;
      uint8_t highestFeatureLevelMinor;
      uint8_t highestShaderModelMajor;
      uint8_t highestShaderModelMinor;
      uint8_t resourceBindingTier;
      uint8_t rayTracingTier; // 0=none, 1=DXR 1.0, 2=DXR 1.1
      uint8_t meshShaderTier;
      uint8_t isWarp : 1;
    } d3d12;
#endif
  };
};

struct RIDevice {
  RIDevice() { memset(this, 0, sizeof(*this)); }
  // Creates the logical device, queues and memory allocator (VMA / D3D12MA) on
  // the adapter in init->physicalAdapter. Capability-neutral: it enables what
  // the adapter can give and publishes the result on the *Enabled fields below,
  // so callers vet the adapter first and read those fields afterwards.
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
#if (DEVICE_IMPL_D3D12)
    struct {
      ID3D12Device *device;
      // QueryInterface'd from `device` once at init. NULL when the runtime or
      // adapter predates DXR; every acceleration-structure and state-object
      // entry point checks it before use.
      ID3D12Device5 *device5;
      D3D12MA::Allocator *allocator;
      // Owned COM references parallel to `queues[]`. Released alongside the device.
      ID3D12CommandQueue *queues[RI_QUEUE_LEN];
      ID3D12InfoQueue *infoQueue;
      ID3D12InfoQueue1 *infoQueue1;
      ID3D12CommandSignature *drawIndirectSignature;
      ID3D12CommandSignature *drawIndirectPaddedSignature;
      ID3D12CommandSignature *drawIndexedIndirectSignature;
      DWORD infoQueueCookie;
      uint32_t nextGeometrySrvIndex;
    } d3d12;
#endif
  };
};

inline bool RITexture::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.image == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return RID3D12_TextureIsEmpty(*this);
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RIBuffer::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.buffer == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return RID3D12_BufferIsEmpty(*this);
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RITextureView::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.image == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return RID3D12_TextureViewIsEmpty(*this);
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RICmd::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.cmd == VK_NULL_HANDLE || vk.pool == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return d3d12.cmdList == nullptr || d3d12.allocator == nullptr;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RISampler::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.sampler == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return RID3D12_SamplerIsEmpty(*this);
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RIAccelStructure::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.handle == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return RID3D12_AccelStructureIsEmpty(*this);
#endif
  assert(false && "unhandled backend");
  return true;
}

#endif // RI_DEVICE_H
