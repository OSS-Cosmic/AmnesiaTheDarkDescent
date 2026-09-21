#include "graphics/XessUpscaler.h"

#include "engine/Interface.h"
#include "graphics/Graphics.h"
#include "graphics/RIDevice.h" // RIActiveBackendApi, RIIsTargetSelected
#include "system/LowLevelSystem.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdio.h>
#include <string.h>

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
#include "graphics/RICommand.h"
#include "graphics/RID3D12.h"
#include "graphics/RIVK.h"
#ifdef _WIN32
#include <windows.h>
#endif
#endif

namespace hpl {

namespace {

static void CopyReason(char *destination, const char *reason) {
  if (!reason || !reason[0])
    reason = "unspecified reason";
  strncpy(destination, reason, 127);
  destination[127] = '\0';
}

static bool SameExtent(TemporalUpscalerExtent a, TemporalUpscalerExtent b) {
  return a.width == b.width && a.height == b.height;
}

static bool IsNonZeroExtent(TemporalUpscalerExtent extent) {
  return extent.width != 0 && extent.height != 0;
}

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE

static bool MapQuality(TemporalUpscalerQuality quality,
                       xess_quality_settings_t *mapped) {
  if (!mapped)
    return false;

  switch (quality) {
  case TemporalUpscalerQuality::NativeAA:
    *mapped = XESS_QUALITY_SETTING_AA;
    return true;
  case TemporalUpscalerQuality::Quality:
    *mapped = XESS_QUALITY_SETTING_QUALITY;
    return true;
  case TemporalUpscalerQuality::Balanced:
    *mapped = XESS_QUALITY_SETTING_BALANCED;
    return true;
  case TemporalUpscalerQuality::Performance:
    *mapped = XESS_QUALITY_SETTING_PERFORMANCE;
    return true;
  case TemporalUpscalerQuality::UltraPerformance:
    *mapped = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    return true;
  }
  return false;
}

static void LogXessFailure(const char *operation, xess_result_t result) {
  Log("XeSS: %s failed with result %d\n", operation, static_cast<int>(result));
}

static bool Is2DBinding(const TemporalUpscalerTextureBinding &binding) {
  return binding.IsValid() && binding.mipCount == 1 &&
         binding.layerCount == 1 &&
         binding.mipOffset <= std::numeric_limits<uint16_t>::max() &&
         binding.layerOffset <= std::numeric_limits<uint16_t>::max();
}

#if (DEVICE_IMPL_VULKAN)
// Vulkan needs the whole description spelled out: a VkImage carries no
// queryable metadata, so image, view, aspect, subresource range, format and
// extent all have to be supplied. The D3D12 arm below is the mirror image of
// this -- XeSS derives all of it from ID3D12Resource::GetDesc().
static bool FillImageViewInfo(const TemporalUpscalerTextureBinding &binding,
                              VkImageAspectFlags aspect,
                              xess_vk_image_view_info *info, const char *name) {
  if (!info || !Is2DBinding(binding) || binding.texture->isEmpty() ||
      binding.view->isEmpty() || binding.texture->vk.image == VK_NULL_HANDLE ||
      binding.view->vk.image == VK_NULL_HANDLE) {
    Log("XeSS: invalid %s image/view binding (XeSS requires a 2D view and "
        "one mip/layer)\n",
        name);
    return false;
  }

  *info = {};
  info->imageView = binding.view->vk.image;
  info->image = binding.texture->vk.image;
  info->subresourceRange.aspectMask = aspect;
  info->subresourceRange.baseMipLevel = binding.mipOffset;
  info->subresourceRange.levelCount = binding.mipCount;
  info->subresourceRange.baseArrayLayer = binding.layerOffset;
  info->subresourceRange.layerCount = binding.layerCount;
  info->format = RIFormatToVK(binding.format);
  info->width = binding.extent.width;
  info->height = binding.extent.height;
  if (info->format == VK_FORMAT_UNDEFINED) {
    Log("XeSS: invalid %s image/view binding: RI format has no Vulkan "
        "mapping\n",
        name);
    return false;
  }
  return true;
}
#endif

#if (DEVICE_IMPL_D3D12)
// D3D12 takes the bare resource: XeSS reads format, extent and mip count from
// GetDesc() and builds its own descriptors, so there is no view, aspect or
// format to hand over and nothing to mismatch.
//
// The engine's depth resource is created TYPELESS (R32G8X24_TYPELESS, see
// ri_d3d12_depth_typeless_format) because it is both a depth target and
// sampled. That is exactly the form XeSS accepts for depth, so the combined
// depth/stencil format needs no intermediate copy here -- unlike FSR, whose
// ffx-api backend wants a plain R32_FLOAT surface.
static ID3D12Resource *
BindingResource(const TemporalUpscalerTextureBinding &binding,
                const char *name) {
  // isEmpty() rather than a direct d3d12.resource test: the Vulkan and D3D12
  // handles share union storage, so a raw member read under the wrong backend
  // reinterprets the other backend's pointer and passes any null check.
  if (!Is2DBinding(binding) || binding.texture->isEmpty() ||
      binding.texture->d3d12.resource == nullptr) {
    Log("XeSS: invalid %s resource binding (XeSS requires a 2D texture with "
        "one mip/layer)\n",
        name);
    return nullptr;
  }
  return binding.texture->d3d12.resource;
}
#endif

static void AppendTextureBarrier(std::array<RITextureBarrier, 5> *barriers,
                                 uint32_t *count,
                                 const TemporalUpscalerTextureBinding &binding,
                                 uint32_t before, uint32_t after,
                                 uint32_t beforeStages, uint32_t afterStages,
                                 RIBarrierAspect_e aspect) {
  if (!barriers || !count)
    return;

  RITextureBarrier barrier(binding.texture, before, after, beforeStages,
                           afterStages, aspect);
  barrier.baseMip = static_cast<uint16_t>(binding.mipOffset);
  barrier.mipCount = static_cast<uint16_t>(binding.mipCount);
  barrier.baseLayer = static_cast<uint16_t>(binding.layerOffset);
  barrier.layerCount = static_cast<uint16_t>(binding.layerCount);
  (*barriers)[(*count)++] = barrier;
}

// xessD3D12Execute calls SetDescriptorHeaps and SetComputeRootSignature
// straight on the command list we hand it, leaving RI's redundancy caches
// naming bindings that are no longer there. Put the command list back under
// engine control afterwards, on every path that reached the execute.
static void RestoreEngineBindings(cGraphics *graphics, RICmd *cmd) {
#if (DEVICE_IMPL_D3D12)
  if (graphics && cmd && RIIsTargetSelected(RI_DEVICE_API_D3D12))
    RID3D12_RestoreCachedBindings(graphics->device, *cmd);
#else
  (void)graphics;
  (void)cmd;
#endif
}

#endif

} // namespace

cXessSupport::cXessSupport()
    : m_moduleAvailable(false), m_moduleUnavailableReason{},
      m_vkAvailable(false), m_vkUnavailableReason{}, m_d3d12Available(false),
      m_d3d12UnavailableReason{} {
  CopyReason(m_moduleUnavailableReason, "XeSS is unavailable");
  CopyReason(m_vkUnavailableReason, "XeSS is unavailable");
  CopyReason(m_d3d12UnavailableReason, "XeSS is unavailable");

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  m_library = NULL;
  m_destroyContext = NULL;
  m_getOptimalInputResolution = NULL;
  m_setVelocityScale = NULL;
  m_setJitterScale = NULL;
  m_setExposureMultiplier = NULL;
  m_getProperties = NULL;
  m_isOptimalDriver = NULL;
  m_getVersion = NULL;
  m_setLoggingCallback = NULL;
#if (DEVICE_IMPL_VULKAN)
  m_vkGetRequiredInstanceExtensions = NULL;
  m_vkGetRequiredDeviceExtensions = NULL;
  m_vkGetRequiredDeviceFeatures = NULL;
  m_vkCreateContext = NULL;
  m_vkInit = NULL;
  m_vkExecute = NULL;
#endif
#if (DEVICE_IMPL_D3D12)
  m_d3d12CreateContext = NULL;
  m_d3d12Init = NULL;
  m_d3d12Execute = NULL;
#endif

#ifdef _WIN32
  m_library = (void *)LoadLibraryA("libxess.dll");
  if (!m_library) {
    const char *reason = "XeSS DLL libxess.dll could not be loaded";
    CopyReason(m_moduleUnavailableReason, reason);
    CopyReason(m_vkUnavailableReason, reason);
    CopyReason(m_d3d12UnavailableReason, reason);
    return;
  }

#define XESS_RESOLVE(member, name)                                             \
  member = reinterpret_cast<decltype(member)>(                                 \
      GetProcAddress(static_cast<HMODULE>(m_library), name))

  // A missing symbol here is fatal for every backend: these are the entry
  // points both arms call.
#define XESS_LOAD_SHARED(member, name)                                         \
  XESS_RESOLVE(member, name);                                                  \
  if (!member) {                                                               \
    char reason[128];                                                          \
    snprintf(reason, sizeof(reason), "XeSS DLL symbol %s is missing", name);   \
    CopyReason(m_moduleUnavailableReason, reason);                             \
    CopyReason(m_vkUnavailableReason, reason);                                 \
    CopyReason(m_d3d12UnavailableReason, reason);                              \
    return;                                                                    \
  }

  XESS_LOAD_SHARED(m_destroyContext, "xessDestroyContext");
  XESS_LOAD_SHARED(m_getOptimalInputResolution, "xessGetOptimalInputResolution");
  XESS_LOAD_SHARED(m_setVelocityScale, "xessSetVelocityScale");
  XESS_LOAD_SHARED(m_setJitterScale, "xessSetJitterScale");
  XESS_LOAD_SHARED(m_setExposureMultiplier, "xessSetExposureMultiplier");
  XESS_LOAD_SHARED(m_getProperties, "xessGetProperties");
  XESS_LOAD_SHARED(m_isOptimalDriver, "xessIsOptimalDriver");
  XESS_LOAD_SHARED(m_getVersion, "xessGetVersion");
  XESS_LOAD_SHARED(m_setLoggingCallback, "xessSetLoggingCallback");

#undef XESS_LOAD_SHARED
  m_moduleAvailable = true;
  m_moduleUnavailableReason[0] = '\0';

  // Past this point a missing symbol disables only the backend that needs it.
  // The two backends are resolved independently so that a build serving one of
  // them is never taken down by the other's entry points.
#if (DEVICE_IMPL_VULKAN)
  XESS_RESOLVE(m_vkGetRequiredInstanceExtensions,
               "xessVKGetRequiredInstanceExtensions");
  XESS_RESOLVE(m_vkGetRequiredDeviceExtensions,
               "xessVKGetRequiredDeviceExtensions");
  XESS_RESOLVE(m_vkGetRequiredDeviceFeatures,
               "xessVKGetRequiredDeviceFeatures");
  XESS_RESOLVE(m_vkCreateContext, "xessVKCreateContext");
  XESS_RESOLVE(m_vkInit, "xessVKInit");
  XESS_RESOLVE(m_vkExecute, "xessVKExecute");
  if (m_vkGetRequiredInstanceExtensions && m_vkGetRequiredDeviceExtensions &&
      m_vkGetRequiredDeviceFeatures && m_vkCreateContext && m_vkInit &&
      m_vkExecute) {
    m_vkAvailable = true;
    m_vkUnavailableReason[0] = '\0';
  } else {
    CopyReason(m_vkUnavailableReason,
               "XeSS DLL symbol xessVK* is missing");
  }
#else
  CopyReason(m_vkUnavailableReason,
             "this build has no XeSS backend for Vulkan");
#endif

#if (DEVICE_IMPL_D3D12)
  XESS_RESOLVE(m_d3d12CreateContext, "xessD3D12CreateContext");
  XESS_RESOLVE(m_d3d12Init, "xessD3D12Init");
  XESS_RESOLVE(m_d3d12Execute, "xessD3D12Execute");
  if (m_d3d12CreateContext && m_d3d12Init && m_d3d12Execute) {
    m_d3d12Available = true;
    m_d3d12UnavailableReason[0] = '\0';
  } else {
    CopyReason(m_d3d12UnavailableReason,
               "XeSS DLL symbol xessD3D12* is missing");
  }
#else
  CopyReason(m_d3d12UnavailableReason,
             "this build has no XeSS backend for Direct3D 12");
#endif

#undef XESS_RESOLVE
#else
  const char *reason = "XeSS is only available on Windows";
  CopyReason(m_moduleUnavailableReason, reason);
  CopyReason(m_vkUnavailableReason, reason);
  CopyReason(m_d3d12UnavailableReason, reason);
#endif
#else
  const char *reason = "XeSS support is not compiled into this build";
  CopyReason(m_moduleUnavailableReason, reason);
  CopyReason(m_vkUnavailableReason, reason);
  CopyReason(m_d3d12UnavailableReason, reason);
#endif
}

cXessSupport::~cXessSupport() {
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE && defined(_WIN32)
  if (m_library)
    FreeLibrary(static_cast<HMODULE>(m_library));
#endif
}

bool cXessSupport::IsAvailableFor(uint8_t backendApi) const {
  if (!m_moduleAvailable)
    return false;
  switch (backendApi) {
  case RI_DEVICE_API_VK:
    return m_vkAvailable;
  case RI_DEVICE_API_D3D12:
    return m_d3d12Available;
  default:
    return false;
  }
}

const char *cXessSupport::UnavailableReasonFor(uint8_t backendApi) const {
  if (!m_moduleAvailable)
    return m_moduleUnavailableReason;
  switch (backendApi) {
  case RI_DEVICE_API_VK:
    return m_vkUnavailableReason;
  case RI_DEVICE_API_D3D12:
    return m_d3d12UnavailableReason;
  default:
    return "XeSS has no backend for the active renderer";
  }
}

bool cXessSupport::IsAvailable() const {
  return IsAvailableFor(RIActiveBackendApi());
}

const char *cXessSupport::UnavailableReason() const {
  return UnavailableReasonFor(RIActiveBackendApi());
}

void cXessSupport::SetVulkanUnavailable(const char *reason) {
  m_vkAvailable = false;
  CopyReason(m_vkUnavailableReason, reason);
}

cXessSupport &XessSupportInstance() {
  static cXessSupport support;
  return support;
}

#if (DEVICE_IMPL_VULKAN)
namespace {

// RI's rejection callback for both contributions below. The loader is a
// process-lifetime singleton, so there is nothing to carry in userData.
void XessRequirementRejected(void *userData, const char *reason) {
  (void)userData;
  XessSupportInstance().SetVulkanUnavailable(reason);
}

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
// The SDK's feature query needs the instance and adapter, which RI does not
// pass to mergeFeatureChain, so they ride along in userData. One device is
// created per process, so a single instance of this suffices; it lives at file
// scope because it has to outlive the RIDeviceDesc that points at it.
struct XessDeviceQueryTarget {
  VkInstance instance;
  VkPhysicalDevice physicalDevice;
};
XessDeviceQueryTarget g_deviceQueryTarget = {VK_NULL_HANDLE, VK_NULL_HANDLE};

// Wraps xessVKGetRequiredDeviceFeatures for RIVkDeviceRequirements. RI seeds
// *featureChain with its own chain head and the SDK returns the merged head.
bool XessMergeFeatureChain(void *userData, void **featureChain) {
  const XessDeviceQueryTarget *target =
      static_cast<const XessDeviceQueryTarget *>(userData);
  const auto getRequiredDeviceFeatures =
      XessSupportInstance().VKGetRequiredDeviceFeatures();
  if (!getRequiredDeviceFeatures || !target)
    return false;
  const xess_result_t result = getRequiredDeviceFeatures(
      target->instance, target->physicalDevice, featureChain);
  if (result != XESS_RESULT_SUCCESS) {
    // RI only learns that the query failed, so the SDK's code is logged here
    // or it is lost.
    Log("XeSS: required device feature query failed (%d)\n", (int)result);
    return false;
  }
  return true;
}
#endif

} // namespace

bool XessVkInstanceRequirements(RIVkInstanceRequirements *out) {
  if (!out)
    return false;
  *out = {};
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  cXessSupport &support = XessSupportInstance();
  // The renderer has not selected a backend yet, so ask for the Vulkan verdict
  // specifically rather than the active-backend one.
  if (!support.IsAvailableFor(RI_DEVICE_API_VK)) {
    Log("XeSS: instance preflight unavailable: %s\n",
        support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }

  const auto getRequiredInstanceExtensions =
      support.VKGetRequiredInstanceExtensions();
  if (!getRequiredInstanceExtensions) {
    support.SetVulkanUnavailable(
        "required instance query entry point is unavailable");
    Log("XeSS: %s\n", support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }

  uint32_t extensionCount = 0;
  uint32_t minVkApiVersion = 0;
  const char *const *extensionNames = NULL;
  const xess_result_t result = getRequiredInstanceExtensions(
      &extensionCount, &extensionNames, &minVkApiVersion);
  if (result != XESS_RESULT_SUCCESS) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "required instance extension query failed (%d)", (int)result);
    support.SetVulkanUnavailable(reason);
    Log("XeSS: %s\n", support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }
  if (extensionCount && !extensionNames) {
    support.SetVulkanUnavailable(
        "required instance extension query returned no names");
    Log("XeSS: %s\n", support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }

  // The names belong to the SDK module, which the loader keeps resident for the
  // process lifetime, so borrowing them across InitRIRenderer is safe.
  out->debugName = "XeSS";
  out->extensionNames = extensionNames;
  out->extensionCount = extensionCount;
  out->minInstanceApiVersion = minVkApiVersion;
  out->onRejected = XessRequirementRejected;
  return true;
#else
  return false;
#endif
}

bool XessVkDeviceRequirements(RIVkDeviceRequirements *out, VkInstance instance,
                              VkPhysicalDevice physicalDevice) {
  if (!out)
    return false;
  *out = {};
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  cXessSupport &support = XessSupportInstance();
  if (!support.IsAvailableFor(RI_DEVICE_API_VK)) {
    Log("XeSS: device preflight unavailable: %s\n",
        support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }
  if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE) {
    support.SetVulkanUnavailable(
        "device preflight has no instance or adapter to query");
    Log("XeSS: %s\n", support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }

  const auto getRequiredDeviceExtensions =
      support.VKGetRequiredDeviceExtensions();
  if (!getRequiredDeviceExtensions || !support.VKGetRequiredDeviceFeatures()) {
    support.SetVulkanUnavailable(
        "required device query entry point is unavailable");
    Log("XeSS: %s\n", support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }

  uint32_t extensionCount = 0;
  const char *const *extensionNames = NULL;
  const xess_result_t result = getRequiredDeviceExtensions(
      instance, physicalDevice, &extensionCount, &extensionNames);
  if (result != XESS_RESULT_SUCCESS) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "required device extension query failed (%d)", (int)result);
    support.SetVulkanUnavailable(reason);
    Log("XeSS: %s\n", support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }
  if (extensionCount && !extensionNames) {
    support.SetVulkanUnavailable(
        "required device extension query returned no names");
    Log("XeSS: %s\n", support.UnavailableReasonFor(RI_DEVICE_API_VK));
    return false;
  }

  g_deviceQueryTarget.instance = instance;
  g_deviceQueryTarget.physicalDevice = physicalDevice;

  out->debugName = "XeSS";
  out->userData = &g_deviceQueryTarget;
  out->extensionNames = extensionNames;
  out->extensionCount = extensionCount;
  out->mergeFeatureChain = XessMergeFeatureChain;
  out->onRejected = XessRequirementRejected;
  return true;
#else
  (void)instance;
  (void)physicalDevice;
  return false;
#endif
}
#endif // DEVICE_IMPL_VULKAN

bool cXessUpscaler::DeviceIsUsable(cGraphics *graphics, const char **reason) {
  if (!graphics) {
    if (reason)
      *reason = "no graphics device";
    return false;
  }
  // The vk/d3d12 union members alias, so each read sits inside its own
  // RIIsTargetSelected arm: a read under the wrong backend would silently
  // reinterpret the other backend's pointer.
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
    *reason = "XeSS has no backend for the active renderer";
  return false;
}

cXessUpscaler::cXessUpscaler(cGraphics *graphics) : mpGraphics(graphics) {}

cXessUpscaler::~cXessUpscaler() {
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  RetireContext();
#endif
}

cGraphics *cXessUpscaler::Graphics() const {
  return mpGraphics ? mpGraphics : Interface<cGraphics>::Get();
}

bool cXessUpscaler::Supports(TemporalUpscalerProvider provider,
                             TemporalUpscalerQuality quality) const {
  if (provider != TemporalUpscalerProvider::XeSS)
    return false;

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  xess_quality_settings_t mappedQuality = XESS_QUALITY_SETTING_QUALITY;
  cGraphics *graphics = Graphics();
  if (!MapQuality(quality, &mappedQuality) || !graphics)
    return false;

  const cXessSupport &support = XessSupportInstance();
  return support.IsAvailable() && DeviceIsUsable(graphics, nullptr);
#else
  (void)quality;
  return false;
#endif
}

TemporalUpscalerExtent cXessUpscaler::GetRecommendedRenderExtent(
    TemporalUpscalerExtent output, TemporalUpscalerQuality quality) const {
  if (!IsNonZeroExtent(output))
    return {};

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  xess_quality_settings_t mappedQuality = XESS_QUALITY_SETTING_QUALITY;
  cGraphics *graphics = Graphics();
  if (!MapQuality(quality, &mappedQuality) || !graphics)
    return output;

  const cXessSupport &support = XessSupportInstance();
  if (!support.IsAvailable() || !DeviceIsUsable(graphics, nullptr))
    return output;

  if (!EnsureContextForQuery())
    return output;

  xess_2d_t outputResolution = {output.width, output.height};
  xess_2d_t optimal = {};
  xess_2d_t minimum = {};
  xess_2d_t maximum = {};
  const auto getOptimal = support.GetOptimalInputResolution();
  if (!getOptimal) {
    Log("XeSS: xessGetOptimalInputResolution entry point is unavailable; "
        "using native render extent\n");
    return output;
  }

  const xess_result_t result =
      getOptimal(m_context, &outputResolution, mappedQuality, &optimal,
                 &minimum, &maximum);
  if (result != XESS_RESULT_SUCCESS || optimal.x == 0 || optimal.y == 0) {
    LogXessFailure("xessGetOptimalInputResolution", result);
    return output;
  }

  return {optimal.x, optimal.y};
#else
  (void)quality;
  return output;
#endif
}

uint32_t
cXessUpscaler::GetJitterPhaseCount(TemporalUpscalerExtent render,
                                   TemporalUpscalerExtent output) const {
  if (!IsNonZeroExtent(render) || !IsNonZeroExtent(output))
    return 8;

  // XeSS's guide scales the base eight Halton samples with the square of the
  // upscale factor.  Use the width ratio as the guide does; the optimal input
  // and output extents preserve aspect ratio.  The minimum keeps native and
  // downscaled configurations on the ordinary eight-phase sequence.
  const double upscale =
      static_cast<double>(output.width) / static_cast<double>(render.width);
  const double requested = std::ceil(8.0 * upscale * upscale);
  if (requested <= 8.0)
    return 8;
  if (requested >= static_cast<double>(std::numeric_limits<uint32_t>::max()))
    return std::numeric_limits<uint32_t>::max();
  return static_cast<uint32_t>(requested);
}

bool cXessUpscaler::PrepareContext(const TemporalUpscalerSettings &settings,
                                   TemporalUpscalerExtent render,
                                   TemporalUpscalerExtent output,
                                   cGraphics::FrameContext *frame) {
  if (!frame) {
    Log("XeSS: PrepareContext failed: frame context is null\n");
    return false;
  }
  if (!IsNonZeroExtent(render) || !IsNonZeroExtent(output)) {
    Log("XeSS: PrepareContext failed: render/output extent is zero\n");
    return false;
  }

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  if (!Supports(settings.provider, settings.quality)) {
    cGraphics *graphics = Graphics();
    // A missing device is the more specific complaint, so prefer it; otherwise
    // the loader's verdict says why XeSS itself is out.
    const char *reason = nullptr;
    if (DeviceIsUsable(graphics, &reason))
      reason = XessSupportInstance().UnavailableReason();
    Log("XeSS: PrepareContext failed: provider/quality/device is unavailable: "
        "%s\n",
        reason ? reason : "unknown reason");
    RetireContext();
    return false;
  }

  if (m_contextInitialized && SameExtent(m_preparedRender, render) &&
      SameExtent(m_preparedOutput, output) &&
      m_preparedQuality == settings.quality) {
    return true;
  }

  // A changed extent or quality invalidates XeSS's internal history. Retire
  // the complete old context behind the graphics timeline before creating the
  // replacement; xessVKInit is not a substitute for this GPU-safe lifetime
  // boundary.
  if (m_context)
    RetireContext();

  if (!EnsureContextForQuery()) {
    Log("XeSS: PrepareContext failed: could not create a XeSS context\n");
    return false;
  }

  if (!InitializeContext(render, output, settings.quality,
                         XESS_INIT_FLAG_NONE)) {
    Log("XeSS: PrepareContext failed: XeSS initialization did not complete\n");
    RetireContext();
    return false;
  }

  m_preparedRender = render;
  m_preparedOutput = output;
  m_preparedQuality = settings.quality;
  return true;
#else
  (void)settings;
  (void)render;
  (void)output;
  Log("XeSS: PrepareContext failed: XeSS support is not compiled into this "
      "build\n");
  return false;
#endif
}

TemporalUpscalerOutput
cXessUpscaler::RecordResolve(TemporalUpscalerExtent render,
                             TemporalUpscalerExtent output,
                             const TemporalUpscalerFrameInput &input) {
  TemporalUpscalerOutput failure = {};

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  auto fail = [&failure](const char *reason) {
    Log("XeSS: RecordResolve failed: %s\n", reason);
    return failure;
  };

  if (!m_context || !m_contextInitialized)
    return fail("context was not prepared");
  if (!IsNonZeroExtent(render) || !IsNonZeroExtent(output) ||
      !SameExtent(render, m_preparedRender) ||
      !SameExtent(output, m_preparedOutput))
    return fail("render/output extent does not match the prepared context");
  // No backend gate here: EnsureContextForQuery and InitializeContext both
  // reject a renderer XeSS has no arm for, and RecordResolve is only reached
  // once they have succeeded.
  //
  // isEmpty() rather than a direct vk.cmd test: the Vulkan and D3D12 command
  // handles share union storage, so reading vk.cmd under D3D12 reinterprets an
  // ID3D12GraphicsCommandList pointer as a VkCommandBuffer.
  if (!input.cmd || input.cmd->isEmpty())
    return fail("command buffer is null");
  if (!input.color.IsValid() || !input.depth.IsValid() ||
      !input.motionVectors.IsValid() || !input.output.IsValid())
    return fail("one or more required texture bindings are invalid");
  if (!SameExtent(input.color.extent, render) ||
      !SameExtent(input.depth.extent, render) ||
      !SameExtent(input.motionVectors.extent, render) ||
      !SameExtent(input.output.extent, output))
    return fail("binding extent does not match the resolve extent");
  if (input.color.format != cGraphics::PogoColorFormat ||
      input.output.format != cGraphics::PogoColorFormat)
    return fail("HDR color/output must be RGBA16F");
  if (input.motionVectors.format != cGraphics::VelocityFormat)
    return fail("motion vectors must be input-resolution RG16F");
  if (input.depth.format != cGraphics::DepthFormat)
    return fail("depth format does not match the engine depth resource");
  if (!Is2DBinding(input.color) || !Is2DBinding(input.depth) ||
      !Is2DBinding(input.motionVectors) || !Is2DBinding(input.output))
    return fail("XeSS requires one-mip, one-layer 2D bindings");
  if (input.responsiveMaskUnjittered.IsValid() &&
      (!SameExtent(input.responsiveMaskUnjittered.extent, render) ||
       !Is2DBinding(input.responsiveMaskUnjittered)))
    return fail(
        "responsive mask is not a one-mip, one-layer render-sized view");
  if (input.jitterPixels[0] < -0.5f || input.jitterPixels[0] > 0.5f ||
      input.jitterPixels[1] < -0.5f || input.jitterPixels[1] > 0.5f)
    return fail("jitter is outside the contract's [-0.5, 0.5] range");

  // The contract exposes a caller-owned optional mask. XeSS consumes that
  // mask as a read-only input, so configure the context exactly when a valid
  // caller binding is present. Initialization is repeated only before the
  // first execute; changing this flag after commands have been recorded would
  // violate xessVKInit's pending-command restriction.
  const bool useResponsiveMask = input.responsiveMaskUnjittered.IsValid();
  if (useResponsiveMask != m_responsiveMaskEnabled) {
    if (m_hasExecuted)
      return fail("responsive-mask availability changed after XeSS executed");
    const uint32_t responsiveMaskFlag =
        static_cast<uint32_t>(XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK);
    const uint32_t flags = useResponsiveMask
                               ? (m_initFlags | responsiveMaskFlag)
                               : (m_initFlags & ~responsiveMaskFlag);
    if (!InitializeContext(m_preparedRender, m_preparedOutput,
                           m_preparedQuality, flags)) {
      RetireContext();
      return fail("XeSS re-initialization for the responsive mask failed");
    }
  }

  // The scalar tail is identical in both execute-params structs, so it is
  // written once through a generic lambda rather than kept in step by hand.
  const auto fillSharedParams = [&](auto &params) {
    params.jitterOffsetX = -input.jitterPixels[0];
    params.jitterOffsetY = -input.jitterPixels[1];
    params.exposureScale =
        input.preExposure > 0.0f ? 1.0f / input.preExposure : 1.0f;
    params.resetHistory = input.resetHistory ? 1u : 0u;
    params.inputWidth = render.width;
    params.inputHeight = render.height;
  };

  // Both live on this stack frame; the arms below fill exactly one. Resource
  // binding is validated here, before any barrier is recorded, so a malformed
  // binding leaves the command list untouched.
#if (DEVICE_IMPL_VULKAN)
  xess_vk_execute_params_t vkExecute = {};
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (!FillImageViewInfo(input.color, VK_IMAGE_ASPECT_COLOR_BIT,
                           &vkExecute.colorTexture, "color") ||
        !FillImageViewInfo(input.motionVectors, VK_IMAGE_ASPECT_COLOR_BIT,
                           &vkExecute.velocityTexture, "motion") ||
        !FillImageViewInfo(input.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                           &vkExecute.depthTexture, "depth") ||
        !FillImageViewInfo(input.output, VK_IMAGE_ASPECT_COLOR_BIT,
                           &vkExecute.outputTexture, "output"))
      return failure;
    if (useResponsiveMask &&
        !FillImageViewInfo(
            input.responsiveMaskUnjittered, VK_IMAGE_ASPECT_COLOR_BIT,
            &vkExecute.responsivePixelMaskTexture, "responsive mask"))
      return failure;
    fillSharedParams(vkExecute);
  }
#endif
#if (DEVICE_IMPL_D3D12)
  xess_d3d12_execute_params_t d3d12Execute = {};
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    d3d12Execute.pColorTexture = BindingResource(input.color, "color");
    d3d12Execute.pVelocityTexture =
        BindingResource(input.motionVectors, "motion");
    d3d12Execute.pDepthTexture = BindingResource(input.depth, "depth");
    d3d12Execute.pOutputTexture = BindingResource(input.output, "output");
    if (!d3d12Execute.pColorTexture || !d3d12Execute.pVelocityTexture ||
        !d3d12Execute.pDepthTexture || !d3d12Execute.pOutputTexture)
      return failure;
    if (useResponsiveMask) {
      d3d12Execute.pResponsivePixelMaskTexture =
          BindingResource(input.responsiveMaskUnjittered, "responsive mask");
      if (!d3d12Execute.pResponsivePixelMaskTexture)
        return failure;
    }
    // pDescriptorHeap/descriptorHeapOffset stay null: they are only read under
    // XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP, which this context never sets.
    fillSharedParams(d3d12Execute);
  }
#endif

  std::array<RITextureBarrier, 5> beginBarriers = {};
  uint32_t beginCount = 0;
  AppendTextureBarrier(&beginBarriers, &beginCount, input.color,
                       input.color.entryState,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                       RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_COLOR);
  AppendTextureBarrier(&beginBarriers, &beginCount, input.depth,
                       input.depth.entryState,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                       RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_DEPTH);
  AppendTextureBarrier(&beginBarriers, &beginCount, input.motionVectors,
                       input.motionVectors.entryState,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                       RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_COLOR);
  if (useResponsiveMask)
    AppendTextureBarrier(&beginBarriers, &beginCount,
                         input.responsiveMaskUnjittered,
                         input.responsiveMaskUnjittered.entryState,
                         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                         RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_COLOR);
  AppendTextureBarrier(&beginBarriers, &beginCount, input.output,
                       input.output.entryState, RI_RESOURCE_STATE_GENERAL,
                       RI_STAGE_NONE, RI_STAGE_COMPUTE,
                       RI_BARRIER_ASPECT_COLOR);
  if (beginCount != 0)
    input.cmd->vk_d3d12_textureBarriers<5>(beginCount, beginBarriers.data());

  const auto restoreInputs = [&]() {
    std::array<RITextureBarrier, 5> endBarriers = {};
    uint32_t endCount = 0;
    AppendTextureBarrier(&endBarriers, &endCount, input.color,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         input.color.exitState, RI_STAGE_COMPUTE, RI_STAGE_NONE,
                         RI_BARRIER_ASPECT_COLOR);
    AppendTextureBarrier(&endBarriers, &endCount, input.depth,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         input.depth.exitState, RI_STAGE_COMPUTE, RI_STAGE_NONE,
                         RI_BARRIER_ASPECT_DEPTH);
    AppendTextureBarrier(&endBarriers, &endCount, input.motionVectors,
                         RI_RESOURCE_STATE_SHADER_RESOURCE,
                         input.motionVectors.exitState, RI_STAGE_COMPUTE,
                         RI_STAGE_NONE, RI_BARRIER_ASPECT_COLOR);
    if (useResponsiveMask)
      AppendTextureBarrier(
          &endBarriers, &endCount, input.responsiveMaskUnjittered,
          RI_RESOURCE_STATE_SHADER_RESOURCE,
          input.responsiveMaskUnjittered.exitState, RI_STAGE_COMPUTE,
          RI_STAGE_NONE, RI_BARRIER_ASPECT_COLOR);
    // xessVKExecute leaves its output in GENERAL.  The declared exit state is
    // the presentation contract, so publish that state before returning.
    AppendTextureBarrier(&endBarriers, &endCount, input.output,
                         RI_RESOURCE_STATE_GENERAL, input.output.exitState,
                         RI_STAGE_COMPUTE, RI_STAGE_NONE,
                         RI_BARRIER_ASPECT_COLOR);
    if (endCount != 0)
      input.cmd->vk_d3d12_textureBarriers<5>(endCount, endBarriers.data());
  };

  const cXessSupport &support = XessSupportInstance();

  // The engine's motion is current-minus-previous unjittered UV. XeSS
  // consumes input-pixel motion pointing back to the previous frame; the
  // single sign/UV-to-pixel conversion is supplied by the velocity scale set
  // in InitializeContext.
  const char *entryPoint = nullptr;
  xess_result_t result = XESS_RESULT_ERROR_UNSUPPORTED_DEVICE;

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    const auto executeFn = support.VKExecute();
    if (!executeFn) {
      restoreInputs();
      return fail("xessVKExecute entry point is unavailable");
    }
    entryPoint = "xessVKExecute";
    m_hasExecuted = true;
    result = executeFn(m_context, input.cmd->vk.cmd, &vkExecute);
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    const auto executeFn = support.D3D12Execute();
    if (!executeFn) {
      restoreInputs();
      return fail("xessD3D12Execute entry point is unavailable");
    }
    entryPoint = "xessD3D12Execute";
    m_hasExecuted = true;
    result = executeFn(m_context, input.cmd->d3d12.cmdList, &d3d12Execute);
    RestoreEngineBindings(Graphics(), input.cmd);
  }
#endif

  if (!entryPoint) {
    restoreInputs();
    return fail("XeSS has no backend for the active renderer");
  }
  restoreInputs();
  if (result != XESS_RESULT_SUCCESS) {
    LogXessFailure(entryPoint, result);
    return failure;
  }

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
  Log("XeSS: RecordResolve failed: XeSS support is not compiled into this "
      "build\n");
  return failure;
#endif
}

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE

bool cXessUpscaler::EnsureContextForQuery() const {
  if (m_context)
    return true;

  cGraphics *graphics = Graphics();
  if (!graphics) {
    Log("XeSS: context creation failed: graphics interface is unavailable\n");
    return false;
  }

  const cXessSupport &support = XessSupportInstance();
  const char *deviceReason = nullptr;
  if (!support.IsAvailable() || !DeviceIsUsable(graphics, &deviceReason)) {
    Log("XeSS: context creation failed: device preflight is unavailable: %s\n",
        deviceReason ? deviceReason : support.UnavailableReason());
    return false;
  }

  // Each arm reads only its own backend's device handles; the union members
  // alias, so a read under the wrong backend reinterprets the other backend's
  // pointer. D3D12 needs just the device -- there is no instance or physical
  // adapter to pass, and no extension negotiation behind it.
  xess_context_handle_t context = nullptr;
  const char *entryPoint = nullptr;
  xess_result_t result = XESS_RESULT_ERROR_UNSUPPORTED_DEVICE;

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    const auto createContext = support.VKCreateContext();
    if (!createContext) {
      Log("XeSS: context creation failed: xessVKCreateContext entry point is "
          "unavailable\n");
      return false;
    }
    entryPoint = "xessVKCreateContext";
    result = createContext(RIGetVkInstance(),
                           graphics->device.physicalAdapter.vk.physicalDevice,
                           graphics->device.vk.device, &context);
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    const auto createContext = support.D3D12CreateContext();
    if (!createContext) {
      Log("XeSS: context creation failed: xessD3D12CreateContext entry point "
          "is unavailable\n");
      return false;
    }
    if (graphics->device.d3d12.device == nullptr) {
      Log("XeSS: context creation failed: Direct3D 12 device is unavailable\n");
      return false;
    }
    entryPoint = "xessD3D12CreateContext";
    result = createContext(graphics->device.d3d12.device, &context);
  }
#endif

  if (!entryPoint) {
    Log("XeSS: context creation failed: XeSS has no backend for the active "
        "renderer\n");
    return false;
  }
  if (result != XESS_RESULT_SUCCESS || !context) {
    LogXessFailure(entryPoint, result);
    return false;
  }

  m_context = context;
  return true;
}

bool cXessUpscaler::InitializeContext(TemporalUpscalerExtent render,
                                      TemporalUpscalerExtent output,
                                      TemporalUpscalerQuality quality,
                                      uint32_t initFlags) {
  if (!m_context || !IsNonZeroExtent(render) || !IsNonZeroExtent(output)) {
    Log("XeSS: initialization failed: context or output extent is invalid\n");
    return false;
  }

  xess_quality_settings_t mappedQuality = XESS_QUALITY_SETTING_QUALITY;
  if (!MapQuality(quality, &mappedQuality)) {
    Log("XeSS: initialization failed: quality has no XeSS mapping\n");
    return false;
  }

  const cXessSupport &support = XessSupportInstance();
  const auto setVelocityScale = support.SetVelocityScale();
  if (!setVelocityScale) {
    Log("XeSS: initialization failed: xessSetVelocityScale entry point is "
        "unavailable\n");
    return false;
  }

  // The two init-params structs agree on the only three fields set here; they
  // differ solely in the handle types of the temp-heap and pipeline-cache tail,
  // which is left zero on both backends.
  const char *entryPoint = nullptr;
  xess_result_t initResult = XESS_RESULT_ERROR_UNSUPPORTED_DEVICE;

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    const auto init = support.VKInit();
    if (!init) {
      Log("XeSS: initialization failed: xessVKInit entry point is "
          "unavailable\n");
      return false;
    }
    xess_vk_init_params_t params = {};
    params.outputResolution = {output.width, output.height};
    params.qualitySetting = mappedQuality;
    params.initFlags = initFlags;
    entryPoint = "xessVKInit";
    initResult = init(m_context, &params);
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    const auto init = support.D3D12Init();
    if (!init) {
      Log("XeSS: initialization failed: xessD3D12Init entry point is "
          "unavailable\n");
      return false;
    }
    xess_d3d12_init_params_t params = {};
    params.outputResolution = {output.width, output.height};
    params.qualitySetting = mappedQuality;
    params.initFlags = initFlags;
    entryPoint = "xessD3D12Init";
    initResult = init(m_context, &params);
  }
#endif

  if (!entryPoint) {
    Log("XeSS: initialization failed: XeSS has no backend for the active "
        "renderer\n");
    return false;
  }
  if (initResult != XESS_RESULT_SUCCESS) {
    LogXessFailure(entryPoint, initResult);
    return false;
  }

  // Engine motion is UV current-minus-previous.  XeSS wants pixel motion
  // from current back to previous, so negate and scale once per context
  // initialization/extent change. Jitter remains an independent execution
  // parameter and does not affect this conversion.
  const xess_result_t velocityResult =
      setVelocityScale(m_context, -static_cast<float>(render.width),
                       -static_cast<float>(render.height));
  if (velocityResult != XESS_RESULT_SUCCESS) {
    LogXessFailure("xessSetVelocityScale", velocityResult);
    return false;
  }

  m_contextInitialized = true;
  m_responsiveMaskEnabled =
      (initFlags & XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK) != 0;
  m_initFlags = initFlags;
  return true;
}

void cXessUpscaler::RetireContext() {
  xess_context_handle_t context = m_context;
  if (!context)
    return;

  cGraphics *graphics = Graphics();
  const auto destroyContext = XessSupportInstance().DestroyContext();
  if (!graphics || !destroyContext) {
    Log("XeSS: context retirement failed: graphics deferral or destroy entry "
        "point is unavailable; retaining the live context\n");
    return;
  }

  m_context = nullptr;
  m_contextInitialized = false;
  m_responsiveMaskEnabled = false;
  m_hasExecuted = false;
  m_initFlags = XESS_INIT_FLAG_NONE;
  m_preparedRender = {};
  m_preparedOutput = {};

  // The loader is a process-lifetime singleton. Capturing the typed function
  // pointer keeps this retirement independent of the adapter object while the
  // context remains parked behind the graphics timeline.
  graphics->graphicsDefer.push(
      std::function<void()>([context, destroyContext]() {
        const xess_result_t result = destroyContext(context);
        if (result != XESS_RESULT_SUCCESS)
          LogXessFailure("xessDestroyContext", result);
      }));
}

#endif

} // namespace hpl
