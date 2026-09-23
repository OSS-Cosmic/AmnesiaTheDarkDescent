#include "graphics/WaterReflectionPass.h"

#include "graphics/Graphics.h"
#include "graphics/GlobalManagedSets.h"
#include "graphics/PathTracePayload.h"
#include "graphics/RIBarrier.h"
#include "graphics/RICommand.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "graphics/RIGpuProfiler.h"
#include "graphics/WaterGuidePipelineDesc.h"
#include "graphics/WaterReflectionJitterMath.h"
#include "resources/Resources.h"
#include "scene/Viewport.h"

#include "system/Hasher.h"

#include <array>
#include <cassert>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hpl {
namespace {

struct ScratchImage {
  RISharedPointer<RITexture> texture;
  RISharedPointer<RITextureView> view;
  // Only populated for images created with RI_USAGE_COLOR_ATTACHMENT (the three
  // guide targets). D3D12 rejects `view` as a render target; see
  // CreateViewportColorAttachmentView.
  RISharedPointer<RITextureView> attachmentView;
  uint32_t state = RI_RESOURCE_STATE_UNDEFINED;
  uint32_t stage = RI_STAGE_NONE;
};

static void resetImageState(ScratchImage &image) {
  image.state = RI_RESOURCE_STATE_UNDEFINED;
  image.stage = RI_STAGE_NONE;
}

static void deferNrd(cGraphics *graphics,
                    std::shared_ptr<NrdIntegration> nrd) {
  if (!nrd)
    return;

  // NrdIntegration destroys its pipelines eagerly. Keep the whole instance
  // alive through the graphics timeline; deferring only its textures would
  // leave in-flight command buffers pointing at freed NRD pipelines.
  graphics->graphicsDefer.push(std::function<void()>(
      [keep = std::move(nrd)]() mutable { keep.reset(); }));
}

static void deferImage(cGraphics *graphics, ScratchImage &image) {
  // A view must outlive the image it names, so queue the views first.
  graphics->graphicsDefer.push(std::move(image.view));
  if (!image.attachmentView.isEmpty())
    graphics->graphicsDefer.push(std::move(image.attachmentView));
  graphics->graphicsDefer.push(std::move(image.texture));
  resetImageState(image);
}

static void transition(RICmd *cmd, ScratchImage &image, uint32_t after,
                       uint32_t afterStage) {
  RITextureBarrier barrier(image.texture.Get(), image.state, after,
                           image.stage, afterStage);
  cmd->vk_d3d12_textureBarrier(barrier);
  image.state = after;
  image.stage = afterStage;
}

static void appendSharedBindings(
    std::vector<RIProgram::DescriptorBinding> &bindings,
    const WaterReflectionSurfaceDesc &surface) {
  if (!surface.sharedBindings || surface.sharedBindingCount == 0)
    return;
  bindings.insert(bindings.end(), surface.sharedBindings,
                  surface.sharedBindings + surface.sharedBindingCount);
}

static void appendBinding(std::vector<RIProgram::DescriptorBinding> &bindings,
                          const char *name, RIDescriptor descriptor) {
  bindings.emplace_back(name, descriptor);
}

static void bindGuideResources(
    cGraphics *graphics, RICmd *cmd, RIProgram &program,
    const WaterReflectionSurfaceDesc &surface, uint32_t frameIndex) {
  std::vector<RIProgram::DescriptorBinding> bindings;
  bindings.reserve(surface.sharedBindingCount);
  appendSharedBindings(bindings, surface);
  program.bindDescriptors(&graphics->device, cmd, frameIndex, bindings.data(),
                          bindings.size());
}

static void bindComputeResources(
    cGraphics *graphics, RICmd *cmd, RIProgram &program,
    uint32_t frameIndex, std::vector<RIProgram::DescriptorBinding> &bindings) {
  program.bindDescriptors(&graphics->device, cmd, frameIndex, bindings.data(),
                          bindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE);
}

static void bindTraceResources(
    cGraphics *graphics, RICmd *cmd, RIProgram &program,
    uint32_t frameIndex, std::vector<RIProgram::DescriptorBinding> &bindings) {
  program.bindDescriptors(&graphics->device, cmd, frameIndex, bindings.data(),
                          bindings.size(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
}

} // namespace

static bool createScratchImage(cGraphics *graphics, uint32_t width,
                               uint32_t height, RI_Format_e format,
                               uint32_t usage, const char *name,
                               ScratchImage &image);

struct WaterReflectionViewportState::Impl {
  struct History {
    std::shared_ptr<NrdIntegration> nrd;
    uint32_t nrdWidth = 0;
    uint32_t nrdHeight = 0;
    uint64_t materialSignature = 0;
    uint32_t lastFrameIndex = 0;
    bool hasSuccessfulDenoise = false;
    bool usedThisFrame = false;
  };

  cGraphics *graphics = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t halfWidth = 0;
  uint32_t halfHeight = 0;
  uint32_t frameIndex = 0;
  bool resetHistory = false;
  bool frameBarrierIssued = false;
  NrdFrameData nrd = {};

  // Full-resolution raster guides.
  ScratchImage guidePositionViewZ;
  ScratchImage guideNormalWeight;
  ScratchImage guideVelocity;

  // Half-resolution reducer output and raw RT signal.
  ScratchImage halfPositionViewZ;
  ScratchImage halfNormalWeight;
  ScratchImage halfNormalSpread;
  ScratchImage halfVelocity;
  ScratchImage reflectionRadianceHitDist;

  // Pack output. IN_MV is writable during NRD stabilization, hence its state
  // deliberately retains both SHADER_RESOURCE and STORAGE_WRITE.
  ScratchImage nrdNormalRoughness;
  ScratchImage nrdViewZ;
  ScratchImage nrdSpecularRadianceHitDist;
  ScratchImage nrdMotionVectors;

  std::unordered_map<uint64_t, History> histories;

  bool ensureScratch();

  bool hasScratch() const {
    return !guidePositionViewZ.texture.isEmpty() &&
           !guideNormalWeight.texture.isEmpty() &&
           !guideVelocity.texture.isEmpty() &&
           !halfPositionViewZ.texture.isEmpty() &&
           !halfNormalWeight.texture.isEmpty() &&
           !halfNormalSpread.texture.isEmpty() &&
           !halfVelocity.texture.isEmpty() &&
           !reflectionRadianceHitDist.texture.isEmpty() &&
           !nrdNormalRoughness.texture.isEmpty() &&
           !nrdViewZ.texture.isEmpty() &&
           !nrdSpecularRadianceHitDist.texture.isEmpty() &&
           !nrdMotionVectors.texture.isEmpty();
  }

  void releaseHistories() {
    for (auto &entry : histories)
      deferNrd(graphics, std::move(entry.second.nrd));
    histories.clear();
  }

  void releaseScratch() {
    deferImage(graphics, guidePositionViewZ);
    deferImage(graphics, guideNormalWeight);
    deferImage(graphics, guideVelocity);
    deferImage(graphics, halfPositionViewZ);
    deferImage(graphics, halfNormalWeight);
    deferImage(graphics, halfNormalSpread);
    deferImage(graphics, halfVelocity);
    deferImage(graphics, reflectionRadianceHitDist);
    deferImage(graphics, nrdNormalRoughness);
    deferImage(graphics, nrdViewZ);
    deferImage(graphics, nrdSpecularRadianceHitDist);
    deferImage(graphics, nrdMotionVectors);
  }

  void releaseAll() {
    releaseScratch();
    releaseHistories();
  }

  void invalidate(uint64_t cookie) {
    auto it = histories.find(cookie);
    if (it == histories.end())
      return;
    deferNrd(graphics, std::move(it->second.nrd));
    histories.erase(it);
  }
};

WaterReflectionViewportState::WaterReflectionViewportState()
    : m_impl(std::make_unique<Impl>()) {}

WaterReflectionViewportState::~WaterReflectionViewportState() {
  if (m_impl->graphics)
    m_impl->releaseAll();
}

void WaterReflectionPass::Initialize(cGraphics *graphics, cResources *resources) {
  assert(graphics != nullptr);
  assert(resources != nullptr);
  m_graphics = graphics;

  const RIBindlessLayout externalLayouts[] = {
      graphics->globalset->m_bindlessSet.layout()};

  auto guideVert = RIProgram::loadShaderStage(
      resources->GetFileSearcher(), "WaterGuide.vert");
  auto guideFrag = RIProgram::loadShaderStage(
      resources->GetFileSearcher(), "WaterGuide.frag");
  std::array<RIProgram::ModuleStage, 2> guideStages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, guideVert,
                             "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, guideFrag,
                             "psMain"}};
  m_guide.initialize(&graphics->device, guideStages, externalLayouts,
                     "Water.guide");

  auto reduceBin = RIProgram::loadShaderStage(
      resources->GetFileSearcher(), "WaterGuideReduce.cs");
  RIProgram::ModuleStage reduceStage = {RIProgram::PROGRAM_STAGE_COMPUTE,
                                        reduceBin, "csMain"};
  m_reduce.initialize(&graphics->device,
                      std::span<RIProgram::ModuleStage>(&reduceStage, 1),
                      externalLayouts, "Water.reduce");

  auto traceBin = RIProgram::loadShaderStage(
      resources->GetFileSearcher(), "WaterReflection.rt");
  std::array<RIProgram::ModuleStage, 4> traceStages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_RAYGEN, traceBin,
                             "waterReflRayGen"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_MISS, traceBin,
                             "waterReflMiss"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_CLOSEST_HIT, traceBin,
                             "waterReflCloseHit"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_ANY_HIT, traceBin,
                             "waterReflAnyHit"}};
  m_trace.initialize(&graphics->device, traceStages, externalLayouts,
                     "Water.trace");

  auto packBin = RIProgram::loadShaderStage(
      resources->GetFileSearcher(), "WaterReflectionPack.cs");
  RIProgram::ModuleStage packStage = {RIProgram::PROGRAM_STAGE_COMPUTE,
                                      packBin, "csMain"};
  m_pack.initialize(&graphics->device,
                    std::span<RIProgram::ModuleStage>(&packStage, 1),
                    externalLayouts, "Water.pack");
}

void WaterReflectionPass::Dispose(cGraphics *graphics) {
  assert(graphics != nullptr);
  m_guide.dispose(&graphics->device);
  m_reduce.dispose(&graphics->device);
  m_trace.dispose(&graphics->device);
  m_pack.dispose(&graphics->device);
}

void WaterReflectionPass::BeginFrame(WaterReflectionViewportState &state,
                                     const WaterReflectionFrameDesc &desc) {
  assert(m_graphics != nullptr);
  WaterReflectionViewportState::Impl &impl = *state.m_impl;
  impl.graphics = m_graphics;
  impl.frameBarrierIssued = false;

  const bool extentChanged = impl.width != desc.width ||
                             impl.height != desc.height;
  if (extentChanged || desc.resetHistory || desc.width == 0 ||
      desc.height == 0) {
    // Resetting history releases the reusable images as well. This ensures
    // the next real surface starts from fresh image layouts and fresh NRD
    // instances, while an empty viewport performs no allocation or dispatch.
    impl.releaseAll();
  } else {
    for (auto &entry : impl.histories)
      entry.second.usedThisFrame = false;
  }

  impl.width = desc.width;
  impl.height = desc.height;
  impl.halfWidth = WaterReflectionHalfResExtent(desc.width);
  impl.halfHeight = WaterReflectionHalfResExtent(desc.height);
  impl.frameIndex = desc.frameIndex;
  impl.resetHistory = desc.resetHistory;
  impl.nrd = desc.nrd;

  // Convert full-extent jitter into the half-resolution NRD instance's input
  // pixels using the same ceil-based extents allocated above. Zero extent is
  // valid during teardown, so the conversion produces zero jitter instead of
  // dividing by zero. Both ratios are at most one and the input is in
  // [-0.5, 0.5], so the result remains inside NRD's asserted range without a
  // clamp.
  WaterReflectionScaleJitterToHalfRes(
      desc.nrd.cameraJitter, desc.width, desc.height, impl.nrd.cameraJitter);
  // Scale cameraJitterPrev by the current frame's ratio. An extent change
  // calls impl.releaseAll() through extentChanged, dropping every NRD
  // instance and its history, so a previous ratio cannot be needed for
  // reprojection on the only frame where it could differ.
  WaterReflectionScaleJitterToHalfRes(desc.nrd.cameraJitterPrev, desc.width,
                                      desc.height, impl.nrd.cameraJitterPrev);
}

static bool createScratchImage(cGraphics *graphics, uint32_t width,
                               uint32_t height, RI_Format_e format,
                               uint32_t usage, const char *name,
                               ScratchImage &image) {
  if (!CreateViewportAttachmentTexture(
          &graphics->device, width, height, format, usage,
          RI_VIEWTYPE_SHADER_RESOURCE_2D, &image.texture, &image.view, name))
    return false;
  // The guide targets are rasterized into; the half-res ones are storage-only
  // and need no RTV-typed view.
  if (usage & RI_USAGE_COLOR_ATTACHMENT)
    return CreateViewportColorAttachmentView(&graphics->device, &image.texture,
                                             format, &image.attachmentView);
  return true;
}

bool WaterReflectionViewportState::Impl::ensureScratch() {
  if (hasScratch())
    return true;

  assert(graphics != nullptr);
  if (!createScratchImage(
          graphics, width, height, RI_FORMAT_RGBA32_SFLOAT,
          RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.guidePositionViewZ", guidePositionViewZ) ||
      !createScratchImage(
          graphics, width, height, RI_FORMAT_RGBA16_SFLOAT,
          RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.guideNormalWeight", guideNormalWeight) ||
      !createScratchImage(graphics, width, height,
                          RI_FORMAT_RG16_SFLOAT,
                          RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
                          "WaterReflection.guideVelocity", guideVelocity) ||
      !createScratchImage(
          graphics, halfWidth, halfHeight,
          RI_FORMAT_RGBA32_SFLOAT,
          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.halfPositionViewZ", halfPositionViewZ) ||
      !createScratchImage(
          graphics, halfWidth, halfHeight,
          RI_FORMAT_RGBA16_SFLOAT,
          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.halfNormalWeight", halfNormalWeight) ||
      !createScratchImage(
          graphics, halfWidth, halfHeight,
          RI_FORMAT_R16_SFLOAT,
          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.halfNormalSpread", halfNormalSpread) ||
      !createScratchImage(
          graphics, halfWidth, halfHeight, RI_FORMAT_RG16_SFLOAT,
          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.halfVelocity", halfVelocity) ||
      !createScratchImage(
          graphics, halfWidth, halfHeight,
          RI_FORMAT_RGBA16_SFLOAT,
          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.radianceHitDist", reflectionRadianceHitDist) ||
      !createScratchImage(
          graphics, halfWidth, halfHeight,
          RI_FORMAT_R10_G10_B10_A2_UNORM,
          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.nrdNormalRoughness", nrdNormalRoughness) ||
      !createScratchImage(graphics, halfWidth, halfHeight,
                          RI_FORMAT_R32_SFLOAT,
                          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
                          "WaterReflection.nrdViewZ", nrdViewZ) ||
      !createScratchImage(
          graphics, halfWidth, halfHeight,
          RI_FORMAT_RGBA16_SFLOAT,
          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
          "WaterReflection.nrdSpecularRadianceHitDist",
          nrdSpecularRadianceHitDist) ||
      // Simultaneous access: REBLUR samples IN_MV in its temporal passes and
      // stores it during stabilization, with no transition between, so this
      // one is held in a combined read+write state (see the transition before
      // Denoise below). Vulkan expresses that with GENERAL; on D3D12 only a
      // simultaneous-access texture admits both. Matches the equivalent
      // HybridViewportState.nrdMotionVectors in HybridRenderer.
      !createScratchImage(
          graphics, halfWidth, halfHeight, RI_FORMAT_RG16_SFLOAT,
          RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
              RI_USAGE_SIMULTANEOUS_ACCESS,
          "WaterReflection.nrdMotionVectors", nrdMotionVectors)) {
    releaseScratch();
    return false;
  }
  return true;
}

WaterReflectionResult WaterReflectionPass::RecordSurface(
    RICmd *cmd, WaterReflectionViewportState &state,
    const WaterReflectionSurfaceDesc &surface) {
  WaterReflectionResult result;
  WaterReflectionViewportState::Impl &impl = *state.m_impl;

  // NRD is runtime-loaded and ships separately, so it may be absent. A single
  // ray per half-resolution texel is far too noisy to show undenoised, and the
  // caller already handles an unavailable result (it falls back to the
  // non-raytraced reflection), so retire the pass entirely rather than trace
  // for an image that cannot be filtered.
  if (!NrdIntegration::IsAvailable()) {
    impl.invalidate(surface.renderableCookie);
    return result;
  }

  if (impl.width == 0 || impl.height == 0 || surface.tlas == nullptr ||
      surface.indexCount == 0 || surface.opaqueDepthView == nullptr) {
    // A missing TLAS must never be sent to the RT pipeline. Remove stale
    // history too: an object that was skipped this frame must not reappear
    // with yesterday's reflection when it becomes visible again. The opaque
    // depth view is equally required for the guide's read-only depth test.
    impl.invalidate(surface.renderableCookie);
    return result;
  }

  if (!impl.frameBarrierIssued) {
    // The NRD-owned output was sampled by the caller in FRAGMENT in the
    // previous frame, but this module does not track that texture. Make this
    // cross-frame WAR visible before this frame's NRD COMPUTE writes;
    // NrdIntegration only covers COMPUTE-to-COMPUTE.
    cmd->vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
    impl.frameBarrierIssued = true;
  }

  if (!impl.ensureScratch())
    return result;

  cGraphics *graphics = impl.graphics;
  assert(graphics != nullptr);

  {
    RIGpuScope scope(&graphics->profiler, cmd, "Water.Guide");
    transition(cmd, impl.guidePositionViewZ, RI_RESOURCE_STATE_RENDER_TARGET,
               RI_STAGE_FRAGMENT);
    transition(cmd, impl.guideNormalWeight, RI_RESOURCE_STATE_RENDER_TARGET,
               RI_STAGE_FRAGMENT);
    transition(cmd, impl.guideVelocity, RI_RESOURCE_STATE_RENDER_TARGET,
               RI_STAGE_FRAGMENT);

    RIRenderingAttachment colors[3] = {};
    colors[0].view = *impl.guidePositionViewZ.attachmentView.Get();
    colors[1].view = *impl.guideNormalWeight.attachmentView.Get();
    colors[2].view = *impl.guideVelocity.attachmentView.Get();
    for (RIRenderingAttachment &color : colors) {
      color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      color.clearValue.color[0] = 0.0f;
      color.clearValue.color[1] = 0.0f;
      color.clearValue.color[2] = 0.0f;
      color.clearValue.color[3] = 0.0f;
    }

    RIRenderingAttachment depth = {};
    depth.view = *surface.opaqueDepthView;
    depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
    depth.readOnly = true;

    RIBeginRenderingDesc rendering = {};
    rendering.renderArea.width = impl.width;
    rendering.renderArea.height = impl.height;
    rendering.colorCount = 3;
    rendering.colors = colors;
    rendering.depthStencil = &depth;
    cmd->vk_d3d12_beginRendering(&graphics->device, rendering);

    RIViewport viewport = {};
    viewport.y = static_cast<float>(impl.height);
    viewport.width = static_cast<float>(impl.width);
    viewport.height = -static_cast<float>(impl.height);
    viewport.depthMin = 0.0f;
    viewport.depthMax = 1.0f;
    cmd->setViewport(&graphics->device, viewport);
    RIRect scissor = {};
    scissor.width = impl.width;
    scissor.height = impl.height;
    cmd->setScissor(&graphics->device, scissor);

    m_guide.bindPipeline(&graphics->device, cmd, HASH_INITIAL_VALUE,
                         "Water.guide",
                         MakeWaterGuidePipelineDesc(
                             RI_FORMAT_RGBA32_SFLOAT, RI_FORMAT_RGBA16_SFLOAT,
                             RI_FORMAT_RG16_SFLOAT, cGraphics::DepthFormat,
                             surface.vertexPresentMask));
    m_guide.bindBindlessDescriptorSet(cmd, &graphics->globalset->m_bindlessSet,
                                      uint32_t(0));
    bindGuideResources(graphics, cmd, m_guide, surface, impl.frameIndex);
    cmd->drawIndexed(&graphics->device, surface.indexCount, 1, 0, 0,
                     surface.objectSlot);
    cmd->vk_d3d12_endRendering(&graphics->device);
  }

  {
    RIGpuScope scope(&graphics->profiler, cmd, "Water.Reduce");
    transition(cmd, impl.guidePositionViewZ, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.guideNormalWeight, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.guideVelocity, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.halfPositionViewZ, RI_RESOURCE_STATE_STORAGE_WRITE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.halfNormalWeight, RI_RESOURCE_STATE_STORAGE_WRITE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.halfNormalSpread, RI_RESOURCE_STATE_STORAGE_WRITE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.halfVelocity, RI_RESOURCE_STATE_STORAGE_WRITE,
               RI_STAGE_COMPUTE);

    const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, 0x57524443u);
    m_reduce.bindComputePipeline(&graphics->device, cmd, pipelineHash,
                                 "Water.reduce");
    m_reduce.bindBindlessDescriptorSet(
        cmd, &graphics->globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    std::vector<RIProgram::DescriptorBinding> bindings;
    appendSharedBindings(bindings, surface);
    appendBinding(bindings, "gWaterGuidePosViewZ",
                  RIDescriptor::sampledImage(&graphics->device,
                                              impl.guidePositionViewZ.view.Get()));
    appendBinding(bindings, "gWaterGuideNormalWeight",
                  RIDescriptor::sampledImage(&graphics->device,
                                              impl.guideNormalWeight.view.Get()));
    appendBinding(bindings, "gWaterGuideVelocity",
                  RIDescriptor::sampledImage(&graphics->device,
                                              impl.guideVelocity.view.Get()));
    appendBinding(bindings, "gWaterGuideHalfPosViewZ",
                  RIDescriptor::storageImage(&graphics->device,
                                              impl.halfPositionViewZ.view.Get()));
    appendBinding(bindings, "gWaterGuideHalfNormalWeight",
                  RIDescriptor::storageImage(
                      &graphics->device, impl.halfNormalWeight.view.Get()));
    appendBinding(bindings, "gWaterGuideHalfNormalSpread",
                  RIDescriptor::storageImage(
                      &graphics->device, impl.halfNormalSpread.view.Get()));
    appendBinding(bindings, "gWaterGuideHalfVelocity",
                  RIDescriptor::storageImage(&graphics->device,
                                              impl.halfVelocity.view.Get()));
    bindComputeResources(graphics, cmd, m_reduce, impl.frameIndex, bindings);
    cmd->dispatch(&graphics->device, (impl.halfWidth + 15u) / 16u,
                  (impl.halfHeight + 15u) / 16u, 1);
  }

  {
    RIGpuScope scope(&graphics->profiler, cmd, "Water.Trace");
    transition(cmd, impl.halfPositionViewZ, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_RAY_TRACING);
    transition(cmd, impl.halfNormalWeight, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_RAY_TRACING);
    transition(cmd, impl.halfNormalSpread, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_RAY_TRACING);
    transition(cmd, impl.reflectionRadianceHitDist,
               RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_RAY_TRACING);

    RIRayTracingPipelineDesc pipelineDesc = {};
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.maxPayloadSize = kScatterPayloadSize;
    pipelineDesc.maxAttributeSize = kTriangleAttributeSize;
    const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, 0x57525443u);
    m_trace.bindRayTracingPipeline(&graphics->device, cmd, pipelineHash,
                                   "Water.trace", pipelineDesc);
    m_trace.bindBindlessDescriptorSet(
        cmd, &graphics->globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
    std::vector<RIProgram::DescriptorBinding> bindings;
    appendSharedBindings(bindings, surface);
    appendBinding(bindings, "gWaterGuideHalfPosViewZ",
                  RIDescriptor::sampledImage(
                      &graphics->device, impl.halfPositionViewZ.view.Get(),
                      RI_RESOURCE_STATE_SHADER_RESOURCE));
    appendBinding(bindings, "gWaterGuideHalfNormalWeight",
                  RIDescriptor::sampledImage(
                      &graphics->device, impl.halfNormalWeight.view.Get(),
                      RI_RESOURCE_STATE_SHADER_RESOURCE));
    appendBinding(bindings, "gWaterGuideHalfNormalSpread",
                  RIDescriptor::sampledImage(
                      &graphics->device, impl.halfNormalSpread.view.Get(),
                      RI_RESOURCE_STATE_SHADER_RESOURCE));
    appendBinding(bindings, "gWaterReflectionRadianceHitDist",
                  RIDescriptor::storageImage(
                      &graphics->device, impl.reflectionRadianceHitDist.view.Get()));
    bindTraceResources(graphics, cmd, m_trace, impl.frameIndex, bindings);
    m_trace.traceRays(cmd, pipelineHash, impl.halfWidth, impl.halfHeight, 1);
  }

  {
    RIGpuScope scope(&graphics->profiler, cmd, "Water.Pack");
    transition(cmd, impl.halfPositionViewZ, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.halfNormalWeight, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.halfNormalSpread, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.halfVelocity, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.reflectionRadianceHitDist, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.nrdNormalRoughness, RI_RESOURCE_STATE_STORAGE_WRITE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.nrdViewZ, RI_RESOURCE_STATE_STORAGE_WRITE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.nrdSpecularRadianceHitDist,
               RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE);
    transition(cmd, impl.nrdMotionVectors, RI_RESOURCE_STATE_STORAGE_WRITE,
               RI_STAGE_COMPUTE);

    const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, 0x57504b43u);
    m_pack.bindComputePipeline(&graphics->device, cmd, pipelineHash,
                               "Water.pack");
    m_pack.bindBindlessDescriptorSet(
        cmd, &graphics->globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    std::vector<RIProgram::DescriptorBinding> bindings;
    appendSharedBindings(bindings, surface);
    appendBinding(bindings, "gWaterGuideHalfPosViewZ",
                  RIDescriptor::sampledImage(
                      &graphics->device, impl.halfPositionViewZ.view.Get(),
                      RI_RESOURCE_STATE_SHADER_RESOURCE));
    appendBinding(bindings, "gWaterGuideHalfNormalWeight",
                  RIDescriptor::sampledImage(
                      &graphics->device, impl.halfNormalWeight.view.Get(),
                      RI_RESOURCE_STATE_SHADER_RESOURCE));
    appendBinding(bindings, "gWaterGuideHalfNormalSpread",
                  RIDescriptor::sampledImage(
                      &graphics->device, impl.halfNormalSpread.view.Get(),
                      RI_RESOURCE_STATE_SHADER_RESOURCE));
    appendBinding(bindings, "gWaterGuideHalfVelocity",
                  RIDescriptor::sampledImage(
                      &graphics->device, impl.halfVelocity.view.Get(),
                      RI_RESOURCE_STATE_SHADER_RESOURCE));
    appendBinding(bindings, "gWaterReflectionRadianceHitDist",
                  RIDescriptor::sampledImage(
                      &graphics->device,
                      impl.reflectionRadianceHitDist.view.Get(),
                      RI_RESOURCE_STATE_SHADER_RESOURCE));
    appendBinding(bindings, "gWaterNrdNormalRoughness",
                  RIDescriptor::storageImage(&graphics->device,
                                              impl.nrdNormalRoughness.view.Get()));
    appendBinding(bindings, "gWaterNrdViewZ",
                  RIDescriptor::storageImage(&graphics->device,
                                              impl.nrdViewZ.view.Get()));
    appendBinding(bindings, "gWaterNrdSpecularRadianceHitDist",
                  RIDescriptor::storageImage(
                      &graphics->device,
                      impl.nrdSpecularRadianceHitDist.view.Get()));
    appendBinding(bindings, "gWaterNrdMotionVectors",
                  RIDescriptor::storageImage(
                      &graphics->device, impl.nrdMotionVectors.view.Get()));
    bindComputeResources(graphics, cmd, m_pack, impl.frameIndex, bindings);
    cmd->dispatch(&graphics->device, (impl.halfWidth + 15u) / 16u,
                  (impl.halfHeight + 15u) / 16u, 1);

    transition(cmd, impl.nrdNormalRoughness, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.nrdViewZ, RI_RESOURCE_STATE_SHADER_RESOURCE,
               RI_STAGE_COMPUTE);
    transition(cmd, impl.nrdSpecularRadianceHitDist,
               RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE);
    transition(cmd, impl.nrdMotionVectors,
               RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
               RI_STAGE_COMPUTE);
  }

  RITextureView *denoisedView = nullptr;
  {
    RIGpuScope scope(&graphics->profiler, cmd, "Water.Denoise");
    auto historyIt = impl.histories.find(surface.renderableCookie);
    if (historyIt == impl.histories.end()) {
      WaterReflectionViewportState::Impl::History history;
      history.nrd = std::make_shared<NrdIntegration>(
          graphics, NrdDenoiserMode::Specular);
      history.nrdWidth = impl.halfWidth;
      history.nrdHeight = impl.halfHeight;
      historyIt = impl.histories.emplace(surface.renderableCookie,
                                         std::move(history)).first;
      historyIt->second.nrd->OnResize(impl.halfWidth, impl.halfHeight);
    } else if (historyIt->second.nrdWidth != impl.halfWidth ||
               historyIt->second.nrdHeight != impl.halfHeight) {
      historyIt->second.nrd->OnResize(impl.halfWidth, impl.halfHeight);
      historyIt->second.nrdWidth = impl.halfWidth;
      historyIt->second.nrdHeight = impl.halfHeight;
      historyIt->second.hasSuccessfulDenoise = false;
    }

    auto &history = historyIt->second;
    const bool materialChanged =
        history.hasSuccessfulDenoise &&
        history.materialSignature != surface.materialSignature;
    const bool frameSequenceBroken =
        history.hasSuccessfulDenoise &&
        impl.frameIndex != history.lastFrameIndex + 1u;
    if (!history.hasSuccessfulDenoise || materialChanged || impl.resetHistory ||
        frameSequenceBroken)
      history.nrd->ResetHistory();

    history.usedThisFrame = true;
    NrdDenoiseInputs inputs = {};
    inputs.normalRoughness = impl.nrdNormalRoughness.view.Get();
    inputs.viewZ = impl.nrdViewZ.view.Get();
    inputs.motionVectors = impl.nrdMotionVectors.view.Get();
    // Specular mode intentionally has no diffuse input. The radiance signal
    // is already packed as a specular lobe by WaterReflectionPack.
    inputs.specularRadianceHitDistance =
        impl.nrdSpecularRadianceHitDist.view.Get();
    const NrdDenoiseOutputs outputs =
        history.nrd->Denoise(cmd, impl.nrd, inputs);
    denoisedView = outputs.specularRadianceHitDistance;
    if (denoisedView != nullptr) {
      history.lastFrameIndex = impl.frameIndex;
      history.materialSignature = surface.materialSignature;
      history.hasSuccessfulDenoise = true;

      // Half guides are already in the sampled state after their compute
      // reads. Retain that layout for the fragment descriptors, while adding
      // the explicit compute-to-fragment dependency for both guides and NRD's
      // output.
      transition(cmd, impl.halfPositionViewZ, RI_RESOURCE_STATE_SHADER_RESOURCE,
                 RI_STAGE_FRAGMENT);
      transition(cmd, impl.halfNormalWeight, RI_RESOURCE_STATE_SHADER_RESOURCE,
                 RI_STAGE_FRAGMENT);
      cmd->vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_FRAGMENT});
    }
  }

  if (!denoisedView)
    return result;
  result.available = true;
  result.specularRadianceHitDist = denoisedView;
  result.halfPositionViewZ = impl.halfPositionViewZ.view.Get();
  result.halfNormalWeight = impl.halfNormalWeight.view.Get();
  result.width = impl.halfWidth;
  result.height = impl.halfHeight;
  return result;
}

void WaterReflectionPass::EndFrame(WaterReflectionViewportState &state) {
  WaterReflectionViewportState::Impl &impl = *state.m_impl;
  if (!impl.graphics)
    return;
  for (auto it = impl.histories.begin(); it != impl.histories.end();) {
    if (it->second.usedThisFrame) {
      ++it;
      continue;
    }
    // A surface that does not return this frame is intentionally retired; a
    // later return gets a fresh history instead of accumulating invisible
    // objects in the per-viewport cache.
    deferNrd(impl.graphics, std::move(it->second.nrd));
    it = impl.histories.erase(it);
  }
}

} // namespace hpl
