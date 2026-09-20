#include "graphics/XessUpscaler.h"

#include "engine/Interface.h"
#include "graphics/Graphics.h"
#include "system/LowLevelSystem.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
#include "graphics/RICommand.h"
#include "graphics/RIDevice.h"
#include "graphics/RIVK.h"
#include "graphics/XessVulkanSupport.h"
#endif

namespace hpl {

namespace {

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

#endif

} // namespace

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

  const cXessVulkanSupport &support = XessVulkanSupportInstance();
  return support.IsAvailable() && graphics->device.xessAvailable;
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

  const cXessVulkanSupport &support = XessVulkanSupportInstance();
  if (!support.IsAvailable() || !graphics->device.xessAvailable)
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
    const char *reason = graphics && graphics->device.xessUnavailableReason[0]
                             ? graphics->device.xessUnavailableReason
                             : XessVulkanSupportInstance().UnavailableReason();
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
  if (!input.cmd || input.cmd->vk.cmd == VK_NULL_HANDLE)
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

  xess_vk_execute_params_t execute = {};
  if (!FillImageViewInfo(input.color, VK_IMAGE_ASPECT_COLOR_BIT,
                         &execute.colorTexture, "color") ||
      !FillImageViewInfo(input.motionVectors, VK_IMAGE_ASPECT_COLOR_BIT,
                         &execute.velocityTexture, "motion") ||
      !FillImageViewInfo(input.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                         &execute.depthTexture, "depth") ||
      !FillImageViewInfo(input.output, VK_IMAGE_ASPECT_COLOR_BIT,
                         &execute.outputTexture, "output"))
    return failure;
  if (useResponsiveMask &&
      !FillImageViewInfo(
          input.responsiveMaskUnjittered, VK_IMAGE_ASPECT_COLOR_BIT,
          &execute.responsivePixelMaskTexture, "responsive mask"))
    return failure;

  execute.jitterOffsetX = -input.jitterPixels[0];
  execute.jitterOffsetY = -input.jitterPixels[1];
  execute.exposureScale =
      input.preExposure > 0.0f ? 1.0f / input.preExposure : 1.0f;
  execute.resetHistory = input.resetHistory ? 1u : 0u;
  execute.inputWidth = render.width;
  execute.inputHeight = render.height;

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

  const cXessVulkanSupport &support = XessVulkanSupportInstance();
  const auto executeFn = support.VKExecute();
  if (!executeFn) {
    restoreInputs();
    return fail("xessVKExecute entry point is unavailable");
  }

  // The engine's motion is current-minus-previous unjittered UV. XeSS
  // consumes input-pixel motion pointing back to the previous frame; this
  // scale supplies the single sign/UV-to-pixel conversion.
  m_hasExecuted = true;
  const xess_result_t result =
      executeFn(m_context, input.cmd->vk.cmd, &execute);
  restoreInputs();
  if (result != XESS_RESULT_SUCCESS) {
    LogXessFailure("xessVKExecute", result);
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

  const cXessVulkanSupport &support = XessVulkanSupportInstance();
  if (!support.IsAvailable() || !graphics->device.xessAvailable) {
    Log("XeSS: context creation failed: device preflight is unavailable: %s\n",
        graphics->device.xessUnavailableReason[0]
            ? graphics->device.xessUnavailableReason
            : support.UnavailableReason());
    return false;
  }

  const auto createContext = support.VKCreateContext();
  if (!createContext) {
    Log("XeSS: context creation failed: xessVKCreateContext entry point is "
        "unavailable\n");
    return false;
  }

  xess_context_handle_t context = nullptr;
  const xess_result_t result = createContext(
      RIGetVkInstance(), graphics->device.physicalAdapter.vk.physicalDevice,
      graphics->device.vk.device, &context);
  if (result != XESS_RESULT_SUCCESS || !context) {
    LogXessFailure("xessVKCreateContext", result);
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
    Log("XeSS: xessVKInit failed: context or output extent is invalid\n");
    return false;
  }

  xess_quality_settings_t mappedQuality = XESS_QUALITY_SETTING_QUALITY;
  if (!MapQuality(quality, &mappedQuality)) {
    Log("XeSS: xessVKInit failed: quality has no XeSS mapping\n");
    return false;
  }

  const cXessVulkanSupport &support = XessVulkanSupportInstance();
  const auto init = support.VKInit();
  const auto setVelocityScale = support.SetVelocityScale();
  if (!init || !setVelocityScale) {
    Log("XeSS: xessVKInit failed: required XeSS entry point is unavailable\n");
    return false;
  }

  xess_vk_init_params_t params = {};
  params.outputResolution = {output.width, output.height};
  params.qualitySetting = mappedQuality;
  params.initFlags = initFlags;

  const xess_result_t initResult = init(m_context, &params);
  if (initResult != XESS_RESULT_SUCCESS) {
    LogXessFailure("xessVKInit", initResult);
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
  const auto destroyContext = XessVulkanSupportInstance().DestroyContext();
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
