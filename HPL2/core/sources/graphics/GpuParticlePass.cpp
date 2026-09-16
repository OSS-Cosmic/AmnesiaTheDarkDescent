#include "graphics/GpuParticlePass.h"

#include "graphics/GlobalManagedSets.h"
#include "graphics/GpuParticles.h"
#include "graphics/RIGpuProfiler.h"
#include "graphics/RIVK.h"
#include "resources/Resources.h"
#include "system/LowLevelSystem.h"

#include <array>
#include <vector>

namespace hpl {

namespace {

// Matches ParticleSpawnPC in Standard.particleSpawn.cs.slang.
struct SpawnPushBlock {
  uint32_t spawnCount;
  uint32_t pad0;
  uint32_t pad1;
  uint32_t pad2;
};

// Matches ParticleSimPC in Standard.particleSim.cs.slang.
struct SimPushBlock {
  uint32_t groupCount;
  uint32_t collisionEnabled;
  float collisionThickness;
  uint32_t pad0;
};

// Matches ParticleExpandPC in Standard.particleExpand.cs.slang.
struct ExpandPushBlock {
  uint32_t groupCount;
  uint32_t invertCull;
  float cameraOriginX;
  float cameraOriginY;
  float cameraOriginZ;
  uint32_t pad0;
  uint32_t pad1;
  uint32_t pad2;
};

} // namespace

//-----------------------------------------------------------------------

cGpuParticlePass::cGpuParticlePass(cGraphics *apGraphics,
                                   cResources *apResources)
    : mpGraphics(apGraphics), mpResources(apResources) {}

cGpuParticlePass::~cGpuParticlePass() { DestroyData(); }

//-----------------------------------------------------------------------

bool cGpuParticlePass::LoadData() {
  // Idempotent: the renderers call LoadData on reload while a frame may already
  // be in flight, and replacing a live program every call would invalidate the
  // pipeline cache and defer a module for nothing.
  if (m_loaded && m_spawn && m_sim && m_expand)
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;

  const VkDescriptorSetLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.vk.m_bindlessSetLayout};

  auto load = [&](std::shared_ptr<RIProgram> &target, const char *name) {
    auto bin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(), name);
    if (bin.empty()) {
      Error("cGpuParticlePass: %s not found.\n", name);
      return false;
    }
    auto program = std::make_shared<RIProgram>();
    std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
        RIProgram::PROGRAM_STAGE_COMPUTE, bin, "csMain"}};
    program->initialize(&mpGraphics->device, stages, external, name);

    auto old = std::move(target);
    target = std::move(program);
    if (old)
      mpGraphics->graphicsDefer.push(std::function<void()>(
          [old = std::move(old), device = &mpGraphics->device]() mutable {
            old->dispose(device);
          }));
    return true;
  };

  if (!load(m_spawn, "Standard.particleSpawn.cs.spv"))
    return false;
  if (!load(m_sim, "Standard.particleSim.cs.spv"))
    return false;
  if (!load(m_expand, "Standard.particleExpand.cs.spv"))
    return false;

  m_loaded = true;
  return true;
}

//-----------------------------------------------------------------------

void cGpuParticlePass::DestroyData() {
  auto retire = [&](std::shared_ptr<RIProgram> &target) {
    auto old = std::move(target);
    if (old && mpGraphics)
      mpGraphics->graphicsDefer.push(std::function<void()>(
          [old = std::move(old), device = &mpGraphics->device]() mutable {
            old->dispose(device);
          }));
  };
  retire(m_spawn);
  retire(m_sim);
  retire(m_expand);
  m_loaded = false;
}

//-----------------------------------------------------------------------

void cGpuParticlePass::BindCommon(
    RICmd *apCmd, uint32_t alFrameIndex, RIProgram *apProgram,
    const char *asDebugName, const FrameBuffers &aBuffers,
    RIProgram::DescriptorBinding *apFrameBinding,
    std::vector<RIProgram::DescriptorBinding> &aBindings) {
  VkComputePipelineCreateInfo createInfo = {
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
  apProgram->bindComputePipeline(&mpGraphics->device, apCmd, kHash, asDebugName,
                                 &createInfo);
  apProgram->bindBindlessDescriptorSet(apCmd, &mpGraphics->globalset->m_bindlessSet,
                                       0, VK_PIPELINE_BIND_POINT_COMPUTE);

  aBindings.clear();
  // gPerFrame (set 1) is only reflected by sim and expand -- spawn is
  // view-independent and does not import it -- so it is optional here.
  if (apFrameBinding)
    aBindings.push_back(*apFrameBinding);

  aBindings.emplace_back(
      "gParticleState",
      RIDescriptor::storageBuffer(&mpGraphics->device, aBuffers.state,
                                  aBuffers.stateOffset, aBuffers.stateRange));
  aBindings.emplace_back(
      "gParticleEmitters",
      RIDescriptor::storageBuffer(&mpGraphics->device, aBuffers.emitters,
                                  aBuffers.emitterOffset,
                                  aBuffers.emitterRange));
  aBindings.emplace_back(
      "gParticleEmitterParams",
      RIDescriptor::storageBuffer(&mpGraphics->device, aBuffers.params,
                                  aBuffers.paramOffset, aBuffers.paramRange));
}

//-----------------------------------------------------------------------

void cGpuParticlePass::RecordSpawn(RICmd *apCmd, uint32_t alFrameIndex,
                                   const SpawnArgs &aArgs) {
  if (!IsLoaded() || !apCmd || aArgs.spawnCount == 0 || !aArgs.spawnCmds)
    return;

  RIGpuScope scope(&mpGraphics->profiler, apCmd, "GpuParticles.spawn");

  std::vector<RIProgram::DescriptorBinding> bindings;
  BindCommon(apCmd, alFrameIndex, m_spawn.get(), "Standard.particleSpawn",
             aArgs.buffers, nullptr, bindings);
  bindings.emplace_back(
      "gParticleSpawnCmds",
      RIDescriptor::storageBuffer(&mpGraphics->device, aArgs.spawnCmds,
                                  aArgs.spawnOffset, aArgs.spawnRange));
  m_spawn->bindDescriptors(&mpGraphics->device, apCmd, alFrameIndex,
                           bindings.data(), (uint32_t)bindings.size(),
                           VK_PIPELINE_BIND_POINT_COMPUTE);

  SpawnPushBlock push = {aArgs.spawnCount, 0u, 0u, 0u};
  apCmd->vk_d3d12_setPushConstants(&mpGraphics->device, *m_spawn, 0,
                                   sizeof(push), &push);

  // One thread per command, not per pool slot: the host scheduler already
  // picked the exact slot each command initialises.
  apCmd->dispatch(&mpGraphics->device,
                  (aArgs.spawnCount + kGpuParticleGroupSize - 1u) /
                      kGpuParticleGroupSize,
                  1u, 1u);
}

//-----------------------------------------------------------------------

void cGpuParticlePass::RecordSim(RICmd *apCmd, uint32_t alFrameIndex,
                                 RIProgram::DescriptorBinding *apFrameBinding,
                                 const SimArgs &aArgs) {
  if (!IsLoaded() || !apCmd || !apFrameBinding || aArgs.work.groupCount == 0)
    return;
  // A reflected-but-unwritten binding is a debug-build error in
  // bindDescriptors, so the depth view is required even with collision off.
  if (!aArgs.collisionDepth)
    return;

  RIGpuScope scope(&mpGraphics->profiler, apCmd, "GpuParticles.sim");

  std::vector<RIProgram::DescriptorBinding> bindings;
  BindCommon(apCmd, alFrameIndex, m_sim.get(), "Standard.particleSim",
             aArgs.buffers, apFrameBinding, bindings);
  bindings.emplace_back(
      "gParticleGroups",
      RIDescriptor::storageBuffer(&mpGraphics->device, aArgs.work.groups,
                                  aArgs.work.offset, aArgs.work.range));
  bindings.emplace_back(
      "gParticleCollisionDepth",
      RIDescriptor::sampledImage(&mpGraphics->device, aArgs.collisionDepth,
                                 RI_RESOURCE_STATE_SHADER_RESOURCE));
  m_sim->bindDescriptors(&mpGraphics->device, apCmd, alFrameIndex,
                         bindings.data(), (uint32_t)bindings.size(),
                         VK_PIPELINE_BIND_POINT_COMPUTE);

  SimPushBlock push = {aArgs.work.groupCount,
                       aArgs.collisionEnabled ? 1u : 0u,
                       aArgs.collisionThickness, 0u};
  apCmd->vk_d3d12_setPushConstants(&mpGraphics->device, *m_sim, 0, sizeof(push),
                                   &push);

  apCmd->dispatch(&mpGraphics->device, aArgs.work.groupCount, 1u, 1u);
}

//-----------------------------------------------------------------------

void cGpuParticlePass::RecordExpand(RICmd *apCmd, uint32_t alFrameIndex,
                                    RIProgram::DescriptorBinding *apFrameBinding,
                                    const ExpandArgs &aArgs) {
  if (!IsLoaded() || !apCmd || !apFrameBinding || aArgs.work.groupCount == 0)
    return;
  if (!aArgs.sliceOffsets || !aArgs.quadVerts)
    return;

  RIGpuScope scope(&mpGraphics->profiler, apCmd, "GpuParticles.expand");

  std::vector<RIProgram::DescriptorBinding> bindings;
  BindCommon(apCmd, alFrameIndex, m_expand.get(), "Standard.particleExpand",
             aArgs.buffers, apFrameBinding, bindings);
  bindings.emplace_back(
      "gParticleGroups",
      RIDescriptor::storageBuffer(&mpGraphics->device, aArgs.work.groups,
                                  aArgs.work.offset, aArgs.work.range));
  bindings.emplace_back(
      "gParticleSliceOffset",
      RIDescriptor::storageBuffer(&mpGraphics->device, aArgs.sliceOffsets,
                                  aArgs.sliceOffsetOffset,
                                  aArgs.sliceOffsetRange));
  bindings.emplace_back(
      "gParticleQuadVerts",
      RIDescriptor::storageBuffer(&mpGraphics->device, aArgs.quadVerts,
                                  aArgs.quadOffset, aArgs.quadRange));
  m_expand->bindDescriptors(&mpGraphics->device, apCmd, alFrameIndex,
                            bindings.data(), (uint32_t)bindings.size(),
                            VK_PIPELINE_BIND_POINT_COMPUTE);

  ExpandPushBlock push = {aArgs.work.groupCount,
                          aArgs.invertCull ? 1u : 0u,
                          aArgs.cameraOrigin[0],
                          aArgs.cameraOrigin[1],
                          aArgs.cameraOrigin[2],
                          0u,
                          0u,
                          0u};
  apCmd->vk_d3d12_setPushConstants(&mpGraphics->device, *m_expand, 0,
                                   sizeof(push), &push);

  apCmd->dispatch(&mpGraphics->device, aArgs.work.groupCount, 1u, 1u);
}

//-----------------------------------------------------------------------

void cGpuParticlePass::BarrierSpawnToSim(RICmd *apCmd) {
  if (!apCmd)
    return;
  apCmd->vk_d3d12_memoryBarrier(
      {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_UNORDERED_ACCESS,
       RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
}

void cGpuParticlePass::BarrierSimToExpand(RICmd *apCmd) {
  if (!apCmd)
    return;
  apCmd->vk_d3d12_memoryBarrier(
      {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
       RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
}

void cGpuParticlePass::BarrierExpandToDraw(RICmd *apCmd) {
  if (!apCmd)
    return;
  // Both destination classes are required. Standard and Hybrid pull the quad
  // streams by buffer device address inside the vertex shader (SHADER_RESOURCE),
  // while RendererSimple and RendererWireFrame bind the same memory as a real
  // vertex buffer (VERTEX_BUFFER). Dropping either bit is silent on some
  // drivers and garbage geometry on others.
  apCmd->vk_d3d12_memoryBarrier(
      {RI_RESOURCE_STATE_STORAGE_WRITE,
       RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_VERTEX_BUFFER |
           RI_RESOURCE_STATE_INDEX_BUFFER,
       RI_STAGE_COMPUTE, RI_STAGE_VERTEX});
}

//-----------------------------------------------------------------------

} // namespace hpl
