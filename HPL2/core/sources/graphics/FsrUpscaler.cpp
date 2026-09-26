#include "graphics/FsrUpscaler.h"

#include "engine/Interface.h"
#include "graphics/Graphics.h"
#include "graphics/RIBarrier.h"
#include "graphics/RICommand.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIDevice.h"
#include "graphics/RID3D12.h"
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
// ffx-api rather than the FidelityFX SDK proper: the five entry points behind
// FfxApiLoader are backend-independent, so one adapter serves Vulkan and D3D12
// and the backend is picked by chaining a create-context descriptor.
#include "graphics/FfxApiLoader.h"

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#if (DEVICE_IMPL_VULKAN)
#include <ffx_api/vk/ffx_api_vk.h>
#endif
#if (DEVICE_IMPL_D3D12)
#include <ffx_api/dx12/ffx_api_dx12.h>
#endif
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

// ffxApiMessage. Only installed in debug builds, where setting it also turns on
// ffx-api's per-call descriptor validation.
static void FsrMessageCallback(uint32_t type, const wchar_t *message) {
  (void)type;
  const std::string converted =
      cString::To8Char(std::wstring(message != nullptr ? message : L""));
  Warning("FSR SDK: %s\n", converted.c_str());
}

static bool MapQuality(TemporalUpscalerQuality quality, uint32_t *mapped) {
  if (!mapped)
    return false;

  switch (quality) {
  case TemporalUpscalerQuality::NativeAA:
    *mapped = FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    return true;
  case TemporalUpscalerQuality::Quality:
    *mapped = FFX_UPSCALE_QUALITY_MODE_QUALITY;
    return true;
  case TemporalUpscalerQuality::Balanced:
    *mapped = FFX_UPSCALE_QUALITY_MODE_BALANCED;
    return true;
  case TemporalUpscalerQuality::Performance:
    *mapped = FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    return true;
  case TemporalUpscalerQuality::UltraPerformance:
    *mapped = FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
    return true;
  }
  return false;
}

static bool Is2DBinding(const TemporalUpscalerTextureBinding &binding) {
  // The FFX resource description has no view offset fields. Rejecting a partial
  // view is safer than registering the right image with metadata that silently
  // describes a different mip or array slice.
  //
  // The mipCount == 1 invariant is load-bearing on D3D12 in particular: the
  // description is derived there from the *resource*, so a view onto one mip of
  // a mipped resource would be described with the resource's full mip count.
  //
  // isEmpty() rather than a direct vk.image test: RI's Vulkan and D3D12 handles
  // share union storage, so reading vk.image while D3D12 is active reinterprets
  // an ID3D12Resource pointer as a VkImage and passes any null check.
  return binding.IsValid() && binding.mipOffset == 0 &&
         binding.mipCount == 1 && binding.layerOffset == 0 &&
         binding.layerCount == 1 && !binding.texture->isEmpty() &&
         !binding.view->isEmpty();
}

// Hand an RI texture to ffx-api as an FfxApiResource.
//
// The two backends are asymmetric and deliberately so. The Vulkan helper takes a
// description we build, because a VkImage carries no queryable metadata. The
// D3D12 helper derives the entire description from pRes->GetDesc() and treats
// `usage` as flags to OR onto what it derived, so passing our intent there is
// additive rather than authoritative.
//
// This is the only place the vk/d3d12 union members are read, and each read sits
// inside its own RIIsTargetSelected arm -- the members alias, so a read under
// the wrong backend silently reinterprets the other backend's pointer.
static FfxApiResource MakeApiResource(RITexture *texture, RI_Format_e format,
                                      TemporalUpscalerExtent extent,
                                      uint32_t mipCount, uint32_t usage,
                                      uint32_t state) {
  if (!texture)
    return FfxApiResource{};

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    FfxApiResourceDescription description = {};
    description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    description.format = ffxApiGetSurfaceFormatVK(RIFormatToVK(format));
    description.width = extent.width;
    description.height = extent.height;
    description.depth = 1;
    description.mipCount = mipCount;
    description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    description.usage = usage;
    return ffxApiGetResourceVK(reinterpret_cast<void *>(texture->vk.image),
                               description, state);
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    (void)format;
    (void)extent;
    (void)mipCount;
    return ffxApiGetResourceDX12(texture->d3d12.resource, state, usage);
  }
#endif
  assert(false && "unhandled backend");
  return FfxApiResource{};
}

static FfxApiResource MakeBindingResource(
    const TemporalUpscalerTextureBinding &binding, uint32_t usage,
    uint32_t state) {
  return MakeApiResource(binding.texture, binding.format, binding.extent,
                         binding.mipCount, usage, state);
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
  // The FFX usage bits this image was created for. The FFX-side state is NOT
  // cached alongside it: it is passed explicitly at every use, so it cannot
  // drift out of step with `state` the way a stored copy did.
  uint32_t apiUsage = FFX_API_RESOURCE_USAGE_READ_ONLY;

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
    apiUsage = FFX_API_RESOURCE_USAGE_READ_ONLY;
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
  image->texture->setDebugObjectName(&graphics->device, name ? name : "FSR.image");

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

// The state is an argument rather than a cached member on purpose. It has to
// agree with the RI barrier that immediately precedes the call, and a stored
// copy silently disagreed: AppendOwnedBarrier updates image.state but nothing
// updated the FFX-side state, so the reactive mask was handed to the upscale
// dispatch as UNORDERED_ACCESS after a barrier had already moved it to
// SHADER_RESOURCE.
static FfxApiResource MakeOwnedResource(const FsrImage &image, uint32_t state) {
  return MakeApiResource(image.texture.Get(), image.format, image.extent, 1,
                         image.apiUsage, state);
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

// Whether the live backend's device handles are usable, checked one backend arm
// at a time because the handles alias.
//
// This replaces the old capability probe, which built a whole FfxInterface just
// to read FfxDeviceCapabilities. ffx-api owns its backend and its scratch buffer
// internally and exposes neither, so shader-model and wave-size interrogation is
// no longer reachable without linking the SDK again. Anything it would have
// rejected now surfaces as an ffxCreateContext failure in PrepareContext, which
// the caller already handles by falling back to native resolution.
static bool DeviceIsUsable(cGraphics *graphics, std::string *reason) {
  if (!graphics) {
    if (reason)
      *reason = "graphics device is unavailable";
    return false;
  }

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (graphics->device.vk.device == VK_NULL_HANDLE ||
        graphics->device.physicalAdapter.vk.physicalDevice == VK_NULL_HANDLE) {
      if (reason)
        *reason = "Vulkan device is unavailable";
      return false;
    }
    return true;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (graphics->device.d3d12.device == nullptr) {
      if (reason)
        *reason = "Direct3D 12 device is unavailable";
      return false;
    }
    return true;
  }
#endif
  if (reason)
    *reason = "FSR has no backend for the active renderer";
  return false;
}

// Chain the backend create-context descriptor for the live backend onto
// `upscale`. The descriptors are caller-owned stack storage; ffx-api copies
// what it needs out of them inside ffxCreateContext.
static bool ChainBackendDesc(cGraphics *graphics,
                             ffxCreateContextDescUpscale *upscale,
#if (DEVICE_IMPL_VULKAN)
                             ffxCreateBackendVKDesc *vkBackend,
#endif
#if (DEVICE_IMPL_D3D12)
                             ffxCreateBackendDX12Desc *dx12Backend,
#endif
                             uint8_t *outBackendApi, std::string *reason) {
  if (!graphics || !upscale || !outBackendApi)
    return false;

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    *vkBackend = {};
    vkBackend->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    vkBackend->header.pNext = nullptr;
    vkBackend->vkDevice = graphics->device.vk.device;
    vkBackend->vkPhysicalDevice =
        graphics->device.physicalAdapter.vk.physicalDevice;
    // RIRenderer uses volk's global loader entry point after volkLoadDevice.
    vkBackend->vkDeviceProcAddr = vkGetDeviceProcAddr;
    upscale->header.pNext = &vkBackend->header;
    *outBackendApi = RI_DEVICE_API_VK;
    return true;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    *dx12Backend = {};
    dx12Backend->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    dx12Backend->header.pNext = nullptr;
    dx12Backend->device = graphics->device.d3d12.device;
    upscale->header.pNext = &dx12Backend->header;
    *outBackendApi = RI_DEVICE_API_D3D12;
    return true;
  }
#endif
  if (reason)
    *reason = "FSR has no backend for the active renderer";
  return false;
}

// The command list ffx-api records into. Passed through untouched to the SDK
// backend, so it is the raw VkCommandBuffer or ID3D12GraphicsCommandList.
static void *ActiveCommandList(RICmd *cmd) {
  if (!cmd)
    return nullptr;
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return reinterpret_cast<void *>(cmd->vk.cmd);
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return cmd->d3d12.cmdList;
#endif
  assert(false && "unhandled backend");
  return nullptr;
}

// ffx-api's D3D12 backend calls SetDescriptorHeaps and SetComputeRootSignature
// straight on the command list we hand it, leaving RI's redundancy caches
// naming bindings that are no longer there. Put the command list back under
// engine control after every dispatch, including the ones that failed -- a
// failed dispatch has still recorded commands.
static void RestoreEngineBindings(cGraphics *graphics, RICmd *cmd) {
#if (DEVICE_IMPL_D3D12)
  if (graphics && cmd && RIIsTargetSelected(RI_DEVICE_API_D3D12))
    RID3D12_RestoreCachedBindings(graphics->device, *cmd);
#else
  (void)graphics;
  (void)cmd;
#endif
}

#endif // defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE

} // namespace

struct cFsrUpscaler::Impl {
  explicit Impl(cGraphics *owner) : graphics(owner) {}

  cGraphics *graphics = nullptr;

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
  struct PreparedState {
    // ffxContext is an opaque void*; the module owns the context object and the
    // backend scratch behind it, and frees both in ffxDestroyContext. The three
    // shared history textures the SDK path used to require the engine to create
    // (dilatedDepth, dilatedMotionVectors, reconstructedPrevNearestDepth) are
    // likewise allocated and released inside the module now.
    ffxContext context = nullptr;
    bool contextCreated = false;
    // The module this context belongs to. FfxApiFor caches one entry per
    // backend for the life of the process, so holding a pointer is stable, and
    // it keeps every later call on this context -- dispatch included -- going
    // through the same module that created it.
    const hpl::FfxApi *api = nullptr;
    // Captured at creation, NOT re-derived when the defer drains. This lambda
    // runs frames later, and in a build with both backends compiled in the
    // active backend can have changed by then -- destroying a context through
    // the other module's entry point walks into the wrong provider table.
    PfnFfxDestroyContext destroyContext = nullptr;
    std::shared_ptr<RIProgram> copyDeviceDepthProgram;
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
      if (state.contextCreated && state.destroyContext) {
        const ffxReturnCode_t result =
            state.destroyContext(&state.context, nullptr);
        if (result != FFX_API_RETURN_OK)
          Log("FSR: context destruction failed (%d)\n",
              static_cast<int>(result));
        state.contextCreated = false;
      }
      return;
    }

    // Destroying the context also releases the backend scratch and the shared
    // history textures, all of which live inside the module.
    auto parked = std::make_shared<PreparedState>(std::move(state));
    RIDevice *device = &graphics->device;
    graphics->graphicsDefer.push(std::function<void()>(
        [parked, device]() mutable {
          // destroyContext is the pointer captured when the context was made,
          // never FfxApiActive() re-read here; see PreparedState.
          if (parked->contextCreated && parked->destroyContext) {
            const ffxReturnCode_t result =
                parked->destroyContext(&parked->context, nullptr);
            if (result != FFX_API_RETURN_OK)
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
          resources->GetFileSearcher(), "CopyDeviceDepth.cs");
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

    const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, 0x43445054u);
    prepared.copyDeviceDepthProgram->bindComputePipeline(
        &graphics->device, cmd, pipelineHash, "Temporal.CopyDeviceDepth.cs");

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
    if (!prepared.contextCreated && !prepared.copyDeviceDepthProgram &&
        !prepared.deviceDepth.texture && !prepared.reactiveMask.texture)
      return;
    DeferPrepared(std::move(prepared));
    // Aggregate assignment from {} creates a full PreparedState temporary on
    // the stack; when this is called while PrepareContext also has a candidate
    // alive, Windows' default 1 MiB stack overflows in __chkstk. Reconstruct
    // the heap-resident Impl member directly instead.
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
  uint32_t mappedQuality = FFX_UPSCALE_QUALITY_MODE_QUALITY;
  if (!MapQuality(quality, &mappedQuality)) {
    m_impl->LogSupportFailure("quality has no FSR mapping");
    return false;
  }
  (void)mappedQuality;

  std::string reason;
  if (!DeviceIsUsable(m_impl->graphics, &reason)) {
    m_impl->LogSupportFailure(reason.c_str());
    return false;
  }

  // Whether the module for the live backend loaded is the remaining gate, and
  // it is a cheap one: resolving it creates no GPU context and records nothing,
  // so this stays callable from the options menu.
  //
  // The old probe built a whole FfxInterface here purely to read back
  // FfxDeviceCapabilities and check shader model, wave32 and format support.
  // ffx-api owns its backend and scratch internally and exposes neither, so
  // none of that is reachable any more. Anything it would have rejected now
  // surfaces as an ffxCreateContext failure in PrepareContext, which the caller
  // already handles by falling back to native resolution.
  const hpl::FfxApi &api = hpl::FfxApiActive();
  if (!api.available) {
    m_impl->LogSupportFailure(api.unavailableReason);
    return false;
  }
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
  uint32_t mappedQuality = FFX_UPSCALE_QUALITY_MODE_QUALITY;
  if (!MapQuality(quality, &mappedQuality)) {
    Log("FSR: invalid quality for resolution query\n");
    return {};
  }

  const hpl::FfxApi &api = hpl::FfxApiActive();
  if (!api.available) {
    Log("FSR: resolution query failed: %s\n", api.unavailableReason);
    return {};
  }

  uint32_t renderWidth = 0;
  uint32_t renderHeight = 0;
  // A null context is the documented form for the queries that describe the
  // effect rather than a live context; ffx-api picks the provider from the
  // descriptor type.
  ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query = {};
  query.header.type =
      FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
  query.header.pNext = nullptr;
  query.displayWidth = output.width;
  query.displayHeight = output.height;
  query.qualityMode = mappedQuality;
  query.pOutRenderWidth = &renderWidth;
  query.pOutRenderHeight = &renderHeight;
  const ffxReturnCode_t result = api.Query(nullptr, &query.header);
  if (result != FFX_API_RETURN_OK || renderWidth == 0 || renderHeight == 0 ||
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
  const hpl::FfxApi &api = hpl::FfxApiActive();
  if (!api.available) {
    Log("FSR: jitter query failed: %s\n", api.unavailableReason);
    return 0;
  }

  int32_t phaseCount = 0;
  ffxQueryDescUpscaleGetJitterPhaseCount query = {};
  query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  query.header.pNext = nullptr;
  query.renderWidth = render.width;
  query.displayWidth = output.width;
  query.pOutPhaseCount = &phaseCount;
  if (api.Query(nullptr, &query.header) != FFX_API_RETURN_OK || phaseCount <= 0) {
    Log("FSR: SDK returned zero jitter phases\n");
    return 0;
  }
  return static_cast<uint32_t>(phaseCount);
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

  uint32_t mappedQuality = FFX_UPSCALE_QUALITY_MODE_QUALITY;
  if (!MapQuality(settings.quality, &mappedQuality)) {
    Log("FSR: PrepareContext failed: quality has no SDK mapping\n");
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }
  (void)mappedQuality;

  std::string reason;
  if (!DeviceIsUsable(m_impl->graphics, &reason)) {
    Log("FSR: PrepareContext failed: %s\n", reason.c_str());
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }

  const hpl::FfxApi &api = hpl::FfxApiActive();
  if (!api.available) {
    Log("FSR: PrepareContext failed: %s\n", api.unavailableReason);
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }

  ffxCreateContextDescUpscale description = {};
  description.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  description.header.pNext = nullptr;
  // No FFX_UPSCALE_ENABLE_AUTO_EXPOSURE: the scene exposure is a fixed
  // constant applied at tonemap, after FSR. Auto exposure metered these dark,
  // candle-lit frames and chased every flame flicker, which pulsed the
  // accumulated history. With no exposure texture FSR uses a stable 1.0.
  description.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
#if !defined(NDEBUG)
  description.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
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

  // The backend descriptor is chained onto the create-context descriptor; this
  // is what selects Vulkan or D3D12 inside the module. Both live on this stack
  // frame and ffx-api copies what it needs during the call.
#if (DEVICE_IMPL_VULKAN)
  ffxCreateBackendVKDesc vkBackend = {};
#endif
#if (DEVICE_IMPL_D3D12)
  ffxCreateBackendDX12Desc dx12Backend = {};
#endif
  uint8_t backendApi = 0;
  if (!ChainBackendDesc(m_impl->graphics, &description,
#if (DEVICE_IMPL_VULKAN)
                        &vkBackend,
#endif
#if (DEVICE_IMPL_D3D12)
                        &dx12Backend,
#endif
                        &backendApi, &reason)) {
    Log("FSR: PrepareContext failed: %s\n", reason.c_str());
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }

  const ffxReturnCode_t contextResult =
      api.CreateContext(&candidate->context, &description.header, nullptr);
  if (contextResult != FFX_API_RETURN_OK) {
    Log("FSR: PrepareContext failed: SDK context creation error %d\n",
        static_cast<int>(contextResult));
    m_impl->DeferPrepared(std::move(*candidate));
    return false;
  }
  candidate->contextCreated = true;
  // Captured now so neither dispatch nor the deferred teardown can reach for a
  // different module's entry point later; see PreparedState::destroyContext.
  candidate->api = &api;
  candidate->destroyContext = api.DestroyContext;

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
  // MakeOwnedResource rebuilds the rest of the description from the image's
  // own format and extent each time, and takes the resource state from the
  // caller so it cannot drift from the barrier that precedes the call. Only
  // the usage intent is fixed per image, so that is all that is stored.
  candidate->deviceDepth.apiUsage = FFX_API_RESOURCE_USAGE_READ_ONLY;
  candidate->reactiveMask.apiUsage = FFX_API_RESOURCE_USAGE_UAV;

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
  // isEmpty() rather than a direct vk.cmd test: the Vulkan and D3D12 command
  // handles share union storage, so reading vk.cmd under D3D12 reinterprets an
  // ID3D12GraphicsCommandList pointer as a VkCommandBuffer.
  if (!input.cmd || input.cmd->isEmpty())
    return fail("command buffer is null");
  // TemporalPresentation calls providers after closing its dynamic-rendering
  // scope. RI has no public query for that scope, so the command-buffer
  // contract is asserted here and remains explicit at the call boundary.
  assert(!input.cmd->isEmpty());

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
  // The dilated depth, dilated motion vector and reconstructed previous
  // nearest depth history surfaces are allocated inside the module now, so it
  // owns their state transitions too and the engine must not barrier them.
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
    ffxDispatchDescUpscaleGenerateReactiveMask reactive = {};
    reactive.header.type =
        FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
    reactive.header.pNext = nullptr;
    reactive.commandList = ActiveCommandList(input.cmd);
    reactive.colorOpaqueOnly = MakeBindingResource(
        input.opaqueColor, FFX_API_RESOURCE_USAGE_READ_ONLY,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    reactive.colorPreUpscale = MakeBindingResource(
        input.color, FFX_API_RESOURCE_USAGE_READ_ONLY,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    reactive.outReactive = MakeOwnedResource(
        m_impl->prepared.reactiveMask, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    reactive.renderSize = {render.width, render.height};
    reactive.scale = 1.0f;
    reactive.cutoffThreshold = 0.0f;
    reactive.binaryValue = 0.0f;
    reactive.flags = 0;
    const ffxReturnCode_t reactiveResult = m_impl->prepared.api->Dispatch(
        &m_impl->prepared.context, &reactive.header);
    RestoreEngineBindings(m_impl->graphics, input.cmd);
    if (reactiveResult != FFX_API_RETURN_OK) {
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

  // dilatedDepth, dilatedMotionVectors and reconstructedPrevNearestDepth are
  // absent by design: ffx-api allocates and binds those history surfaces inside
  // the module, so the engine neither creates nor passes them any more.
  ffxDispatchDescUpscale dispatch = {};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.header.pNext = nullptr;
  dispatch.commandList = ActiveCommandList(input.cmd);
  dispatch.color = MakeBindingResource(input.color,
                                       FFX_API_RESOURCE_USAGE_READ_ONLY,
                                       FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.depth =
      depthIsCombined
          ? MakeOwnedResource(m_impl->prepared.deviceDepth,
                              FFX_API_RESOURCE_STATE_COMPUTE_READ)
          : MakeBindingResource(input.depth,
                                FFX_API_RESOURCE_USAGE_READ_ONLY |
                                    FFX_API_RESOURCE_USAGE_DEPTHTARGET,
                                FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.motionVectors =
      MakeBindingResource(input.motionVectors, FFX_API_RESOURCE_USAGE_READ_ONLY,
                          FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.exposure = {};
  dispatch.reactive =
      useReactiveMask
          ? MakeBindingResource(input.reactiveMaskJittered,
                                FFX_API_RESOURCE_USAGE_READ_ONLY,
                                FFX_API_RESOURCE_STATE_COMPUTE_READ)
          : (generateReactive
                 ? MakeOwnedResource(m_impl->prepared.reactiveMask,
                                     FFX_API_RESOURCE_STATE_COMPUTE_READ)
                 : FfxApiResource{});
  dispatch.transparencyAndComposition =
      useCompositionMask
          ? MakeBindingResource(input.compositionMaskJittered,
                                FFX_API_RESOURCE_USAGE_READ_ONLY,
                                FFX_API_RESOURCE_STATE_COMPUTE_READ)
          : FfxApiResource{};
  dispatch.output =
      MakeBindingResource(input.output, FFX_API_RESOURCE_USAGE_UAV,
                          FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
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

  const ffxReturnCode_t dispatchResult = m_impl->prepared.api->Dispatch(
      &m_impl->prepared.context, &dispatch.header);
  RestoreEngineBindings(m_impl->graphics, input.cmd);
  restoreBorrowed();
  if (dispatchResult != FFX_API_RETURN_OK)
    return fail("SDK dispatch failed");

  TemporalUpscalerOutput success = {};
  success.success = true;
  success.result = input.output;
  success.resultState = input.output.exitState;
  success.resultStage = input.output.exitStage;
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
