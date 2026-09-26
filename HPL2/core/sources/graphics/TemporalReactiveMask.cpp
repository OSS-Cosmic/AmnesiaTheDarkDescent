#include "graphics/TemporalReactiveMask.h"

#include "graphics/Graphics.h"
#include "graphics/RIBarrier.h"
#include "graphics/RICommand.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIProgram.h"
#include "graphics/RIGpuProfiler.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "scene/Viewport.h"
#include "system/Hasher.h"

#include <algorithm>
#include <array>
#include <functional>
#include <utility>

namespace hpl {
namespace {

struct ReactiveMaskPushConstants {
  uint32_t extent[2];
  float jitterPixels[2];
  uint32_t writeResponsive;
  float luminanceFloor;
  float reactiveScale;
  float compositionScale;
  float compositionKnee;
  float changeEpsilon;
};

static_assert(sizeof(ReactiveMaskPushConstants) == 40,
              "ReactiveMaskPC must match the Slang push-constant layout");

static bool SameExtent(TemporalUpscalerExtent a, TemporalUpscalerExtent b) {
  return a.width == b.width && a.height == b.height;
}

static bool IsNonZeroExtent(TemporalUpscalerExtent extent) {
  return extent.width != 0 && extent.height != 0;
}

static uint32_t SwapchainImageCount(cGraphics *graphics) {
  if (!graphics || !graphics->swapchain)
    return 0;
  return std::min<uint32_t>(graphics->swapchain->imageCount,
                            RI_MAX_SWAPCHAIN_IMAGES);
}

} // namespace

void cTemporalReactiveMask::ResetImageState(ScratchImage &image) {
  image.state = RI_RESOURCE_STATE_UNDEFINED;
  image.stage = RI_STAGE_NONE;
}

void cTemporalReactiveMask::Transition(RICmd *cmd, ScratchImage &image,
                                       RIResourceState_e after,
                                       uint32_t afterStage) {
  RITextureBarrier barrier(image.texture.Get(), image.state, after,
                           image.stage, afterStage);
  cmd->vk_d3d12_textureBarrier(barrier);
  image.state = after;
  image.stage = afterStage;
}

TemporalUpscalerTextureBinding cTemporalReactiveMask::MakeBinding(
    const ScratchImage &image, RI_Format_e format,
    TemporalUpscalerExtent extent) {
  TemporalUpscalerTextureBinding binding = {};
  binding.texture = image.texture.Get();
  binding.view = image.view.Get();
  binding.format = format;
  binding.extent = extent;
  binding.mipOffset = 0;
  binding.mipCount = 1;
  binding.layerOffset = 0;
  binding.layerCount = 1;
  binding.entryState = image.state;
  binding.exitState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  return binding;
}

cTemporalReactiveMask::cTemporalReactiveMask(cFileSearcher *shaderFiles)
    : mpShaderFiles(shaderFiles) {}

cTemporalReactiveMask::~cTemporalReactiveMask() { Release(nullptr); }

void cTemporalReactiveMask::InvalidateFrame() {
  m_frameValid = false;
  m_frameIndex = 0;
  mpViewportCookie = nullptr;
  m_frameExtent = {};
  m_snapshotValid = false;
  m_masksValid = false;
}

bool cTemporalReactiveMask::EnsureReactiveMaskProgram() {
  if (mpReactiveMask)
    return true;
  if (!mpShaderFiles)
    return false;

  auto reactiveMaskBin =
      RIProgram::loadShaderStage(mpShaderFiles, "ReactiveMask.cs");
  if (reactiveMaskBin.empty())
    return false;

  std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
      RIProgram::PROGRAM_STAGE_COMPUTE, reactiveMaskBin, "csMain"}};
  auto program = std::make_shared<RIProgram>();
  program->initialize(&Interface<cGraphics>::Get()->device, stages, {},
                      "Temporal.ReactiveMask.cs");
  mpReactiveMask = std::move(program);
  return true;
}

bool cTemporalReactiveMask::AllocateResources(
    cGraphics *graphics, TemporalUpscalerExtent extent, uint32_t imageCount,
    RI_Format_e maskFormat) {
  for (uint32_t i = 0; i < imageCount; ++i) {
    if (!CreateViewportAttachmentTexture(
            &graphics->device, extent.width, extent.height,
            cGraphics::PogoColorFormat,
            RI_USAGE_TRANSFER_DST | RI_USAGE_SHADER_RESOURCE,
            RI_VIEWTYPE_SHADER_RESOURCE_2D, &m_opaqueColors[i].texture,
            &m_opaqueColors[i].view, "TemporalReactiveMask.opaqueColor"))
      return false;

    if (!CreateViewportAttachmentTexture(
            &graphics->device, extent.width, extent.height, maskFormat,
            RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
            RI_VIEWTYPE_SHADER_RESOURCE_2D, &m_reactiveMasks[i].texture,
            &m_reactiveMasks[i].view, "TemporalReactiveMask.reactiveMask"))
      return false;
    if (!CreateViewportAttachmentTexture(
            &graphics->device, extent.width, extent.height, maskFormat,
            RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
            RI_VIEWTYPE_SHADER_RESOURCE_2D, &m_compositionMasks[i].texture,
            &m_compositionMasks[i].view,
            "TemporalReactiveMask.compositionMask"))
      return false;

    if (m_needUnjitteredVariant &&
        !CreateViewportAttachmentTexture(
            &graphics->device, extent.width, extent.height, maskFormat,
            RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
            RI_VIEWTYPE_SHADER_RESOURCE_2D, &m_responsiveMasks[i].texture,
            &m_responsiveMasks[i].view,
            "TemporalReactiveMask.responsiveMask"))
      return false;
  }
  return true;
}

bool cTemporalReactiveMask::BeginFrame(
    const TemporalReactiveMaskFrameDesc &desc) {
  if (!desc.enabled || !IsNonZeroExtent(desc.renderExtent) ||
      !desc.frameContext) {
    Release(desc.frameContext);
    InvalidateFrame();
    return false;
  }

  cGraphics *graphics = Interface<cGraphics>::Get();
  const uint32_t imageCount = SwapchainImageCount(graphics);
  if (!graphics || imageCount == 0) {
    Release(desc.frameContext);
    InvalidateFrame();
    return false;
  }

  if (m_resourcesAllocated &&
      (!SameExtent(m_resourceExtent, desc.renderExtent) ||
       m_resourceImageCount != imageCount ||
       m_needUnjitteredVariant != desc.needUnjitteredVariant))
    Release(desc.frameContext);

  if (!m_resourcesAllocated) {
    m_resourceExtent = desc.renderExtent;
    m_resourceImageCount = imageCount;
    m_needUnjitteredVariant = desc.needUnjitteredVariant;

    // R8_UNORM is the compact mask format. Vulkan implementations may reject
    // it as a storage image, while R16_SFLOAT is mandated for storage images;
    // retry the complete allocation with one fallback format so every mask
    // binding agrees.
    if (!AllocateResources(graphics, desc.renderExtent, imageCount,
                           RI_FORMAT_R8_UNORM)) {
      Release(desc.frameContext);
      m_resourceExtent = desc.renderExtent;
      m_resourceImageCount = imageCount;
      m_needUnjitteredVariant = desc.needUnjitteredVariant;
      if (!AllocateResources(graphics, desc.renderExtent, imageCount,
                             RI_FORMAT_R16_SFLOAT)) {
        Release(desc.frameContext);
        InvalidateFrame();
        return false;
      }
      m_maskFormat = RI_FORMAT_R16_SFLOAT;
    } else {
      m_maskFormat = RI_FORMAT_R8_UNORM;
    }
    m_resourcesAllocated = true;
  }

  m_frameIndex = desc.frameIndex;
  mpViewportCookie = desc.viewportCookie;
  m_frameExtent = desc.renderExtent;
  m_frameValid = true;
  m_snapshotValid = false;
  m_masksValid = false;
  return true;
}

bool cTemporalReactiveMask::IsCurrentFrame(
    uint32_t frameIndex, TemporalUpscalerExtent extent,
    const void *viewportCookie) const {
  return m_frameValid && m_frameIndex == frameIndex &&
         SameExtent(m_frameExtent, extent) &&
         mpViewportCookie == viewportCookie;
}

bool cTemporalReactiveMask::IsCurrentSnapshot(
    uint32_t frameIndex, TemporalUpscalerExtent extent,
    const void *viewportCookie) const {
  return m_snapshotValid && IsCurrentFrame(frameIndex, extent, viewportCookie);
}

bool cTemporalReactiveMask::RecordOpaqueSnapshot(
    const TemporalReactiveMaskSnapshotDesc &desc) {
  if (!IsCurrentFrame(desc.frameIndex, desc.extent, desc.viewportCookie) ||
      !desc.cmd || !desc.sceneColor ||
      desc.sceneColorFormat != cGraphics::PogoColorFormat)
    return false;

  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics || graphics->swapchainIndex >= m_resourceImageCount)
    return false;

  RIGpuScope scope(&graphics->profiler, desc.cmd,
                   "TemporalReactiveMask.Snapshot");
  ScratchImage &opaque = m_opaqueColors[graphics->swapchainIndex];
  Transition(desc.cmd, opaque, RI_RESOURCE_STATE_COPY_DST, RI_STAGE_COPY);

  RITextureBarrier sceneToCopy(
      desc.sceneColor, desc.sceneColorEntryState, RI_RESOURCE_STATE_COPY_SRC,
      desc.sceneColorEntryStage, RI_STAGE_COPY);
  desc.cmd->vk_d3d12_textureBarrier(sceneToCopy);

  RIImageCopyDesc copy = {};
  copy.srcMipLevel = 0;
  copy.srcArrayLayer = 0;
  copy.srcX = 0;
  copy.srcY = 0;
  copy.srcZ = 0;
  copy.dstMipLevel = 0;
  copy.dstArrayLayer = 0;
  copy.dstX = 0;
  copy.dstY = 0;
  copy.dstZ = 0;
  copy.width = desc.extent.width;
  copy.height = desc.extent.height;
  copy.depth = 1;
  desc.cmd->copyImage(&graphics->device, desc.sceneColor,
                      opaque.texture.Get(), copy);

  RITextureBarrier sceneBack(
      desc.sceneColor, RI_RESOURCE_STATE_COPY_SRC,
      desc.sceneColorExitState, RI_STAGE_COPY, desc.sceneColorExitStage);
  desc.cmd->vk_d3d12_textureBarrier(sceneBack);

  m_snapshotValid = true;
  m_masksValid = false;
  return true;
}

bool cTemporalReactiveMask::RecordMasks(
    const TemporalReactiveMaskRecordDesc &desc) {
  if (!IsCurrentSnapshot(desc.frameIndex, desc.extent, desc.viewportCookie) ||
      !desc.cmd || !desc.finalColor || !desc.finalColorView ||
      desc.finalColorFormat == RI_FORMAT_UNKNOWN)
    return false;

  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics || graphics->swapchainIndex >= m_resourceImageCount ||
      !m_resourcesAllocated || !EnsureReactiveMaskProgram())
    return false;

  RIGpuScope scope(&graphics->profiler, desc.cmd,
                   "TemporalReactiveMask.Masks");
  const uint32_t imageIndex = graphics->swapchainIndex;
  ScratchImage &opaque = m_opaqueColors[imageIndex];
  ScratchImage &reactive = m_reactiveMasks[imageIndex];
  ScratchImage &composition = m_compositionMasks[imageIndex];
  Transition(desc.cmd, opaque, RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_STAGE_COMPUTE);
  Transition(desc.cmd, reactive, RI_RESOURCE_STATE_STORAGE_WRITE,
             RI_STAGE_COMPUTE);
  Transition(desc.cmd, composition, RI_RESOURCE_STATE_STORAGE_WRITE,
             RI_STAGE_COMPUTE);
  if (m_needUnjitteredVariant) {
    Transition(desc.cmd, m_responsiveMasks[imageIndex],
               RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE);
  }

  RITextureBarrier finalToCompute(
      desc.finalColor, desc.finalColorEntryState,
      RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_COMPUTE);
  desc.cmd->vk_d3d12_textureBarrier(finalToCompute);

  const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, 0x524d4153u);
  mpReactiveMask->bindComputePipeline(&graphics->device, desc.cmd, pipelineHash,
                                      "Temporal.ReactiveMask.cs");

  RITextureView *responsiveView =
      m_needUnjitteredVariant ? m_responsiveMasks[imageIndex].view.Get()
                              : reactive.view.Get();
  std::array<RIProgram::DescriptorBinding, 5> bindings = {
      RIProgram::DescriptorBinding(
          "gOpaqueColor",
          RIDescriptor::sampledImage(&graphics->device, opaque.view.Get(),
                                     RI_RESOURCE_STATE_SHADER_RESOURCE)),
      RIProgram::DescriptorBinding(
          "gSceneColor",
          RIDescriptor::sampledImage(&graphics->device, desc.finalColorView,
                                     RI_RESOURCE_STATE_SHADER_RESOURCE)),
      RIProgram::DescriptorBinding(
          "gReactiveMask",
          RIDescriptor::storageImage(&graphics->device, reactive.view.Get())),
      RIProgram::DescriptorBinding(
          "gCompositionMask",
          RIDescriptor::storageImage(&graphics->device,
                                     composition.view.Get())),
      RIProgram::DescriptorBinding(
          "gResponsiveMask",
          RIDescriptor::storageImage(&graphics->device, responsiveView))
  };
  mpReactiveMask->bindDescriptors(
      &graphics->device, desc.cmd, desc.frameIndex, bindings.data(),
      bindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

  const TemporalReactiveMaskParams &params = desc.params;
  ReactiveMaskPushConstants push = {};
  push.extent[0] = desc.extent.width;
  push.extent[1] = desc.extent.height;
  push.jitterPixels[0] = desc.jitterPixels[0];
  push.jitterPixels[1] = desc.jitterPixels[1];
  push.writeResponsive = m_needUnjitteredVariant ? 1u : 0u;
  push.luminanceFloor = params.luminanceFloor;
  push.reactiveScale = params.reactiveScale;
  push.compositionScale = params.compositionScale;
  push.compositionKnee = params.compositionKnee;
  push.changeEpsilon = params.changeEpsilon;
  desc.cmd->vk_d3d12_setPushConstants(&graphics->device, *mpReactiveMask, 0,
                                       sizeof(push), &push);
  desc.cmd->dispatch(&graphics->device, (desc.extent.width + 15u) / 16u,
                     (desc.extent.height + 15u) / 16u, 1u);

  Transition(desc.cmd, reactive, RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_STAGE_COMPUTE);
  Transition(desc.cmd, composition, RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_STAGE_COMPUTE);
  if (m_needUnjitteredVariant) {
    Transition(desc.cmd, m_responsiveMasks[imageIndex],
               RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE);
  }

  RITextureBarrier finalBack(
      desc.finalColor, RI_RESOURCE_STATE_SHADER_RESOURCE,
      desc.finalColorExitState, RI_STAGE_COMPUTE, RI_STAGE_FRAGMENT);
  desc.cmd->vk_d3d12_textureBarrier(finalBack);

  m_masksValid = true;
  return true;
}

TemporalReactiveMaskBindings cTemporalReactiveMask::GetBindings(
    uint32_t frameIndex, TemporalUpscalerExtent extent,
    const void *viewportCookie) const {
  TemporalReactiveMaskBindings result = {};
  if (!m_masksValid || !IsCurrentFrame(frameIndex, extent, viewportCookie))
    return result;

  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics || graphics->swapchainIndex >= m_resourceImageCount)
    return result;

  const uint32_t imageIndex = graphics->swapchainIndex;
  const ScratchImage &opaque = m_opaqueColors[imageIndex];
  const ScratchImage &reactive = m_reactiveMasks[imageIndex];
  const ScratchImage &composition = m_compositionMasks[imageIndex];
  if (opaque.state != RI_RESOURCE_STATE_SHADER_RESOURCE ||
      reactive.state != RI_RESOURCE_STATE_SHADER_RESOURCE ||
      composition.state != RI_RESOURCE_STATE_SHADER_RESOURCE ||
      (m_needUnjitteredVariant &&
       m_responsiveMasks[imageIndex].state != RI_RESOURCE_STATE_SHADER_RESOURCE))
    return result;

  result.opaqueColor = MakeBinding(opaque, cGraphics::PogoColorFormat,
                                   m_resourceExtent);
  result.reactiveMaskJittered =
      MakeBinding(reactive, m_maskFormat, m_resourceExtent);
  result.compositionMaskJittered =
      MakeBinding(composition, m_maskFormat, m_resourceExtent);
  if (m_needUnjitteredVariant)
    result.responsiveMaskUnjittered = MakeBinding(
        m_responsiveMasks[imageIndex], m_maskFormat, m_resourceExtent);
  result.valid = result.opaqueColor.IsValid() &&
                 result.reactiveMaskJittered.IsValid() &&
                 result.compositionMaskJittered.IsValid() &&
                 (!m_needUnjitteredVariant ||
                  result.responsiveMaskUnjittered.IsValid());
  if (!result.valid)
    return {};
  return result;
}

void cTemporalReactiveMask::Release(cGraphics::FrameContext *frameContext) {
  (void)frameContext;
  cGraphics *graphics = Interface<cGraphics>::Get();

  // Park every view before every backing texture. This ordering keeps an
  // in-flight descriptor's view alive until the image it names is released.
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    graphics->graphicsDefer.push(std::move(m_opaqueColors[i].view));
    graphics->graphicsDefer.push(std::move(m_reactiveMasks[i].view));
    graphics->graphicsDefer.push(std::move(m_compositionMasks[i].view));
    graphics->graphicsDefer.push(std::move(m_responsiveMasks[i].view));
  }
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    graphics->graphicsDefer.push(std::move(m_opaqueColors[i].texture));
    graphics->graphicsDefer.push(std::move(m_reactiveMasks[i].texture));
    graphics->graphicsDefer.push(std::move(m_compositionMasks[i].texture));
    graphics->graphicsDefer.push(std::move(m_responsiveMasks[i].texture));
    ResetImageState(m_opaqueColors[i]);
    ResetImageState(m_reactiveMasks[i]);
    ResetImageState(m_compositionMasks[i]);
    ResetImageState(m_responsiveMasks[i]);
  }

  if (mpReactiveMask) {
    std::shared_ptr<RIProgram> keepProgram = std::move(mpReactiveMask);
    RIDevice *device = &graphics->device;
    graphics->graphicsDefer.push(std::function<void()>(
        [keepProgram = std::move(keepProgram), device]() mutable {
          keepProgram->dispose(device);
          keepProgram.reset();
        }));
  }

  m_resourceExtent = {};
  m_resourcesAllocated = false;
  m_resourceImageCount = 0;
  m_needUnjitteredVariant = false;
  m_maskFormat = RI_FORMAT_UNKNOWN;
  InvalidateFrame();
}

} // namespace hpl
