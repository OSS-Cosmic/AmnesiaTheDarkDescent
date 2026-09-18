#include "graphics/FsrUpscaler.h"

#include "engine/Interface.h"
#include "graphics/Graphics.h"
#include "graphics/RIBarrier.h"
#include "graphics/RICommand.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIDevice.h"
#include "graphics/RIProgram.h"
#include "graphics/RIVK.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "resources/Resources.h"
#include "system/Hasher.h"
#include "system/LowLevelSystem.h"
#include "system/String.h"

// Xlib exposes a global `None` macro, while the parameter contract quite
// reasonably uses None as an enum value. Keep that platform macro out of the
// pure parameter layer and out of the adapter translation unit.
#ifdef None
#undef None
#endif
#include "graphics/FsrUpscalerParams.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#endif

namespace hpl {
namespace {

static bool SameExtent(TemporalUpscalerExtent lhs,
                       TemporalUpscalerExtent rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height;
}

static bool IsNonZeroExtent(TemporalUpscalerExtent extent) {
  return extent.width != 0 && extent.height != 0;
}

static bool IsValidExtent(TemporalUpscalerExtent extent) {
  return IsNonZeroExtent(extent) && extent.width <= 16384u &&
         extent.height <= 16384u;
}

static bool IsFinite(float value) { return std::isfinite(value); }

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE

static void FsrMessageCallback(FfxMsgType type, const wchar_t *message) {
  (void)type;
  const std::string converted =
      cString::To8Char(std::wstring(message != nullptr ? message : L""));
  Warning("FSR SDK: %s\n", converted.c_str());
}

static bool MapQuality(TemporalUpscalerQuality quality,
                       FfxFsr3UpscalerQualityMode *mapped) {
  if (!mapped)
    return false;

  switch (quality) {
  case TemporalUpscalerQuality::NativeAA:
    *mapped = FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
    return true;
  case TemporalUpscalerQuality::Quality:
    *mapped = FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
    return true;
  case TemporalUpscalerQuality::Balanced:
    *mapped = FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED;
    return true;
  case TemporalUpscalerQuality::Performance:
    *mapped = FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE;
    return true;
  case TemporalUpscalerQuality::UltraPerformance:
    *mapped = FFX_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE;
    return true;
  }
  return false;
}

static bool Is2DBinding(const TemporalUpscalerTextureBinding &binding) {
  // FfxResourceDescription has no view offset fields. Rejecting a partial
  // view is safer than registering the right VkImage with metadata that
  // silently describes a different mip or array slice.
  return binding.IsValid() && binding.mipOffset == 0 &&
         binding.mipCount == 1 && binding.layerOffset == 0 &&
         binding.layerCount == 1 && !binding.texture->isEmpty() &&
         !binding.view->isEmpty() &&
         binding.texture->vk.image != VK_NULL_HANDLE &&
         binding.view->vk.image != VK_NULL_HANDLE;
}

static FfxResourceDescription MakeResourceDescription(
    const TemporalUpscalerTextureBinding &binding, FfxResourceUsage usage) {
  FfxResourceDescription description = {};
  description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  description.format =
      ffxGetSurfaceFormatVK(RIFormatToVK(binding.format));
  description.width = binding.extent.width;
  description.height = binding.extent.height;
  description.depth = 1;
  description.mipCount = binding.mipCount;
  description.flags = FFX_RESOURCE_FLAGS_NONE;
  description.usage = usage;
  return description;
}

static FfxResource MakeExternalResource(
    const TemporalUpscalerTextureBinding &binding, FfxResourceStates state,
    FfxResourceUsage usage, const wchar_t *name) {
  return ffxGetResourceVK(
      reinterpret_cast<void *>(binding.texture->vk.image),
      MakeResourceDescription(binding, usage), name, state);
}

static RI_Format_e FfxFormatToRI(FfxSurfaceFormat format) {
  switch (format) {
  case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT:
    return RI_FORMAT_RGBA16_SFLOAT;
  case FFX_SURFACE_FORMAT_R16G16_FLOAT:
    return RI_FORMAT_RG16_SFLOAT;
  case FFX_SURFACE_FORMAT_R32_FLOAT:
    return RI_FORMAT_R32_SFLOAT;
  case FFX_SURFACE_FORMAT_R32_UINT:
    return RI_FORMAT_R32_UINT;
  case FFX_SURFACE_FORMAT_R8_UNORM:
    return RI_FORMAT_R8_UNORM;
  default:
    return RI_FORMAT_UNKNOWN;
  }
}

static uint32_t FfxUsageToRI(FfxResourceUsage usage) {
  // TRANSFER_SRC/DST are unconditional: the SDK's own VK backend creates every
  // internal image with them and its job queue relies on it. executeGpuJobClear*
  // barriers the target to TRANSFER_DST_OPTIMAL and calls vkCmdClearColorImage
  // (FSR3 clears its lock/reconstructed-depth history on reset), and the copy
  // job blits between internal resources. FfxResourceUsage carries no bit for
  // either, so deriving usage from it alone yields SAMPLED|STORAGE and the
  // clear barrier trips VUID-VkImageMemoryBarrier-oldLayout-01213.
  uint32_t riUsage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_SRC |
                     RI_USAGE_TRANSFER_DST;
  if ((usage & FFX_RESOURCE_USAGE_UAV) != 0)
    riUsage |= RI_USAGE_SHADER_RESOURCE_STORAGE;
  if ((usage & FFX_RESOURCE_USAGE_RENDERTARGET) != 0)
    riUsage |= RI_USAGE_COLOR_ATTACHMENT;
  return riUsage;
}

static void AppendBindingBarrier(
    std::array<RITextureBarrier, 16> *barriers, uint32_t *count,
    const TemporalUpscalerTextureBinding &binding, RIResourceState_e before,
    RIResourceState_e after, RIBarrierAspect_e aspect,
    uint32_t beforeStage = RI_STAGE_NONE,
    uint32_t afterStage = RI_STAGE_COMPUTE) {
  if (!barriers || !count || !binding.texture || before == after)
    return;

  assert(*count < barriers->size());
  RITextureBarrier barrier(binding.texture, before, after, beforeStage,
                           afterStage, aspect);
  barrier.baseMip = static_cast<uint16_t>(binding.mipOffset);
  barrier.mipCount = static_cast<uint16_t>(binding.mipCount);
  barrier.baseLayer = static_cast<uint16_t>(binding.layerOffset);
  barrier.layerCount = static_cast<uint16_t>(binding.layerCount);
  (*barriers)[(*count)++] = barrier;
}

struct FsrImage {
  RISharedPointer<RITexture> texture;
  RISharedPointer<RITextureView> view;
  RI_Format_e format = RI_FORMAT_UNKNOWN;
  TemporalUpscalerExtent extent = {};
  RIResourceState_e state = RI_RESOURCE_STATE_UNDEFINED;
  FfxResourceDescription ffxDescription = {};
  FfxResourceStates ffxState = FFX_RESOURCE_STATE_UNORDERED_ACCESS;

  // Vulkan requires the VkImageView be destroyed before the VkImage it was
  // created from, and RISharedPointer disposes the moment the last reference
  // drops.  Member-wise assignment from `{}` would clear `texture` first, so
  // every reset goes through here and releases `view` first.
  void Reset() {
    view = RISharedPointer<RITextureView>();
    texture = RISharedPointer<RITexture>();
    format = RI_FORMAT_UNKNOWN;
    extent = {};
    state = RI_RESOURCE_STATE_UNDEFINED;
    ffxDescription = {};
    ffxState = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
  }
};

static bool CreateImage(cGraphics *graphics, FsrImage *image,
                        RI_Format_e format, TemporalUpscalerExtent extent,
                        uint32_t usage, RITextureViewType_e viewType,
                        const char *name) {
  if (!graphics || !image || !IsNonZeroExtent(extent) ||
      format == RI_FORMAT_UNKNOWN)
    return false;

  RITextureDesc textureDesc = {};
  textureDesc.type = RI_TEXTURE_2D;
  textureDesc.format = format;
  textureDesc.width = extent.width;
  textureDesc.height = extent.height;
  textureDesc.depth = 1;
  textureDesc.mipNum = 1;
  textureDesc.layerNum = 1;
  textureDesc.sampleCount = RI_SAMPLE_COUNT_1;
  textureDesc.usage = usage;
  textureDesc.flags = RI_TEXTURE_FLAG_NONE;

  RITexture texture = RITexture::create(&graphics->device, textureDesc);
  if (texture.isEmpty()) {
    Warning("FSR: could not allocate %s image\n", name ? name : "owned");
    return false;
  }
  image->texture = RISharedPointer<RITexture>(&graphics->device, texture);

  RITextureViewDesc viewDesc = {};
  viewDesc.viewType = viewType;
  viewDesc.format = format;
  viewDesc.baseMip = 0;
  viewDesc.mipNum = 1;
  viewDesc.baseLayer = 0;
  viewDesc.layerNum = 1;
  RITextureView view =
      RITextureView::create(&graphics->device, image->texture.Get(), viewDesc);
  if (view.isEmpty()) {
    Warning("FSR: could not allocate %s view\n", name ? name : "owned");
    return false;
  }
  image->view =
      RISharedPointer<RITextureView>(&graphics->device, view);
  image->format = format;
  image->extent = extent;
  image->state = RI_RESOURCE_STATE_UNDEFINED;
  return true;
}

static bool CreateImageFromDescription(cGraphics *graphics, FsrImage *image,
                                       const FfxCreateResourceDescription &src,
                                       const char *name) {
  if (!image || src.resourceDescription.type != FFX_RESOURCE_TYPE_TEXTURE2D ||
      src.resourceDescription.width == 0 ||
      src.resourceDescription.height == 0)
    return false;

  const RI_Format_e format = FfxFormatToRI(src.resourceDescription.format);
  if (format == RI_FORMAT_UNKNOWN)
    return false;

  if (!CreateImage(graphics, image, format,
                   {src.resourceDescription.width,
                    src.resourceDescription.height},
                   FfxUsageToRI(src.resourceDescription.usage),
                   RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D, name))
    return false;

  image->ffxDescription = src.resourceDescription;
  image->ffxState = src.initialState;
  return true;
}

static FfxResource MakeOwnedResource(const FsrImage &image,
                                     const wchar_t *name) {
  return ffxGetResourceVK(
      reinterpret_cast<void *>(image.texture->vk.image), image.ffxDescription,
      name, image.ffxState);
}

static void AppendOwnedBarrier(std::array<RITextureBarrier, 16> *barriers,
                               uint32_t *count, FsrImage *image,
                               RIResourceState_e after,
                               uint32_t beforeStage = RI_STAGE_COMPUTE,
                               uint32_t afterStage = RI_STAGE_COMPUTE) {
  if (!barriers || !count || !image || !image->texture ||
      image->state == after)
    return;
  assert(*count < barriers->size());
  (*barriers)[(*count)++] = RITextureBarrier(
      image->texture.Get(), image->state, after, beforeStage, afterStage,
      RI_BARRIER_ASPECT_COLOR);
  image->state = after;
}

static bool FormatSupportsImageUsage(VkPhysicalDevice physicalDevice,
                                     VkFormat format,
                                     VkFormatFeatureFlags required) {
  VkFormatProperties properties = {};
  vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
  return (properties.optimalTilingFeatures & required) == required;
}

static bool EnabledFsrFeatures(VkPhysicalDevice physicalDevice,
                               uint32_t apiVersion, std::string *reason) {
  VkPhysicalDeviceFeatures2 features = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features features11 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features features12 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features features13 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  features.pNext = &features11;
  features11.pNext = &features12;
  if (apiVersion >= VK_API_VERSION_1_3)
    features12.pNext = &features13;

  // RIRenderer passes the queried Vulkan 1.1/1.2/1.3 feature chain directly
  // to vkCreateDevice. Reading it again here therefore checks the feature set
  // actually enabled by this engine, rather than a hypothetical driver set.
  vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

  if (!features12.scalarBlockLayout) {
    if (reason)
      *reason = "scalarBlockLayout is not enabled";
    return false;
  }
  if (!features.features.shaderStorageImageExtendedFormats) {
    if (reason)
      *reason = "extended storage-image formats are not enabled";
    return false;
  }

  // FSR's Vulkan backend selects its FP16 and wave-size permutations from the
  // capabilities callback. The engine enables shaderFloat16 when available;
  // otherwise the SDK's FP32 permutation remains valid and is intentionally
  // not rejected here. No vendor identity is relevant to this choice.
  (void)features12.shaderFloat16;
  (void)features11;
  (void)features13;
  return true;
}

static bool GetFsrCapabilities(cGraphics *graphics, FfxInterface *backend,
                               std::shared_ptr<std::vector<uint8_t>> *scratch,
                               FfxDeviceCapabilities *capabilities,
                               std::string *reason) {
  if (!graphics || !backend || !scratch || !capabilities)
    return false;

  const VkPhysicalDevice physicalDevice =
      graphics->device.physicalAdapter.vk.physicalDevice;
  if (physicalDevice == VK_NULL_HANDLE || graphics->device.vk.device == VK_NULL_HANDLE) {
    if (reason)
      *reason = "Vulkan device is unavailable";
    return false;
  }

  const size_t scratchSize = ffxGetScratchMemorySizeVK(physicalDevice, 1);
  if (scratchSize == 0) {
    if (reason)
      *reason = "Vulkan backend scratch size is zero";
    return false;
  }
  *scratch = std::make_shared<std::vector<uint8_t>>(scratchSize);

  VkDeviceContext vkDeviceContext = {};
  vkDeviceContext.vkDevice = graphics->device.vk.device;
  vkDeviceContext.vkPhysicalDevice = physicalDevice;
  // RIRenderer uses volk's global loader entry point after volkLoadDevice.
  vkDeviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
  const FfxDevice device = ffxGetDeviceVK(&vkDeviceContext);
  *backend = {};
  const FfxErrorCode interfaceResult = ffxGetInterfaceVK(
      backend, device, (*scratch)->data(), (*scratch)->size(), 1);
  if (interfaceResult != FFX_OK || !backend->fpGetDeviceCapabilities) {
    if (reason)
      *reason = "Vulkan FSR backend interface is unavailable";
    return false;
  }

  *capabilities = {};
  const FfxErrorCode capabilitiesResult =
      backend->fpGetDeviceCapabilities(backend, capabilities);
  if (capabilitiesResult != FFX_OK) {
    if (reason)
      *reason = "FSR backend capability query failed";
    return false;
  }
  return true;
}

#endif // defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE

} // namespace

struct cFsrUpscaler::Impl {
  explicit Impl(cGraphics *owner) : graphics(owner) {}

  cGraphics *graphics = nullptr;

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
  struct PreparedState {
    FfxFsr3UpscalerContext context = {};
    bool contextCreated = false;
    std::shared_ptr<std::vector<uint8_t>> scratch;
    std::shared_ptr<RIProgram> copyDeviceDepthProgram;
    FsrImage dilatedDepth;
    FsrImage dilatedMotionVectors;
    FsrImage reconstructedPrevNearestDepth;
    FsrImage reactiveMask;
    FsrImage deviceDepth;
    TemporalUpscalerExtent render = {};
    TemporalUpscalerExtent outputExtent = {};
    TemporalUpscalerQuality quality = TemporalUpscalerQuality::Quality;
    bool valid = false;
  } prepared;

  mutable std::string supportFailure;
  void LogSupportFailure(const char *reason) const {
    if (supportFailure.empty()) {
      supportFailure = reason ? reason : "unknown FSR requirement";
      Log("FSR: unavailable: %s\n", supportFailure.c_str());
    }
  }

  void DeferPrepared(PreparedState &&state) {
    if (!graphics) {
      if (state.contextCreated) {
        const FfxErrorCode result =
            ffxFsr3UpscalerContextDestroy(&state.context);
        if (result != FFX_OK)
          Log("FSR: context destruction failed (%d)\n",
              static_cast<int>(result));
      }
      return;
    }

    // Context destruction must precede scratch release: the SDK backend stores
    // its allocator and per-context tables inside this exact scratch block.
    auto parked = std::make_shared<PreparedState>(std::move(state));
    RIDevice *device = &graphics->device;
    graphics->graphicsDefer.push(std::function<void()>(
        [parked, device]() mutable {
          if (parked->contextCreated) {
            const FfxErrorCode result =
                ffxFsr3UpscalerContextDestroy(&parked->context);
            if (result != FFX_OK)
              Log("FSR: deferred context destruction failed (%d)\n",
                  static_cast<int>(result));
            parked->contextCreated = false;
          }
          if (parked->copyDeviceDepthProgram) {
            parked->copyDeviceDepthProgram->dispose(device);
            parked->copyDeviceDepthProgram.reset();
          }
          parked->deviceDepth.Reset();
          parked->reactiveMask.Reset();
          parked->reconstructedPrevNearestDepth.Reset();
          parked->dilatedMotionVectors.Reset();
          parked->dilatedDepth.Reset();
          parked->scratch.reset();
        }));
  }

  bool CopyDeviceDepth(const TemporalUpscalerTextureBinding &depth,
                       RICmd *cmd, uint32_t frameIndex) {
    if (!graphics || !cmd || !depth.view || !prepared.deviceDepth.view ||
        !prepared.deviceDepth.texture)
      return false;

    if (!prepared.copyDeviceDepthProgram) {
      cResources *resources = Interface<cResources>::Get();
      if (!resources || !resources->GetFileSearcher())
        return false;

      auto copyDeviceDepthBin = RIProgram::loadShaderStage(
          resources->GetFileSearcher(), "CopyDeviceDepth.cs.spv");
      if (copyDeviceDepthBin.empty())
        return false;

      std::array<RIProgram::ModuleStage, 1> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_COMPUTE,
                                 copyDeviceDepthBin, "csMain"}};
      auto program = std::make_shared<RIProgram>();
      program->initialize(&graphics->device, stages, {},
                          "Temporal.CopyDeviceDepth.cs");
      prepared.copyDeviceDepthProgram = std::move(program);
    }

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, 0x43445054u);
    prepared.copyDeviceDepthProgram->bindComputePipeline(
        &graphics->device, cmd, pipelineHash, "Temporal.CopyDeviceDepth.cs",
        &pipelineInfo);

    // The caller supplies the depth-aspect-only sampled view. A combined
    // depth/stencil view is not legal for this sampled image descriptor.
    std::array<RIProgram::DescriptorBinding, 2> bindings = {
        RIProgram::DescriptorBinding(
            2, 0,
            RIDescriptor::sampledImage(&graphics->device, depth.view,
                                       RI_RESOURCE_STATE_SHADER_RESOURCE)),
        RIProgram::DescriptorBinding(
            2, 1,
            RIDescriptor::storageImage(&graphics->device,
                                       prepared.deviceDepth.view.Get()))};
    prepared.copyDeviceDepthProgram->bindDescriptors(
        &graphics->device, cmd, frameIndex, bindings.data(), bindings.size(),
        VK_PIPELINE_BIND_POINT_COMPUTE);

    struct CopyDeviceDepthPushConstants {
      uint32_t extent[2];
    } push = {};
    push.extent[0] = prepared.deviceDepth.extent.width;
    push.extent[1] = prepared.deviceDepth.extent.height;
    static_assert(sizeof(CopyDeviceDepthPushConstants) == 8,
                  "CopyDeviceDepthPC must match the Slang push-constant layout");
    cmd->vk_d3d12_setPushConstants(
        &graphics->device, *prepared.copyDeviceDepthProgram, 0, sizeof(push),
        &push);
    cmd->dispatch(&graphics->device, (push.extent[0] + 15u) / 16u,
                  (push.extent[1] + 15u) / 16u, 1u);
    return true;
  }

  void ReleasePrepared() {
    if (!prepared.contextCreated && !prepared.scratch &&
        !prepared.copyDeviceDepthProgram && !prepared.deviceDepth.texture &&
        !prepared.reactiveMask.texture && !prepared.dilatedDepth.texture &&
        !prepared.dilatedMotionVectors.texture &&
        !prepared.reconstructedPrevNearestDepth.texture)
      return;
    DeferPrepared(std::move(prepared));
    // FfxFsr3UpscalerContext alone is 512 KiB. Aggregate assignment from {}
    // creates a full PreparedState temporary on the stack; when this is called
    // while PrepareContext also has a candidate alive, Windows' default 1 MiB
    // stack overflows in __chkstk. Reconstruct the heap-resident Impl member
    // directly instead.
    prepared.~PreparedState();
    ::new (static_cast<void *>(&prepared)) PreparedState();
  }

#else
  void ReleasePrepared() {}
#endif
};

cFsrUpscaler::cFsrUpscaler(cGraphics *graphics)
    : m_impl(std::make_unique<Impl>(graphics)) {}

cFsrUpscaler::~cFsrUpscaler() { m_impl->ReleasePrepared(); }

bool cFsrUpscaler::Supports(TemporalUpscalerProvider provider,
                            TemporalUpscalerQuality quality) const {
  if (provider != TemporalUpscalerProvider::Fsr)
    return false;

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
  FfxFsr3UpscalerQualityMode mappedQuality =
      FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
  if (!MapQuality(quality, &mappedQuality)) {
    m_impl->LogSupportFailure("quality has no FSR mapping");
    return false;
  }
  (void)mappedQuality;

  if (!m_impl->graphics) {
    m_impl->LogSupportFailure("owning graphics device is unavailable");
    return false;
  }
  if (m_impl->graphics->device.vk.device == VK_NULL_HANDLE ||
      m_impl->graphics->device.physicalAdapter.vk.physicalDevice ==
          VK_NULL_HANDLE) {
    m_impl->LogSupportFailure("Vulkan device is unavailable");
    return false;
  }

  FfxInterface backend = {};
  std::shared_ptr<std::vector<uint8_t>> scratch;
  FfxDeviceCapabilities capabilities = {};
  std::string reason;
  if (!GetFsrCapabilities(m_impl->graphics, &backend, &scratch,
                          &capabilities, &reason)) {
    m_impl->LogSupportFailure(reason.c_str());
    return false;
  }
  if (capabilities.maximumSupportedShaderModel < FFX_SHADER_MODEL_5_1) {
    m_impl->LogSupportFailure("backend shader model is below 5.1");
    return false;
  }
  if (capabilities.waveLaneCountMin > 32 ||
      capabilities.waveLaneCountMax < 32) {
    m_impl->LogSupportFailure("backend has no wave32 capability");
    return false;
  }

  const VkPhysicalDevice physicalDevice =
      m_impl->graphics->device.physicalAdapter.vk.physicalDevice;
  const uint32_t apiVersion =
      m_impl->graphics->device.physicalAdapter.vk.apiVersion;
  if (!EnabledFsrFeatures(physicalDevice, apiVersion, &reason)) {
    m_impl->LogSupportFailure(reason.c_str());
    return false;
  }

  const VkFormatFeatureFlags colorFeatures =
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
  if (!FormatSupportsImageUsage(physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT,
                                colorFeatures)) {
    m_impl->LogSupportFailure(
        "RGBA16_SFLOAT storage/sampled/transfer usage is unavailable");
    return false;
  }
  const VkFormatFeatureFlags depthCopyFeatures =
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (!FormatSupportsImageUsage(physicalDevice, VK_FORMAT_R32_SFLOAT,
                                depthCopyFeatures)) {
    m_impl->LogSupportFailure(
        "R32_SFLOAT storage/sampled copy target is unavailable");
    return false;
  }

  // The SDK receives the backend's actual capabilities. In particular, its
  // FP16 and wave-size permutations follow the enabled feature/extension
  // chain; the valid FP32 path remains available when FP16 is absent.
  return true;
#else
  (void)quality;
  Log("FSR: Supports failed: FSR support is not compiled in\n");
  return false;
#endif
}

TemporalUpscalerExtent cFsrUpscaler::GetRecommendedRenderExtent(
    TemporalUpscalerExtent output, TemporalUpscalerQuality quality) const {
  if (!IsNonZeroExtent(output))
    return {};

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
  FfxFsr3UpscalerQualityMode mappedQuality =
      FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
  if (!MapQuality(quality, &mappedQuality)) {
    Log("FSR: invalid quality for resolution query\n");
    return {};
  }

  uint32_t renderWidth = 0;
  uint32_t renderHeight = 0;
  const FfxErrorCode result =
      ffxFsr3UpscalerGetRenderResolutionFromQualityMode(
          &renderWidth, &renderHeight, output.width, output.height,
          mappedQuality);
  if (result != FFX_OK || renderWidth == 0 || renderHeight == 0 ||
      renderWidth > output.width || renderHeight > output.height ||
      renderWidth > 16384u || renderHeight > 16384u) {
    Log("FSR: SDK returned an invalid render resolution\n");
    return {};
  }
  return {renderWidth, renderHeight};
#else
  (void)quality;
  Log("FSR: GetRecommendedRenderExtent failed: FSR support is not compiled in\n");
  return {};
#endif
}

uint32_t cFsrUpscaler::GetJitterPhaseCount(
    TemporalUpscalerExtent render, TemporalUpscalerExtent output) const {
  if (!IsNonZeroExtent(render) || !IsNonZeroExtent(output))
    return 0;

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
  if (render.width > output.width || render.height > output.height) {
    Log("FSR: invalid render/output extent for jitter query\n");
    return 0;
  }
  const int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount(
      static_cast<int32_t>(render.width), static_cast<int32_t>(output.width));
  if (phaseCount <= 0)
    Log("FSR: SDK returned zero jitter phases\n");
  return phaseCount > 0 ? static_cast<uint32_t>(phaseCount) : 0;
#else
  (void)render;
  (void)output;
  Log("FSR: GetJitterPhaseCount failed: FSR support is not compiled in\n");
  return 0;
#endif
}

bool cFsrUpscaler::PrepareContext(const TemporalUpscalerSettings &settings,
                                  TemporalUpscalerExtent render,
                                  TemporalUpscalerExtent output,
                                  cGraphics::FrameContext *frame) {
  if (!frame || !IsValidExtent(render) || !IsValidExtent(output) ||
      render.width > output.width || render.height > output.height) {
    Log("FSR: PrepareContext failed: invalid frame or extent\n");
    return false;
  }

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
  if (settings.provider != TemporalUpscalerProvider::Fsr) {
    Log("FSR: PrepareContext failed: settings select another provider\n");
    return false;
  }
  if (!Supports(settings.provider, settings.quality)) {
    Log("FSR: PrepareContext failed: device or quality is unsupported\n");
    return false;
  }

  if (m_impl->prepared.valid &&
      SameExtent(m_impl->prepared.render, render) &&
      SameExtent(m_impl->prepared.outputExtent, output) &&
      m_impl->prepared.quality == settings.quality)
    return true;

  // The opaque SDK context embedded in PreparedState is 512 KiB. Keep
  // replacement state off the thread stack, particularly because context
  // preparation runs within the render call chain on Windows.
  auto candidate = std::make_unique<Impl::PreparedState>();
  candidate->render = render;
  candidate->outputExtent = output;
  candidate->quality = settings.quality;

  FfxFsr3UpscalerQualityMode mappedQuality =
      FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
  if (!MapQuality(settings.quality, &mappedQuality)) {
    Log("FSR: PrepareContext failed: quality has no SDK mapping\n");
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }

  FfxInterface backend = {};
  std::shared_ptr<std::vector<uint8_t>> scratch;
  FfxDeviceCapabilities capabilities = {};
  std::string reason;
  if (!GetFsrCapabilities(m_impl->graphics, &backend, &scratch,
                          &capabilities, &reason)) {
    Log("FSR: PrepareContext failed: %s\n", reason.c_str());
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }
  candidate->scratch = std::move(scratch);

  FfxFsr3UpscalerContextDescription description = {};
  description.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                     FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
#if !defined(NDEBUG)
  description.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
#endif
  // Motion vectors are render-resolution UV velocity and already have engine
  // jitter removed, so neither the display-resolution nor cancellation flags
  // are set. The omitted depth flags describe finite, non-reversed depth.
  description.maxRenderSize = {render.width, render.height};
  description.maxUpscaleSize = {output.width, output.height};
#if !defined(NDEBUG)
  description.fpMessage = FsrMessageCallback;
#else
  description.fpMessage = nullptr;
#endif
  description.backendInterface = backend;
  const FfxErrorCode contextResult = ffxFsr3UpscalerContextCreate(
      &candidate->context, &description);
  if (contextResult != FFX_OK) {
    Log("FSR: PrepareContext failed: SDK context creation error %d\n",
        static_cast<int>(contextResult));
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }
  candidate->contextCreated = true;

  FfxFsr3UpscalerSharedResourceDescriptions shared = {};
  if (ffxFsr3UpscalerGetSharedResourceDescriptions(&candidate->context,
                                                  &shared) != FFX_OK ||
      !CreateImageFromDescription(m_impl->graphics, &candidate->dilatedDepth,
                                  shared.dilatedDepth,
                                  "FSR.dilatedDepth") ||
      !CreateImageFromDescription(
          m_impl->graphics, &candidate->dilatedMotionVectors,
          shared.dilatedMotionVectors, "FSR.dilatedMotionVectors") ||
      !CreateImageFromDescription(
          m_impl->graphics, &candidate->reconstructedPrevNearestDepth,
          shared.reconstructedPrevNearestDepth,
          "FSR.reconstructedPrevNearestDepth")) {
    Log("FSR: PrepareContext failed: shared resources were not created\n");
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }

  // The automatic path writes this temporary reactive mask. Explicit caller
  // masks never use or overwrite it.
  if (!CreateImage(m_impl->graphics, &candidate->reactiveMask, RI_FORMAT_R8_UNORM,
                   render,
                   RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
                       RI_USAGE_TRANSFER_SRC | RI_USAGE_TRANSFER_DST,
                   RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D,
                   "FSR.reactiveMask")) {
    Log("FSR: PrepareContext failed: automatic reactive resource was not created\n");
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }

  // Combined depth/stencil images cannot be registered with the SDK as its
  // R32_FLOAT surface format. Keep a plain R32 copy at render resolution for
  // that path; the direct D32_SFLOAT path does not use this target.
  if (!CreateImage(m_impl->graphics, &candidate->deviceDepth,
                   RI_FORMAT_R32_SFLOAT, render,
                   RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
                       RI_USAGE_TRANSFER_SRC | RI_USAGE_TRANSFER_DST,
                   RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D,
                   "FSR.deviceDepth")) {
    Log("FSR: PrepareContext failed: device-depth copy resource was not created\n");
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }
  candidate->deviceDepth.ffxDescription = {};
  candidate->deviceDepth.ffxDescription.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  candidate->deviceDepth.ffxDescription.format = FFX_SURFACE_FORMAT_R32_FLOAT;
  candidate->deviceDepth.ffxDescription.width = render.width;
  candidate->deviceDepth.ffxDescription.height = render.height;
  candidate->deviceDepth.ffxDescription.depth = 1;
  candidate->deviceDepth.ffxDescription.mipCount = 1;
  candidate->deviceDepth.ffxDescription.flags = FFX_RESOURCE_FLAGS_NONE;
  candidate->deviceDepth.ffxDescription.usage = FFX_RESOURCE_USAGE_READ_ONLY;
  candidate->deviceDepth.ffxState = FFX_RESOURCE_STATE_COMPUTE_READ;

  candidate->reactiveMask.ffxDescription = {};
  candidate->reactiveMask.ffxDescription.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  candidate->reactiveMask.ffxDescription.format = FFX_SURFACE_FORMAT_R8_UNORM;
  candidate->reactiveMask.ffxDescription.width = render.width;
  candidate->reactiveMask.ffxDescription.height = render.height;
  candidate->reactiveMask.ffxDescription.depth = 1;
  candidate->reactiveMask.ffxDescription.mipCount = 1;
  candidate->reactiveMask.ffxDescription.flags = FFX_RESOURCE_FLAGS_NONE;
  candidate->reactiveMask.ffxDescription.usage = FFX_RESOURCE_USAGE_UAV;
  candidate->reactiveMask.ffxState = FFX_RESOURCE_STATE_UNORDERED_ACCESS;

  // The candidate is not published until every context and image exists, so
  // a later RecordResolve can never observe a half-initialized replacement.
  m_impl->ReleasePrepared();
  candidate->valid = true;
  m_impl->prepared = std::move(*candidate);
  return true;
#else
  (void)settings;
  (void)render;
  (void)output;
  Log("FSR: PrepareContext failed: FSR support is not compiled in\n");
  return false;
#endif
}

TemporalUpscalerOutput cFsrUpscaler::RecordResolve(
    TemporalUpscalerExtent render, TemporalUpscalerExtent output,
    const TemporalUpscalerFrameInput &input) {
  TemporalUpscalerOutput failure = {};

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
  auto fail = [&failure](const char *reason) {
    Log("FSR: RecordResolve failed: %s\n", reason);
    return failure;
  };

  if (!m_impl->prepared.valid || !m_impl->prepared.contextCreated)
    return fail("context was not prepared");
  if (!SameExtent(render, m_impl->prepared.render) ||
      !SameExtent(output, m_impl->prepared.outputExtent))
    return fail("extent does not match the prepared context");
  if (!input.cmd || input.cmd->vk.cmd == VK_NULL_HANDLE)
    return fail("command buffer is null");
  // TemporalPresentation calls providers after closing its dynamic-rendering
  // scope. RI has no public query for that scope, so the command-buffer
  // contract is asserted here and remains explicit at the call boundary.
  assert(input.cmd->vk.cmd != VK_NULL_HANDLE);

  if (!input.color.IsValid() || !input.depth.IsValid() ||
      !input.motionVectors.IsValid() || !input.output.IsValid())
    return fail("required texture binding is invalid");
  if (!SameExtent(input.color.extent, render) ||
      !SameExtent(input.depth.extent, render) ||
      !SameExtent(input.motionVectors.extent, render) ||
      !SameExtent(input.output.extent, output))
    return fail("binding extent does not match the resolve extent");
  if (input.color.format != cGraphics::PogoColorFormat ||
      input.output.format != cGraphics::PogoColorFormat)
    return fail("HDR color/output must be RGBA16_SFLOAT");
  if (input.motionVectors.format != cGraphics::VelocityFormat)
    return fail("motion vectors must be RG16_SFLOAT");
  if (!Is2DBinding(input.color) || !Is2DBinding(input.depth) ||
      !Is2DBinding(input.motionVectors) || !Is2DBinding(input.output))
    return fail("FSR requires one-mip, one-layer 2D bindings");

  const bool depthIsCombined =
      input.depth.format == RI_FORMAT_D32_SFLOAT_S8_UINT;
  const bool depthIsPlain = input.depth.format == RI_FORMAT_D32_SFLOAT;
  if (!depthIsCombined && !depthIsPlain)
    return fail("depth format must be D32_SFLOAT or D32_SFLOAT_S8_UINT");

  auto bindingHasData = [](const TemporalUpscalerTextureBinding &binding) {
    return binding.texture || binding.view ||
           binding.format != RI_FORMAT_UNKNOWN || IsNonZeroExtent(binding.extent) ||
           binding.mipOffset != 0 || binding.mipCount != 0 ||
           binding.layerOffset != 0 || binding.layerCount != 0;
  };
  const bool opaquePresent = input.opaqueColor.IsValid();
  const bool reactivePresent = input.reactiveMaskJittered.IsValid();
  const bool compositionPresent = input.compositionMaskJittered.IsValid();
  if ((bindingHasData(input.opaqueColor) && !opaquePresent) ||
      (bindingHasData(input.reactiveMaskJittered) && !reactivePresent) ||
      (bindingHasData(input.compositionMaskJittered) && !compositionPresent))
    return fail("optional mask or opaque binding is malformed");

  auto validateOptionalBinding = [&](const TemporalUpscalerTextureBinding &binding,
                                     bool present, const char *name) {
    if (!present)
      return true;
    if (!SameExtent(binding.extent, render) || !Is2DBinding(binding)) {
      Log("FSR: RecordResolve failed: %s binding is invalid\n", name);
      return false;
    }
    return true;
  };
  if (!validateOptionalBinding(input.opaqueColor, opaquePresent,
                               "opaque color") ||
      !validateOptionalBinding(input.reactiveMaskJittered, reactivePresent,
                               "reactive mask") ||
      !validateOptionalBinding(input.compositionMaskJittered,
                               compositionPresent, "composition mask"))
    return failure;
  if (opaquePresent &&
      input.opaqueColor.format != cGraphics::PogoColorFormat)
    return fail("opaque color format must be RGBA16_SFLOAT");

  FsrMaskPolicyInput maskInput = {};
  maskInput.renderExtent = {render.width, render.height};
  auto toMaskInfo = [](const TemporalUpscalerTextureBinding &binding) {
    FsrMaskBindingInfo result = {};
    result.present = binding.IsValid();
    result.extent = {binding.extent.width, binding.extent.height};
    result.mipOffset = binding.mipOffset;
    result.mipCount = binding.mipCount;
    result.layerOffset = binding.layerOffset;
    result.layerCount = binding.layerCount;
    return result;
  };
  maskInput.reactiveMask = toMaskInfo(input.reactiveMaskJittered);
  maskInput.compositionMask = toMaskInfo(input.compositionMaskJittered);
  maskInput.opaqueColor = toMaskInfo(input.opaqueColor);
  FsrMaskPolicy maskPolicy = {};
  const FsrMaskError maskError = FsrBuildMaskPolicy(maskInput, &maskPolicy);
  if (maskError != FsrMaskError::None)
    return fail(FsrMaskErrorString(maskError));

  FsrFrameParamsInput paramsInput = {};
  paramsInput.renderExtent = {render.width, render.height};
  paramsInput.outputExtent = {output.width, output.height};
  paramsInput.jitterPixels[0] = input.jitterPixels[0];
  paramsInput.jitterPixels[1] = input.jitterPixels[1];
  paramsInput.deltaTimeMs = input.deltaTimeMs;
  paramsInput.zNear = input.zNear;
  paramsInput.zFar = input.zFar;
  paramsInput.verticalFovRadians = input.verticalFovRadians;
  paramsInput.preExposure = input.preExposure;
  paramsInput.resetHistory = input.resetHistory;
  FsrDispatchParams dispatchParams = {};
  const FsrParamsError paramsError =
      FsrBuildDispatchParams(paramsInput, &dispatchParams);
  if (paramsError != FsrParamsError::None)
    return fail(FsrParamsErrorString(paramsError));

  const bool useReactiveMask = maskPolicy.bindReactiveMask;
  const bool useCompositionMask = maskPolicy.bindCompositionMask;
  const bool generateReactive = maskPolicy.enableAutoReactive;

  std::array<RITextureBarrier, 16> beginBarriers = {};
  uint32_t beginCount = 0;
  AppendBindingBarrier(&beginBarriers, &beginCount, input.color,
                       input.color.entryState, RI_RESOURCE_STATE_SHADER_RESOURCE,
                       RI_BARRIER_ASPECT_COLOR, RI_STAGE_NONE, RI_STAGE_COMPUTE);
  AppendBindingBarrier(&beginBarriers, &beginCount, input.motionVectors,
                       input.motionVectors.entryState,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_BARRIER_ASPECT_COLOR,
                       RI_STAGE_NONE, RI_STAGE_COMPUTE);
  AppendBindingBarrier(&beginBarriers, &beginCount, input.depth,
                       input.depth.entryState, RI_RESOURCE_STATE_SHADER_RESOURCE,
                       RI_BARRIER_ASPECT_DEPTH, RI_STAGE_NONE, RI_STAGE_COMPUTE);
  if (useReactiveMask)
    AppendBindingBarrier(&beginBarriers, &beginCount,
                         input.reactiveMaskJittered,
                         input.reactiveMaskJittered.entryState,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         RI_BARRIER_ASPECT_COLOR, RI_STAGE_NONE, RI_STAGE_COMPUTE);
  if (useCompositionMask)
    AppendBindingBarrier(&beginBarriers, &beginCount,
                         input.compositionMaskJittered,
                         input.compositionMaskJittered.entryState,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         RI_BARRIER_ASPECT_COLOR, RI_STAGE_NONE, RI_STAGE_COMPUTE);
  if (generateReactive)
    AppendBindingBarrier(&beginBarriers, &beginCount, input.opaqueColor,
                         input.opaqueColor.entryState,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         RI_BARRIER_ASPECT_COLOR, RI_STAGE_NONE, RI_STAGE_COMPUTE);
  AppendBindingBarrier(&beginBarriers, &beginCount, input.output,
                       input.output.entryState, RI_RESOURCE_STATE_GENERAL,
                       RI_BARRIER_ASPECT_COLOR, RI_STAGE_NONE, RI_STAGE_COMPUTE);
  AppendOwnedBarrier(&beginBarriers, &beginCount,
                     &m_impl->prepared.dilatedDepth,
                     RI_RESOURCE_STATE_GENERAL);
  AppendOwnedBarrier(&beginBarriers, &beginCount,
                     &m_impl->prepared.dilatedMotionVectors,
                     RI_RESOURCE_STATE_GENERAL);
  AppendOwnedBarrier(&beginBarriers, &beginCount,
                     &m_impl->prepared.reconstructedPrevNearestDepth,
                     RI_RESOURCE_STATE_GENERAL);
  if (depthIsCombined)
    AppendOwnedBarrier(&beginBarriers, &beginCount,
                       &m_impl->prepared.deviceDepth,
                       RI_RESOURCE_STATE_STORAGE_WRITE);
  if (generateReactive)
    AppendOwnedBarrier(&beginBarriers, &beginCount,
                       &m_impl->prepared.reactiveMask,
                       RI_RESOURCE_STATE_GENERAL);
  if (beginCount != 0)
    input.cmd->vk_d3d12_textureBarriers<16>(beginCount, beginBarriers.data());

  auto restoreBorrowed = [&]() {
    std::array<RITextureBarrier, 16> barriers = {};
    uint32_t count = 0;
    AppendBindingBarrier(&barriers, &count, input.color,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         input.color.exitState, RI_BARRIER_ASPECT_COLOR,
                         RI_STAGE_COMPUTE, RI_STAGE_NONE);
    AppendBindingBarrier(&barriers, &count, input.motionVectors,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         input.motionVectors.exitState, RI_BARRIER_ASPECT_COLOR,
                         RI_STAGE_COMPUTE, RI_STAGE_NONE);
    AppendBindingBarrier(&barriers, &count, input.depth,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         input.depth.exitState, RI_BARRIER_ASPECT_DEPTH,
                         RI_STAGE_COMPUTE, RI_STAGE_NONE);
    if (useReactiveMask)
      AppendBindingBarrier(&barriers, &count, input.reactiveMaskJittered,
                           RI_RESOURCE_STATE_SHADER_RESOURCE,
                           input.reactiveMaskJittered.exitState,
                           RI_BARRIER_ASPECT_COLOR, RI_STAGE_COMPUTE,
                           RI_STAGE_NONE);
    if (useCompositionMask)
      AppendBindingBarrier(&barriers, &count, input.compositionMaskJittered,
                           RI_RESOURCE_STATE_SHADER_RESOURCE,
                           input.compositionMaskJittered.exitState,
                           RI_BARRIER_ASPECT_COLOR, RI_STAGE_COMPUTE,
                           RI_STAGE_NONE);
    if (generateReactive)
      AppendBindingBarrier(&barriers, &count, input.opaqueColor,
                           RI_RESOURCE_STATE_SHADER_RESOURCE,
                           input.opaqueColor.exitState, RI_BARRIER_ASPECT_COLOR,
                           RI_STAGE_COMPUTE, RI_STAGE_NONE);
    AppendBindingBarrier(&barriers, &count, input.output,
                         RI_RESOURCE_STATE_GENERAL, input.output.exitState,
                         RI_BARRIER_ASPECT_COLOR, RI_STAGE_COMPUTE,
                         RI_STAGE_NONE);
    if (count != 0)
      input.cmd->vk_d3d12_textureBarriers<16>(count, barriers.data());
  };

  if (generateReactive) {
    FfxFsr3UpscalerGenerateReactiveDescription reactive = {};
    reactive.commandList = ffxGetCommandListVK(input.cmd->vk.cmd);
    reactive.colorOpaqueOnly = MakeExternalResource(
        input.opaqueColor, FFX_RESOURCE_STATE_COMPUTE_READ,
        FFX_RESOURCE_USAGE_READ_ONLY, L"FSR.opaqueColor");
    reactive.colorPreUpscale = MakeExternalResource(
        input.color, FFX_RESOURCE_STATE_COMPUTE_READ,
        FFX_RESOURCE_USAGE_READ_ONLY, L"FSR.color");
    reactive.outReactive = MakeOwnedResource(m_impl->prepared.reactiveMask,
                                             L"FSR.reactiveMask");
    reactive.renderSize = {render.width, render.height};
    reactive.scale = 1.0f;
    reactive.cutoffThreshold = 0.0f;
    reactive.binaryValue = 0.0f;
    reactive.flags = 0;
    if (ffxFsr3UpscalerContextGenerateReactiveMask(
            &m_impl->prepared.context, &reactive) != FFX_OK) {
      restoreBorrowed();
      return fail("reactive-mask generation failed");
    }
    RITextureBarrier reactiveRead(
        m_impl->prepared.reactiveMask.texture.Get(),
        RI_RESOURCE_STATE_GENERAL, RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_STAGE_COMPUTE, RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_COLOR);
    input.cmd->vk_d3d12_textureBarrier(reactiveRead);
    m_impl->prepared.reactiveMask.state = RI_RESOURCE_STATE_SHADER_RESOURCE;
  }

  if (depthIsCombined) {
    if (!m_impl->CopyDeviceDepth(input.depth, input.cmd, input.frameIndex)) {
      restoreBorrowed();
      return fail("device-depth copy dispatch failed");
    }

    RITextureBarrier deviceDepthRead(
        m_impl->prepared.deviceDepth.texture.Get(),
        RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_STAGE_COMPUTE, RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_COLOR);
    input.cmd->vk_d3d12_textureBarrier(deviceDepthRead);
    m_impl->prepared.deviceDepth.state = RI_RESOURCE_STATE_SHADER_RESOURCE;
  }

  FfxFsr3UpscalerDispatchDescription dispatch = {};
  dispatch.commandList = ffxGetCommandListVK(input.cmd->vk.cmd);
  dispatch.color = MakeExternalResource(input.color,
                                        FFX_RESOURCE_STATE_COMPUTE_READ,
                                        FFX_RESOURCE_USAGE_READ_ONLY,
                                        L"FSR.color");
  dispatch.depth = depthIsCombined
                       ? MakeOwnedResource(m_impl->prepared.deviceDepth,
                                           L"FSR.deviceDepth")
                       : MakeExternalResource(
                             input.depth, FFX_RESOURCE_STATE_COMPUTE_READ,
                             static_cast<FfxResourceUsage>(
                                 FFX_RESOURCE_USAGE_READ_ONLY |
                                 FFX_RESOURCE_USAGE_DEPTHTARGET),
                             L"FSR.depth");
  dispatch.motionVectors = MakeExternalResource(
      input.motionVectors, FFX_RESOURCE_STATE_COMPUTE_READ,
      FFX_RESOURCE_USAGE_READ_ONLY, L"FSR.motionVectors");
  dispatch.exposure = {};
  dispatch.reactive = useReactiveMask
                          ? MakeExternalResource(
                                input.reactiveMaskJittered,
                                FFX_RESOURCE_STATE_COMPUTE_READ,
                                FFX_RESOURCE_USAGE_READ_ONLY,
                                L"FSR.reactiveMask")
                          : (generateReactive
                                 ? MakeOwnedResource(m_impl->prepared.reactiveMask,
                                                     L"FSR.reactiveMask")
                                 : FfxResource{});
  dispatch.transparencyAndComposition = useCompositionMask
                                            ? MakeExternalResource(
                                                  input.compositionMaskJittered,
                                                  FFX_RESOURCE_STATE_COMPUTE_READ,
                                                  FFX_RESOURCE_USAGE_READ_ONLY,
                                                  L"FSR.compositionMask")
                                            : FfxResource{};
  dispatch.dilatedDepth = MakeOwnedResource(
      m_impl->prepared.dilatedDepth, L"FSR.dilatedDepth");
  dispatch.dilatedMotionVectors = MakeOwnedResource(
      m_impl->prepared.dilatedMotionVectors, L"FSR.dilatedMotionVectors");
  dispatch.reconstructedPrevNearestDepth = MakeOwnedResource(
      m_impl->prepared.reconstructedPrevNearestDepth,
      L"FSR.reconstructedPrevNearestDepth");
  dispatch.output = MakeExternalResource(
      input.output, FFX_RESOURCE_STATE_UNORDERED_ACCESS,
      FFX_RESOURCE_USAGE_UAV, L"FSR.output");
  // FsrBuildDispatchParams owns the sign conversion: the engine jitter moves
  // geometry by -jitter, so FSR receives the negated engine pixel offset.
  dispatch.jitterOffset = {dispatchParams.jitterX, dispatchParams.jitterY};
  dispatch.motionVectorScale = {dispatchParams.motionVectorScaleX,
                                dispatchParams.motionVectorScaleY};
  dispatch.renderSize = {dispatchParams.renderWidth, dispatchParams.renderHeight};
  dispatch.upscaleSize = {dispatchParams.upscaleWidth,
                          dispatchParams.upscaleHeight};
  dispatch.enableSharpening = dispatchParams.enableSharpening;
  dispatch.sharpness = dispatchParams.sharpness;
  dispatch.frameTimeDelta = dispatchParams.frameTimeDeltaMs;
  dispatch.preExposure = dispatchParams.preExposure;
  dispatch.reset = dispatchParams.reset;
  dispatch.cameraNear = dispatchParams.cameraNear;
  dispatch.cameraFar = dispatchParams.cameraFar;
  dispatch.cameraFovAngleVertical = dispatchParams.cameraFovAngleVertical;
  dispatch.viewSpaceToMetersFactor = 1.0f;
  dispatch.flags = 0;

  const FfxErrorCode dispatchResult = ffxFsr3UpscalerContextDispatch(
      &m_impl->prepared.context, &dispatch);
  restoreBorrowed();
  if (dispatchResult != FFX_OK)
    return fail("SDK dispatch failed");

  TemporalUpscalerOutput success = {};
  success.success = true;
  success.result = input.output;
  success.resultState = input.output.exitState;
  return success;
#else
  (void)render;
  (void)output;
  (void)input;
  Log("FSR: RecordResolve failed: FSR support is not compiled in\n");
  return failure;
#endif
}

} // namespace hpl
