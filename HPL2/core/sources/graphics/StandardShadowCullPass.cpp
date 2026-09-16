#include "graphics/StandardShadowCullPass.h"

#include "graphics/GlobalManagedSets.h"
#include "graphics/RIBarrier.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIRenderer.h"
#include "resources/Resources.h"

#include <array>
#include <vector>

namespace hpl {

cStandardShadowCullPass::cStandardShadowCullPass(cGraphics *graphics,
                                                 cResources *resources)
    : mpGraphics(graphics), mpResources(resources) {}

cStandardShadowCullPass::~cStandardShadowCullPass() { DestroyData(); }

bool cStandardShadowCullPass::LoadData() {
  if (m_loaded && m_reset && m_cull)
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;

  const VkDescriptorSetLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.vk.m_bindlessSetLayout};

  auto load = [&](std::shared_ptr<RIProgram> *slot, const char *entryPoint) {
    auto bin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                          "Standard.cull.cs.spv");
    if (bin.empty())
      return false;
    auto program = std::make_shared<RIProgram>();
    std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
        RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
    program->initialize(&mpGraphics->device, stages, external, entryPoint);
    auto old = std::move(*slot);
    *slot = std::move(program);
    if (old) {
      mpGraphics->graphicsDefer.push(std::function<void()>(
          [old = std::move(old), device = &mpGraphics->device]() mutable {
            old->dispose(device);
          }));
    }
    return true;
  };

  if (!load(&m_reset, "cullShadowResetCounts"))
    return false;
  if (!load(&m_cull, "cullShadowTiles"))
    return false;

  // Latched here rather than per frame: the kernel's mode and the draw call
  // have to agree, and both are derived from this one answer.
  m_useDrawIndirectCount =
      mpGraphics->device.physicalAdapter.isDrawIndirectCountSupported != 0;

  // 1x1 stand-in for gCullHiZ. The kernel reflects the binding on every
  // dispatch, including the shadow ones that never sample it, and an unwritten
  // binding is a descriptor the shader reads undefined.
  if (!m_hiZFallbackReady) {
    RITextureDesc td{};
    td.type = RI_TEXTURE_2D;
    td.format = RI_FORMAT_R32_SFLOAT;
    td.width = 1;
    td.height = 1;
    td.depth = 1;
    td.layerNum = 1;
    td.mipNum = 1;
    td.sampleCount = 1;
    td.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_SHADER_RESOURCE_STORAGE;
    RITexture texture = RITexture::create(&mpGraphics->device, td);
    m_hiZFallback = RISharedPointer<RITexture>(&mpGraphics->device, texture);
    if (!m_hiZFallback.isEmpty()) {
      RITextureViewDesc vd{};
      vd.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      vd.format = RI_FORMAT_R32_SFLOAT;
      vd.mipNum = 1;
      vd.layerNum = 1;
      RITextureView view =
          RITextureView::create(&mpGraphics->device, m_hiZFallback.Get(), vd);
      m_hiZFallbackView =
          RISharedPointer<RITextureView>(&mpGraphics->device, view);
    }
    m_hiZFallbackReady =
        !m_hiZFallback.isEmpty() && !m_hiZFallbackView.isEmpty();
    if (!m_hiZFallbackReady) {
      m_hiZFallbackView = {};
      m_hiZFallback = {};
      return false;
    }
    // Never sampled with a meaningful result, but it must leave UNDEFINED
    // before it can be bound as a shader resource.
    m_hiZFallbackPendingTransition = true;
  }

  m_loaded = true;
  return true;
}

void cStandardShadowCullPass::DestroyData() {
  // Deferred, like LoadData's reload path: a program whose pipeline is still
  // referenced by an in-flight command buffer must not be destroyed inline.
  auto dispose = [&](std::shared_ptr<RIProgram> &program) {
    if (program && mpGraphics) {
      mpGraphics->graphicsDefer.push(std::function<void()>(
          [old = std::move(program), device = &mpGraphics->device]() mutable {
            old->dispose(device);
          }));
    }
    program.reset();
  };
  dispose(m_reset);
  dispose(m_cull);
  if (mpGraphics) {
    if (!m_hiZFallbackView.isEmpty())
      mpGraphics->graphicsDefer.push(m_hiZFallbackView);
    if (!m_hiZFallback.isEmpty())
      mpGraphics->graphicsDefer.push(m_hiZFallback);
  }
  m_hiZFallbackView = {};
  m_hiZFallback = {};
  m_hiZFallbackReady = false;
  m_hiZFallbackPendingTransition = false;
  m_loaded = false;
}

bool cStandardShadowCullPass::Dispatch(RICmd *cmd, uint32_t frameIndex,
                                       const Buffers &buffers,
                                       uint32_t tileBase, uint32_t tileCount,
                                       uint32_t groupBase,
                                       uint32_t groupCount, uint32_t mode,
                                       uint32_t commandWordDelta) {
  if (!IsLoaded() || !cmd)
    return false;
  if (!buffers.candidates || !buffers.tiles || !buffers.groups ||
      !buffers.indirect || !buffers.drawCounts || !buffers.cameras ||
      !buffers.visibility)
    return false;
  // Nothing to cull is a success: the caller simply publishes no tiles.
  if (tileCount == 0 || groupCount == 0)
    return true;
  if (uint64_t(tileBase) + tileCount > buffers.tileCapacity ||
      uint64_t(groupBase) + groupCount > buffers.groupCapacity)
    return false;

  RIGpuScope _gs(&mpGraphics->profiler, cmd, "StandardShadow.cull");

  // Matches StandardCullPC in Standard.cull.cs.slang.
  struct PushConstants {
    uint32_t cullMode;
    uint32_t tileBase;
    uint32_t tileCount;
    uint32_t groupBase;
    uint32_t groupCount;
    uint32_t commandWordDelta;
    uint32_t pad1;
    uint32_t pad2;
  } constants{};
  constants.cullMode =
      mode != kModeAuto
          ? mode
          : (m_useDrawIndirectCount ? kStandardCullModeCompact
                                    : kStandardCullModeInPlace);
  constants.tileBase = tileBase;
  constants.tileCount = tileCount;
  constants.groupBase = groupBase;
  constants.groupCount = groupCount;
  constants.commandWordDelta = commandWordDelta;

  std::vector<RIProgram::DescriptorBinding> bindings;
  const auto bind = [&](const char *name, RIBuffer *buffer, uint64_t stride,
                        uint64_t count) {
    bindings.push_back(RIProgram::DescriptorBinding(
        name, RIDescriptor::storageBuffer(&mpGraphics->device, buffer, 0,
                                          std::max<uint64_t>(1, count) * stride)));
  };

  const auto record = [&](const std::shared_ptr<RIProgram> &program,
                          const char *debugName, uint32_t groups) {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t hash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    program->bindComputePipeline(&mpGraphics->device, cmd, hash, debugName,
                                 &computeCreate);
    program->bindBindlessDescriptorSet(cmd, &mpGraphics->globalset->m_bindlessSet,
                                       0, VK_PIPELINE_BIND_POINT_COMPUTE);
    program->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                             bindings.data(), bindings.size(),
                             VK_PIPELINE_BIND_POINT_COMPUTE);
    cmd->vk_d3d12_setPushConstants(&mpGraphics->device, *program, 0,
                                   sizeof(constants), &constants);
    cmd->dispatch(&mpGraphics->device, groups, 1, 1);
  };

  // The reset kernel touches only the tile and count buffers, but binding the
  // same set for both dispatches keeps one descriptor layout across the pass.
  bind("gShadowCullCandidates", buffers.candidates, sizeof(StandardCullCandidate),
       buffers.candidateCapacity);
  bind("gShadowCullTiles", buffers.tiles, sizeof(StandardCullTile),
       buffers.tileCapacity);
  bind("gShadowCullGroups", buffers.groups, sizeof(StandardCullGroup),
       buffers.groupCapacity);
  bind("gShadowIndirect", buffers.indirect, sizeof(VkDrawIndirectCommand),
       buffers.indirectCapacity);
  bind("gShadowDrawCounts", buffers.drawCounts, sizeof(uint32_t),
       buffers.drawCountCapacity);
  bind("gCullCameras", buffers.cameras, sizeof(StandardCullCamera),
       buffers.cameraCapacity);
  bind("gCullVisibility", buffers.visibility, sizeof(uint32_t),
       buffers.visibilityCapacity);
  // Same buffer as gShadowIndirect, as words: INSTANCE_MASK mode rewrites one
  // word of a command rather than the whole command.
  bind("gCullIndirectWords", buffers.indirect, sizeof(uint32_t),
       buffers.indirectWordCapacity > 0
           ? buffers.indirectWordCapacity
           : buffers.indirectCapacity *
                 (sizeof(VkDrawIndirectCommand) / sizeof(uint32_t)));

  RITextureView *hiZ = buffers.hiZ ? buffers.hiZ : m_hiZFallbackView.Get();
  if (!hiZ)
    return false;
  if (hiZ == m_hiZFallbackView.Get() && m_hiZFallbackPendingTransition) {
    cmd->vk_d3d12_textureBarrier(RITextureBarrier(
        m_hiZFallback.Get(), RI_RESOURCE_STATE_UNDEFINED,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE, RI_STAGE_COMPUTE,
        RI_BARRIER_ASPECT_COLOR));
    m_hiZFallbackPendingTransition = false;
  }
  bindings.push_back(RIProgram::DescriptorBinding(
      "gCullHiZ", RIDescriptor::sampledImage(&mpGraphics->device, hiZ)));

  // Only compact mode touches the counters. In-place gives every candidate a
  // fixed slot and instance-mask rewrites one word of a command the host
  // already built, so for both the reset and its barrier are pure overhead.
  if (constants.cullMode == kStandardCullModeCompact) {
    const uint32_t resetGroups =
        (tileCount + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;
    record(m_reset, "Standard.cull.cs:cullShadowResetCounts", resetGroups);

    // The cull's InterlockedAdd must see the zeroed counters, so the reset has
    // to complete first. A plain buffer barrier is enough: same queue, same
    // stage.
    cmd->vk_d3d12_bufferBarrier(RIBufferBarrier(
        buffers.drawCounts, RI_RESOURCE_STATE_STORAGE_WRITE,
        RI_RESOURCE_STATE_UNORDERED_ACCESS, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE));
  }

  record(m_cull, "Standard.cull.cs:cullShadowTiles", groupCount);

  // Hand both buffers to the draw. Counts are only read as a draw count when
  // the device supports it, but the transition is harmless either way and
  // keeps the state machine consistent.
  RIBufferBarrier toIndirect[2] = {
      RIBufferBarrier(buffers.indirect, RI_RESOURCE_STATE_STORAGE_WRITE,
                      RI_RESOURCE_STATE_INDIRECT_ARGUMENT, RI_STAGE_COMPUTE,
                      RI_STAGE_DRAW_INDIRECT),
      RIBufferBarrier(buffers.drawCounts, RI_RESOURCE_STATE_STORAGE_WRITE,
                      RI_RESOURCE_STATE_INDIRECT_ARGUMENT, RI_STAGE_COMPUTE,
                      RI_STAGE_DRAW_INDIRECT)};
  cmd->vk_d3d12_resourceBarrier<0, 2, 0>(0, nullptr, 2, toIndirect, 0, nullptr);
  return true;
}

} // namespace hpl
