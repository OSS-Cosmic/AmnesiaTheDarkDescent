#include "graphics/TemporalPresentation.h"

#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgram.h"
#include "graphics/RIGpuProfiler.h"
#include "graphics/RIVK.h"
#include "scene/Viewport.h"
#include "system/Hasher.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <utility>

#include "system/LowLevelSystem.h"

namespace hpl {

namespace {

struct ResolveDepthPushConstants {
  uint32_t renderExtent[2];
  uint32_t displayExtent[2];
  float jitterPixels[2];
};

struct SpatialFallbackPushConstants {
  uint32_t renderExtent[2];
  uint32_t displayExtent[2];
  uint32_t validRectMin[2];
  uint32_t validRectMax[2];
  float jitterPixels[2];
};

static_assert(sizeof(ResolveDepthPushConstants) == 24,
              "ResolveDepthPC must match the Slang push-constant layout");
static_assert(sizeof(SpatialFallbackPushConstants) == 40,
              "SpatialFallbackPC must match the Slang push-constant layout");

static bool SameExtent(TemporalUpscalerExtent a, TemporalUpscalerExtent b) {
  return a.width == b.width && a.height == b.height;
}

static bool IsNonZeroExtent(TemporalUpscalerExtent extent) {
  return extent.width != 0 && extent.height != 0;
}

static bool IsContainedRect(const TemporalPresentationRect &rect,
                            TemporalUpscalerExtent extent) {
  return rect.IsValid() &&
         static_cast<uint64_t>(rect.x) + rect.width <= extent.width &&
         static_cast<uint64_t>(rect.y) + rect.height <= extent.height;
}

static uint32_t SwapchainImageCount(cGraphics *graphics) {
  if (!graphics || !graphics->swapchain)
    return 0;
  return std::min<uint32_t>(graphics->swapchain->imageCount,
                            RI_MAX_SWAPCHAIN_IMAGES);
}

static void RejectProviderResult(TemporalPresentationResult &result) {
  result.colorProduced = false;
  result.color = {};
  result.colorState = RI_RESOURCE_STATE_UNDEFINED;
  result.colorEntryStage = RI_STAGE_NONE;
  result.colorExitStage = RI_STAGE_NONE;
  result.colorIsSpatialFallback = false;
  result.providerFailed = true;
}

} // namespace

cTemporalPresentation::cTemporalPresentation(cFileSearcher *shaderFiles)
    : mpShaderFiles(shaderFiles) {}

cTemporalPresentation::~cTemporalPresentation() { Release(nullptr); }

bool cTemporalPresentation::HasOwnedResources() const {
  return m_resourceExtentValid || m_displayDepthAllocated ||
         m_displayColorAllocated || static_cast<bool>(mpResolveDepth) ||
         static_cast<bool>(mpSpatialFallback);
}

void cTemporalPresentation::InvalidateDisplayDepth() {
  m_displayDepthValid = false;
  m_displayDepthExtent = {};
  m_displayDepthFrameIndex = 0;
}

void cTemporalPresentation::LatchProviderFailure() {
  if (!m_providerFailureReported) {
    Log("TemporalPresentation: temporal provider failed; the next frame "
        "must use native extent and zero jitter\n");
    m_providerFailureReported = true;
  }
  m_providerFailureLatched = true;
  // Resolve invalidates the prior frame at entry. A depth resolve completed
  // earlier in this call remains valid even when the provider fails; the
  // caller can use it alongside the explicitly marked spatial color.
}

bool cTemporalPresentation::EnsureResolveDepthProgram() {
  if (mpResolveDepth)
    return true;
  if (!mpShaderFiles)
    return false;

  // This is the same two-entry-point, one-SPIR-V load used by VBufferRaster:
  // reflection creates the sampler/texture descriptor set and push-constant
  // layout, while RIProgram owns the backend pipeline layout.
  auto resolveDepthBin =
      RIProgram::loadShaderStage(mpShaderFiles, "ResolveDepth.3d");
  if (resolveDepthBin.empty())
    return false;

  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, resolveDepthBin,
                             "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, resolveDepthBin,
                             "psMain"}};

  auto program = std::make_shared<RIProgram>();
  program->initialize(&Interface<cGraphics>::Get()->device, stages, {},
                      "Temporal.ResolveDepth.3d");
  mpResolveDepth = std::move(program);
  return true;
}

bool cTemporalPresentation::EnsureSpatialFallbackProgram() {
  if (mpSpatialFallback)
    return true;
  if (!mpShaderFiles)
    return false;

  auto spatialFallbackBin =
      RIProgram::loadShaderStage(mpShaderFiles, "SpatialFallback.3d");
  if (spatialFallbackBin.empty())
    return false;

  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX,
                             spatialFallbackBin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT,
                             spatialFallbackBin, "psMain"}};

  auto program = std::make_shared<RIProgram>();
  program->initialize(&Interface<cGraphics>::Get()->device, stages, {},
                      "Temporal.SpatialFallback.3d");
  mpSpatialFallback = std::move(program);
  return true;
}

bool cTemporalPresentation::EnsureDisplayDepth(
    TemporalUpscalerExtent extent, cGraphics::FrameContext *frameContext) {
  cGraphics *graphics = Interface<cGraphics>::Get();
  const uint32_t imageCount = SwapchainImageCount(graphics);
  if (!graphics || imageCount == 0 || !IsNonZeroExtent(extent))
    return false;

  if (m_resourceExtentValid && !SameExtent(m_resourceExtent, extent))
    Release(frameContext);
  if (!m_resourceExtentValid) {
    m_resourceExtent = extent;
    m_resourceExtentValid = true;
    m_resourceImageCount = imageCount;
  } else if (m_resourceImageCount != imageCount) {
    Release(frameContext);
    m_resourceExtent = extent;
    m_resourceExtentValid = true;
    m_resourceImageCount = imageCount;
  }

  if (m_displayDepthAllocated)
    return true;

  for (uint32_t i = 0; i < imageCount; ++i) {
    // The combined D32S8 attachment is retained because outline uses its
    // stencil aspect; RI transitions depth and stencil independently.
    if (!CreateViewportAttachmentTexture(
            &graphics->device, extent.width, extent.height,
            cGraphics::DepthFormat,
            RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
            RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT, &m_displayDepthTextures[i],
            &m_displayDepthViews[i], "TemporalPresentation.displayDepth")) {
      Release(frameContext);
      return false;
    }

    // A combined depth+stencil view cannot be sampled on Vulkan. This second
    // view addresses the same image but selects only its depth aspect, which
    // is the view ResolveDepth and later temporal providers bind.
    RITextureViewDesc sampleDesc = {};
    sampleDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
    sampleDesc.format = cGraphics::DepthFormat;
    sampleDesc.mipNum = 1;
    sampleDesc.layerNum = 1;
    RITextureView sampleView = RITextureView::create(
        &graphics->device, m_displayDepthTextures[i].Get(), sampleDesc);
    m_displayDepthSampleViews[i] =
        RISharedPointer<RITextureView>(&graphics->device, sampleView);
    if (m_displayDepthSampleViews[i].isEmpty()) {
      Release(frameContext);
      return false;
    }
  }

  m_displayDepthAllocated = true;
  return true;
}

bool cTemporalPresentation::EnsureDisplayColor(
    TemporalUpscalerExtent extent, cGraphics::FrameContext *frameContext) {
  cGraphics *graphics = Interface<cGraphics>::Get();
  const uint32_t imageCount = SwapchainImageCount(graphics);
  if (!graphics || imageCount == 0 || !IsNonZeroExtent(extent))
    return false;

  if (m_resourceExtentValid && !SameExtent(m_resourceExtent, extent))
    Release(frameContext);
  if (!m_resourceExtentValid) {
    m_resourceExtent = extent;
    m_resourceExtentValid = true;
    m_resourceImageCount = imageCount;
  } else if (m_resourceImageCount != imageCount) {
    Release(frameContext);
    m_resourceExtent = extent;
    m_resourceExtentValid = true;
    m_resourceImageCount = imageCount;
  }

  if (m_displayColorAllocated)
    return true;

  const auto releaseColorAllocations = [&]() {
    for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
      graphics->graphicsDefer.push(std::move(m_displayColorViews[i]));
      graphics->graphicsDefer.push(std::move(m_displayColorTextures[i]));
    }
    m_displayColorAllocated = false;
  };

  for (uint32_t i = 0; i < imageCount; ++i) {
    // The provider writes this image as storage, while the fallback and the
    // viewport's later feed blit use it as a color attachment and shader
    // resource. It is not a native-path target.
    if (!CreateViewportColorTexture(
            &graphics->device, extent.width, extent.height,
            cGraphics::PogoColorFormat,
            RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE_STORAGE |
                RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_SRC,
            &m_displayColorTextures[i], &m_displayColorViews[i],
            "TemporalPresentation.displayColor")) {
      // Keep an already-recorded display depth valid for this call. Only the
      // incomplete color allocation is discarded; the caller will see the
      // provider failure and must not consume any stale color target.
      releaseColorAllocations();
      return false;
    }
  }

  m_displayColorAllocated = true;
  return true;
}

bool cTemporalPresentation::RecordDepthResolve(
    const TemporalPresentationFrameInput &input) {
  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics || !input.cmd || !mpResolveDepth ||
      !m_displayDepthAllocated || !input.scene.renderDepthTexture ||
      !input.scene.renderDepthSampleView ||
      !input.scene.renderDepthAttachmentView ||
      input.renderExtent.width == 0 || input.renderExtent.height == 0 ||
      input.displayExtent.width == 0 || input.displayExtent.height == 0 ||
      graphics->swapchainIndex >= m_resourceImageCount)
    return false;

  auto sampler = graphics->resolve_filter_descriptor(
      eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
      eTextureWrap_ClampToEdge, eTextureFilter_Nearest);
  if (!sampler)
    return false;

  RIGpuScope _s(&graphics->profiler, input.cmd, "TemporalDepthResolve");

  const uint32_t imageIndex = graphics->swapchainIndex;
  // The scene renderer produced the render depth for its read-only depth
  // consumer. ResolveDepth's fragment shader is the next consumer, so expose
  // only the depth aspect as SHADER_RESOURCE for this draw.
  RITextureBarrier renderDepthToSample(
      input.scene.renderDepthTexture, input.scene.renderDepthEntryState,
      RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
      RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH);
  // The display attachment has no prior consumer for this frame. Transition
  // depth and stencil separately because dynamic rendering binds them with
  // DEPTH_ATTACHMENT_OPTIMAL and STENCIL_ATTACHMENT_OPTIMAL respectively.
  RITextureBarrier displayDepthToAttachment(
      m_displayDepthTextures[imageIndex].Get(), RI_RESOURCE_STATE_UNDEFINED,
      RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_NONE, RI_STAGE_FRAGMENT,
      RI_BARRIER_ASPECT_DEPTH);
  RITextureBarrier displayStencilToAttachment(
      m_displayDepthTextures[imageIndex].Get(), RI_RESOURCE_STATE_UNDEFINED,
      RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_NONE, RI_STAGE_FRAGMENT,
      RI_BARRIER_ASPECT_STENCIL);
  RITextureBarrier beginBarriers[3] = {
      renderDepthToSample, displayDepthToAttachment,
      displayStencilToAttachment};
  input.cmd->vk_d3d12_textureBarriers<3>(3, beginBarriers);

  RIRenderingAttachment depth = {};
  depth.view = *m_displayDepthViews[imageIndex];
  depth.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depth.readOnly = false;
  depth.hasStencil = true;
  depth.stencilLoadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  depth.stencilStoreOp = RI_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depth = 1.0f;
  depth.clearValue.stencil = 0;

  RIBeginRenderingDesc begin = {};
  begin.renderArea.width = input.displayExtent.width;
  begin.renderArea.height = input.displayExtent.height;
  begin.depthStencil = &depth;
  input.cmd->vk_d3d12_beginRendering(&graphics->device, begin);

  // The shader's display UV is top-left / y-down. Vulkan's negative-height
  // viewport is the engine convention that makes that UV convention match
  // the attachment without changing the shader's fullscreen triangle.
  RIViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = static_cast<float>(input.displayExtent.height);
  viewport.width = static_cast<float>(input.displayExtent.width);
  viewport.height = -static_cast<float>(input.displayExtent.height);
  viewport.depthMin = 0.0f;
  viewport.depthMax = 1.0f;
  input.cmd->setViewport(&graphics->device, viewport);

  RIRect scissor = {};
  scissor.width = input.displayExtent.width;
  scissor.height = input.displayExtent.height;
  input.cmd->setScissor(&graphics->device, scissor);

  // Use the shared fullscreen RI pipeline state; this pass has no color target
  // because the fragment shader writes SV_Depth into D32S8.
  // Start from the shared fullscreen state and strip the colour half of it:
  // RI_FORMAT_UNKNOWN keeps the (unused) colour format inert, and colorCount /
  // blendCount are both zeroed -- the desc asserts they stay equal.
  RIGraphicsPipelineDesc pipelineDesc =
      MakePostEffectPipelineDesc(RI_FORMAT_UNKNOWN, false);
  pipelineDesc.renderTarget.colorCount = 0;
  pipelineDesc.renderTarget.colorFormats[0] = RI_FORMAT_UNKNOWN;
  pipelineDesc.blendCount = 0;
  pipelineDesc.renderTarget.depthFormat = cGraphics::DepthFormat;
  pipelineDesc.renderTarget.stencilFormat = cGraphics::DepthFormat;
  pipelineDesc.depthStencil.depthTest = true;
  pipelineDesc.depthStencil.depthWrite = true;
  pipelineDesc.depthStencil.depthCompare = RI_COMPARE_ALWAYS;
  // stencilTest stays false (the RI default), as it was explicitly in the
  // Vulkan create-info this replaced.
  const hash_t pipelineHash =
      hash_u32(hash_u32(HASH_INITIAL_VALUE, cGraphics::DepthFormat), 0u);
  mpResolveDepth->bindPipeline(&graphics->device, input.cmd, pipelineHash,
                               "Temporal.ResolveDepth.3d", pipelineDesc);

  RIProgram::DescriptorBinding bindings[2] = {};
  bindings[0].handle = DescriptorBindingID::Create("depthSampler");
  bindings[0].descriptor = *sampler;
  bindings[1].handle = DescriptorBindingID::Create("renderDepth");
  bindings[1].descriptor = RIDescriptor::sampledImage(
      &graphics->device, input.scene.renderDepthSampleView,
      RI_RESOURCE_STATE_SHADER_RESOURCE);
  mpResolveDepth->bindDescriptors(&graphics->device, input.cmd,
                                  input.frameIndex, bindings, 2);

  ResolveDepthPushConstants push = {};
  push.renderExtent[0] = input.renderExtent.width;
  push.renderExtent[1] = input.renderExtent.height;
  push.displayExtent[0] = input.displayExtent.width;
  push.displayExtent[1] = input.displayExtent.height;
  push.jitterPixels[0] = input.jitterPixels[0];
  push.jitterPixels[1] = input.jitterPixels[1];
  input.cmd->vk_d3d12_setPushConstants(&graphics->device, *mpResolveDepth, 0,
                                       sizeof(push), &push);
  input.cmd->draw(&graphics->device, 3, 1, 0, 0);
  input.cmd->vk_d3d12_endRendering(&graphics->device);

  // ResolveDepth has finished sampling. Restore the exact attachment state
  // requested by the viewport; OnPostTranslucenceDraw is the next consumer of
  // the render depth and binds it read-only as a depth attachment.
  RITextureBarrier renderDepthBack(
      input.scene.renderDepthTexture, RI_RESOURCE_STATE_SHADER_RESOURCE,
      input.scene.renderDepthExitState, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
      RI_BARRIER_ASPECT_DEPTH);
  input.cmd->vk_d3d12_textureBarriers<1>(1, &renderDepthBack);
  return true;
}

bool cTemporalPresentation::RecordSpatialFallback(
    const TemporalPresentationFrameInput &input) {
  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics || !input.cmd || !mpSpatialFallback ||
      !m_displayColorAllocated || !input.scene.hdrColor.IsValid() ||
      input.scene.hdrColor.format != cGraphics::PogoColorFormat ||
      !SameExtent(input.scene.hdrColor.extent, input.renderExtent) ||
      !IsContainedRect(input.scene.hdrValidRect, input.scene.hdrColor.extent) ||
      !IsNonZeroExtent(input.renderExtent) ||
      !IsNonZeroExtent(input.displayExtent) ||
      graphics->swapchainIndex >= m_resourceImageCount)
    return false;

  auto sampler = graphics->resolve_filter_descriptor(
      eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
      eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
  if (!sampler)
    return false;

  RIGpuScope _s(&graphics->profiler, input.cmd, "TemporalSpatialFallback");

  const uint32_t imageIndex = graphics->swapchainIndex;
  RITextureBarrier hdrColorToSample(
      input.scene.hdrColor.texture, input.scene.hdrColor.entryState,
      RI_RESOURCE_STATE_SHADER_RESOURCE, input.scene.hdrColor.entryStage,
      RI_STAGE_FRAGMENT,
      RI_BARRIER_ASPECT_COLOR);
  RITextureBarrier displayColorToAttachment(
      m_displayColorTextures[imageIndex].Get(), RI_RESOURCE_STATE_UNDEFINED,
      RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE, RI_STAGE_FRAGMENT,
      RI_BARRIER_ASPECT_COLOR);
  RITextureBarrier beginBarriers[2] = {hdrColorToSample,
                                       displayColorToAttachment};
  input.cmd->vk_d3d12_textureBarriers<2>(2, beginBarriers);

  RIRenderingAttachment color = {};
  color.view = *m_displayColorViews[imageIndex];
  color.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

  RIBeginRenderingDesc begin = {};
  begin.renderArea.width = input.displayExtent.width;
  begin.renderArea.height = input.displayExtent.height;
  begin.colorCount = 1;
  begin.colors = &color;
  input.cmd->vk_d3d12_beginRendering(&graphics->device, begin);

  // The shader's display UV is top-left / y-down. Keep the same negative
  // viewport convention as ResolveDepth so the current jitter correction has
  // identical orientation for color and depth.
  RIViewport viewport = {};
  viewport.y = static_cast<float>(input.displayExtent.height);
  viewport.width = static_cast<float>(input.displayExtent.width);
  viewport.height = -static_cast<float>(input.displayExtent.height);
  viewport.depthMin = 0.0f;
  viewport.depthMax = 1.0f;
  input.cmd->setViewport(&graphics->device, viewport);

  RIRect scissor = {};
  scissor.width = input.displayExtent.width;
  scissor.height = input.displayExtent.height;
  input.cmd->setScissor(&graphics->device, scissor);

  const RIGraphicsPipelineDesc pipelineDesc =
      MakePostEffectPipelineDesc(cGraphics::PogoColorFormat, false);
  const hash_t pipelineHash =
      hash_u32(HASH_INITIAL_VALUE, cGraphics::PogoColorFormat);
  mpSpatialFallback->bindPipeline(&graphics->device, input.cmd, pipelineHash,
                                  "Temporal.SpatialFallback.3d", pipelineDesc);

  RIProgram::DescriptorBinding bindings[2] = {};
  bindings[0].handle = DescriptorBindingID::Create("colorSampler");
  bindings[0].descriptor = *sampler;
  bindings[1].handle = DescriptorBindingID::Create("sceneHdrColor");
  bindings[1].descriptor = RIDescriptor::sampledImage(
      &graphics->device, input.scene.hdrColor.view,
      RI_RESOURCE_STATE_SHADER_RESOURCE);
  mpSpatialFallback->bindDescriptors(&graphics->device, input.cmd,
                                     input.frameIndex, bindings, 2);

  const TemporalPresentationRect &validRect = input.scene.hdrValidRect;
  SpatialFallbackPushConstants push = {};
  push.renderExtent[0] = input.renderExtent.width;
  push.renderExtent[1] = input.renderExtent.height;
  push.displayExtent[0] = input.displayExtent.width;
  push.displayExtent[1] = input.displayExtent.height;
  push.validRectMin[0] = validRect.x;
  push.validRectMin[1] = validRect.y;
  push.validRectMax[0] = validRect.x + validRect.width - 1;
  push.validRectMax[1] = validRect.y + validRect.height - 1;
  push.jitterPixels[0] = input.jitterPixels[0];
  push.jitterPixels[1] = input.jitterPixels[1];
  input.cmd->vk_d3d12_setPushConstants(&graphics->device, *mpSpatialFallback,
                                       0, sizeof(push), &push);
  input.cmd->draw(&graphics->device, 3, 1, 0, 0);
  input.cmd->vk_d3d12_endRendering(&graphics->device);

  RITextureBarrier hdrColorBack(
      input.scene.hdrColor.texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
      input.scene.hdrColor.exitState, RI_STAGE_FRAGMENT,
      input.scene.hdrColor.exitStage,
      RI_BARRIER_ASPECT_COLOR);
  RITextureBarrier displayColorBack(
      m_displayColorTextures[imageIndex].Get(), RI_RESOURCE_STATE_RENDER_TARGET,
      RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_NONE,
      RI_BARRIER_ASPECT_COLOR);
  RITextureBarrier endBarriers[2] = {hdrColorBack, displayColorBack};
  input.cmd->vk_d3d12_textureBarriers<2>(2, endBarriers);
  return true;
}

bool cTemporalPresentation::IsCurrentDisplayDepth(
    uint32_t frameIndex, TemporalUpscalerExtent displayExtent) const {
  return m_displayDepthValid && m_displayDepthFrameIndex == frameIndex &&
         SameExtent(m_displayDepthExtent, displayExtent);
}

TemporalPresentationResult cTemporalPresentation::Resolve(
    const TemporalPresentationFrameInput &input) {
  TemporalPresentationResult result = {};
  result.depthAliasesRender = true;
  InvalidateDisplayDepth();

  const bool nativeDepthPath =
      SameExtent(input.renderExtent, input.displayExtent) &&
      input.jitterPixels[0] == 0.0f && input.jitterPixels[1] == 0.0f;
  const bool providerUsed =
      input.provider != nullptr && input.settings.provider != TemporalUpscalerProvider::Off;
  const bool spatialFallbackEligible =
      providerUsed &&
      (!SameExtent(input.renderExtent, input.displayExtent) ||
       input.jitterPixels[0] != 0.0f || input.jitterPixels[1] != 0.0f);
  if (!providerUsed)
    m_providerFailureReported = false;

  auto failProvider = [&](bool ensureDisplayColor = true)
      -> TemporalPresentationResult {
    LatchProviderFailure();
    if (spatialFallbackEligible &&
        (!ensureDisplayColor ||
         EnsureDisplayColor(input.displayExtent, input.frameContext)) &&
        m_displayColorAllocated && EnsureSpatialFallbackProgram() &&
        RecordSpatialFallback(input)) {
      // The fallback rewrote the module-owned target, so it is safe to hand
      // out even though providerFailed remains true for the next-frame reset.
      RejectProviderResult(result);
      cGraphics *graphics = Interface<cGraphics>::Get();
      const uint32_t imageIndex = graphics->swapchainIndex;
      result.colorProduced = true;
      result.color.texture = m_displayColorTextures[imageIndex].Get();
      result.color.view = m_displayColorViews[imageIndex].Get();
      result.color.format = cGraphics::PogoColorFormat;
      result.color.extent = input.displayExtent;
      result.color.entryState = RI_RESOURCE_STATE_UNDEFINED;
      result.color.exitState = RI_RESOURCE_STATE_SHADER_RESOURCE;
      result.color.entryStage = RI_STAGE_NONE;
      result.color.exitStage = RI_STAGE_FRAGMENT;
      result.colorState = RI_RESOURCE_STATE_SHADER_RESOURCE;
      result.colorEntryStage = RI_STAGE_NONE;
      result.colorExitStage = RI_STAGE_FRAGMENT;
      result.colorIsSpatialFallback = true;
    } else {
      // In particular, never expose the previous contents of displayColor.
      // RejectProviderResult leaves any valid display depth from this call
      // intact for the caller.
      RejectProviderResult(result);
    }
    return result;
  };

  // A provider output is not needed on the native path when no provider is
  // active. Drop any resources left by a prior Off transition so the normal
  // path remains a true alias and does not retain a display target.
  if (nativeDepthPath && m_displayDepthAllocated) {
    // The native path aliases the render depth. Drop display-depth resources
    // left by an earlier scaled frame before recording the native render.
    Release(input.frameContext);
  } else if (!providerUsed && nativeDepthPath && HasOwnedResources())
    Release(input.frameContext);
  else if (!providerUsed && m_displayColorAllocated)
    Release(input.frameContext);

  if (!IsNonZeroExtent(input.renderExtent) ||
      !IsNonZeroExtent(input.displayExtent) || !input.cmd) {
    if (providerUsed)
      return failProvider();
    return result;
  }

  if (providerUsed) {
    // These are the bindings RecordResolve must receive. Fail before recording
    // anything if the provider could not possibly write a valid output.
    const bool validProviderInputs =
        input.scene.hdrColor.IsValid() &&
        IsContainedRect(input.scene.hdrValidRect, input.scene.hdrColor.extent) &&
        input.scene.renderDepthTexture &&
        input.scene.renderDepthSampleView && input.scene.motionVectors.IsValid();
    if (!validProviderInputs ||
        !input.provider->Supports(input.settings.provider,
                                  input.settings.quality) ||
        !input.frameContext ||
        !input.provider->PrepareContext(input.settings, input.renderExtent,
                                        input.displayExtent,
                                        input.frameContext)) {
      return failProvider();
    }
  }

  if (!nativeDepthPath) {
    const bool haveDepthInputs =
        input.scene.renderDepthTexture && input.scene.renderDepthSampleView &&
        input.scene.renderDepthAttachmentView;
    if (!haveDepthInputs ||
        !EnsureDisplayDepth(input.displayExtent, input.frameContext) ||
        !EnsureResolveDepthProgram() || !RecordDepthResolve(input)) {
      if (providerUsed)
        return failProvider();
      return result;
    }

    m_displayDepthExtent = input.displayExtent;
    m_displayDepthFrameIndex = input.frameIndex;
    m_displayDepthValid = true;
    result.displayDepthProduced = true;
    result.depthAliasesRender = false;
    const uint32_t imageIndex = Interface<cGraphics>::Get()->swapchainIndex;
    result.displayDepthTexture = m_displayDepthTextures[imageIndex].Get();
    result.displayDepthAttachmentView = m_displayDepthViews[imageIndex].Get();
  }

  if (!providerUsed)
    return result;

  if (!EnsureDisplayColor(input.displayExtent, input.frameContext))
    return failProvider(false);
  if (Interface<cGraphics>::Get()->swapchainIndex >= m_resourceImageCount)
    return failProvider(false);

  cGraphics *graphics = Interface<cGraphics>::Get();
  const uint32_t imageIndex = graphics->swapchainIndex;
  TemporalUpscalerTextureBinding output = {};
  output.texture = m_displayColorTextures[imageIndex].Get();
  output.view = m_displayColorViews[imageIndex].Get();
  output.format = cGraphics::PogoColorFormat;
  output.extent = input.displayExtent;
  output.entryState = RI_RESOURCE_STATE_UNDEFINED;
  output.exitState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  output.entryStage = RI_STAGE_NONE;
  output.exitStage = RI_STAGE_FRAGMENT;

  TemporalUpscalerFrameInput providerInput = {};
  providerInput.color = input.scene.hdrColor;
  providerInput.depth.texture = input.scene.renderDepthTexture;
  providerInput.depth.view = input.scene.renderDepthSampleView;
  providerInput.depth.format = cGraphics::DepthFormat;
  providerInput.depth.extent = input.renderExtent;
  // RecordResolve begins after RecordDepthResolve restored this attachment.
  // Its entry and exit state are therefore the scene's post-translucence
  // read-only depth state, not the temporary SHADER_RESOURCE state.
  providerInput.depth.entryState = input.scene.renderDepthExitState;
  providerInput.depth.exitState = input.scene.renderDepthExitState;
  providerInput.motionVectors = input.scene.motionVectors;
  providerInput.opaqueColor = input.scene.opaqueColor;
  providerInput.reactiveMaskJittered = input.scene.reactiveMaskJittered;
  providerInput.compositionMaskJittered = input.scene.compositionMaskJittered;
  providerInput.responsiveMaskUnjittered = input.scene.responsiveMaskUnjittered;
  providerInput.output = output;
  providerInput.cmd = input.cmd;
  providerInput.frameIndex = input.frameIndex;
  providerInput.deltaTimeMs = input.deltaTimeMs;
  providerInput.jitterPixels[0] = input.jitterPixels[0];
  providerInput.jitterPixels[1] = input.jitterPixels[1];
  providerInput.prevJitterPixels[0] = input.previousJitterPixels[0];
  providerInput.prevJitterPixels[1] = input.previousJitterPixels[1];
  std::memcpy(providerInput.viewMat, input.viewMat, sizeof(providerInput.viewMat));
  std::memcpy(providerInput.unjitteredProjMat, input.unjitteredProjMat,
              sizeof(providerInput.unjitteredProjMat));
  std::memcpy(providerInput.prevViewMat, input.previousViewMat,
              sizeof(providerInput.prevViewMat));
  std::memcpy(providerInput.prevUnjitteredProjMat,
              input.previousUnjitteredProjMat,
              sizeof(providerInput.prevUnjitteredProjMat));
  providerInput.zNear = input.zNear;
  providerInput.zFar = input.zFar;
  providerInput.verticalFovRadians = input.verticalFovRadians;
  providerInput.resetHistory = input.resetHistory;
  providerInput.preExposure = input.preExposure;

  TemporalUpscalerOutput providerOutput = {};
  {
    RIGpuScope _s(&graphics->profiler, input.cmd, "TemporalUpscaleResolve");
    providerOutput = input.provider->RecordResolve(
        input.renderExtent, input.displayExtent, providerInput);
  }
  // Only the module-owned output is safe to hand out. A provider that returns
  // success for another/unwritten image is a failure, never a reason to claim
  // temporal color was produced.
  if (!providerOutput.success ||
      providerOutput.result.texture != output.texture ||
      providerOutput.result.view != output.view ||
      !providerOutput.result.IsValid() ||
      !SameExtent(providerOutput.result.extent, input.displayExtent)) {
    return failProvider(false);
  }

  result.colorProduced = true;
  result.color = providerOutput.result;
  result.colorState = providerOutput.resultState;
  result.colorEntryStage = providerOutput.result.entryStage;
  result.colorExitStage = providerOutput.resultStage;
  result.colorIsSpatialFallback = false;
  m_providerFailureReported = false;
  return result;
}

void cTemporalPresentation::Release(cGraphics::FrameContext *frameContext) {
  (void)frameContext;
  cGraphics *graphics = Interface<cGraphics>::Get();

  // Views are parked before their backing textures. Moving the shared handles
  // into FrameDeferral prevents an in-flight command buffer from seeing an
  // inline RISharedPointer::dispose during resize or viewport destruction.
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    graphics->graphicsDefer.push(std::move(m_displayDepthViews[i]));
    graphics->graphicsDefer.push(std::move(m_displayDepthSampleViews[i]));
    graphics->graphicsDefer.push(std::move(m_displayDepthTextures[i]));
    graphics->graphicsDefer.push(std::move(m_displayColorViews[i]));
    graphics->graphicsDefer.push(std::move(m_displayColorTextures[i]));
  }

  // RIProgram owns descriptor pools, layouts and cached Vulkan pipelines.
  // Its dispose() is just as GPU-sensitive as RITexture::dispose(), so keep
  // the shared object alive until the graphics deferral reaches this lambda.
  if (mpResolveDepth) {
    std::shared_ptr<RIProgram> keepProgram = std::move(mpResolveDepth);
    RIDevice *device = &graphics->device;
    graphics->graphicsDefer.push(std::function<void()>(
        [keepProgram = std::move(keepProgram), device]() mutable {
          keepProgram->dispose(device);
          keepProgram.reset();
        }));
  }
  if (mpSpatialFallback) {
    std::shared_ptr<RIProgram> keepProgram = std::move(mpSpatialFallback);
    RIDevice *device = &graphics->device;
    graphics->graphicsDefer.push(std::function<void()>(
        [keepProgram = std::move(keepProgram), device]() mutable {
          keepProgram->dispose(device);
          keepProgram.reset();
        }));
  }

  m_resourceExtent = {};
  m_resourceExtentValid = false;
  m_resourceImageCount = 0;
  m_displayDepthAllocated = false;
  m_displayColorAllocated = false;
  InvalidateDisplayDepth();
}

bool cTemporalPresentation::ConsumeProviderFailure() {
  const bool failed = m_providerFailureLatched;
  m_providerFailureLatched = false;
  return failed;
}

RITexture *cTemporalPresentation::GetDisplayDepthTexture(
    uint32_t frameIndex, TemporalUpscalerExtent displayExtent) const {
  if (!IsCurrentDisplayDepth(frameIndex, displayExtent))
    return nullptr;
  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics || graphics->swapchainIndex >= m_resourceImageCount)
    return nullptr;
  return m_displayDepthTextures[graphics->swapchainIndex].Get();
}

RITextureView *cTemporalPresentation::GetDisplayDepthAttachmentView(
    uint32_t frameIndex, TemporalUpscalerExtent displayExtent) const {
  if (!IsCurrentDisplayDepth(frameIndex, displayExtent))
    return nullptr;
  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics || graphics->swapchainIndex >= m_resourceImageCount)
    return nullptr;
  return m_displayDepthViews[graphics->swapchainIndex].Get();
}

RITextureView *cTemporalPresentation::GetDisplayDepthSampleView(
    uint32_t frameIndex, TemporalUpscalerExtent displayExtent) const {
  if (!IsCurrentDisplayDepth(frameIndex, displayExtent))
    return nullptr;
  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics || graphics->swapchainIndex >= m_resourceImageCount)
    return nullptr;
  return m_displayDepthSampleViews[graphics->swapchainIndex].Get();
}

} // namespace hpl
