#include "graphics/StandardHiZPass.h"

#include "graphics/GlobalManagedSets.h"
#include "graphics/RIBarrier.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIRenderer.h"
#include "resources/Resources.h"

#include <algorithm>
#include <array>
#include <vector>

namespace hpl {

cStandardHiZPass::cStandardHiZPass(cGraphics *graphics, cResources *resources)
    : mpGraphics(graphics), mpResources(resources) {}

cStandardHiZPass::~cStandardHiZPass() { DestroyData(); }

bool cStandardHiZPass::LoadData() {
  if (m_loaded && m_program)
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;

  auto bin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                        "Standard.hiz.cs", "hizReduce");
  if (bin.empty())
    return false;

  const RIBindlessLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.layout()};
  auto program = std::make_shared<RIProgram>();
  std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
      RIProgram::PROGRAM_STAGE_COMPUTE, bin, "hizReduce"}};
  program->initialize(&mpGraphics->device, stages, external, "Standard.hiz.cs");

  auto old = std::move(m_program);
  m_program = std::move(program);
  if (old) {
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [old = std::move(old), device = &mpGraphics->device]() mutable {
          old->dispose(device);
        }));
  }
  m_loaded = true;
  return true;
}

void cStandardHiZPass::DestroyData() {
  auto old = std::move(m_program);
  if (old && mpGraphics) {
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [old = std::move(old), device = &mpGraphics->device]() mutable {
          old->dispose(device);
        }));
  }
  m_program.reset();
  m_loaded = false;
}

bool cStandardHiZPass::Build(RICmd *cmd, uint32_t frameIndex,
                             HiZPyramid &pyramid, uint32_t image,
                             uint32_t sourceWidth, uint32_t sourceHeight,
                             RITextureView *depth) {
  if (!IsLoaded() || !cmd || !depth)
    return false;
  if (!pyramid.IsUsable(image))
    return false;
  // Validate every level before the opening barrier below. Bailing from inside
  // the loop would leave the texture in UNORDERED_ACCESS with no closing
  // transition, desyncing the state the next pass declares.
  for (uint32_t mip = 0; mip < pyramid.mipCount; ++mip)
    if (pyramid.mipView[image][mip].isEmpty())
      return false;

  RIGpuScope _gs(&mpGraphics->profiler, cmd, "Standard.hiz");

  // From UNDEFINED, not from the previous frame's SHADER_RESOURCE: every texel
  // of every level is overwritten below, so discarding the old contents is
  // correct and saves tracking per-image initialization. The sync still waits
  // on compute: the cull may have sampled this pyramid earlier in the same
  // command list, and D3D12 rejects SyncBefore NONE for an accessed resource.
  cmd->vk_d3d12_textureBarrier(RITextureBarrier(
      pyramid.texture[image].Get(), RI_RESOURCE_STATE_UNDEFINED,
      RI_RESOURCE_STATE_UNORDERED_ACCESS, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE,
      RI_BARRIER_ASPECT_COLOR));

  struct PushConstants {
    uint32_t targetWidth, targetHeight;
    uint32_t sourceWidth, sourceHeight;
    uint32_t sourceMip;
    uint32_t sourceIsDepth;
    uint32_t pad0, pad1;
  } constants{};

  for (uint32_t mip = 0; mip < pyramid.mipCount; ++mip) {
    const uint32_t targetWidth = std::max<uint32_t>(1u, pyramid.width >> mip);
    const uint32_t targetHeight = std::max<uint32_t>(1u, pyramid.height >> mip);
    constants.targetWidth = targetWidth;
    constants.targetHeight = targetHeight;
    if (mip == 0) {
      // Level 0 reduces the full-resolution scene depth.
      constants.sourceWidth = sourceWidth;
      constants.sourceHeight = sourceHeight;
      constants.sourceMip = 0;
      constants.sourceIsDepth = 1;
    } else {
      constants.sourceWidth =
          std::max<uint32_t>(1u, pyramid.width >> (mip - 1));
      constants.sourceHeight =
          std::max<uint32_t>(1u, pyramid.height >> (mip - 1));
      constants.sourceMip = mip - 1;
      constants.sourceIsDepth = 0;
    }

    std::vector<RIProgram::DescriptorBinding> bindings;
    bindings.push_back(RIProgram::DescriptorBinding(
        "gHiZDepth", RIDescriptor::sampledImage(&mpGraphics->device, depth)));
    // The pyramid is in GENERAL for the whole build, so its sampled descriptor
    // has to declare the storage state too -- a SHADER_RESOURCE descriptor
    // would ask for SHADER_READ_ONLY_OPTIMAL and disagree with the image.
    bindings.push_back(RIProgram::DescriptorBinding(
        "gHiZSource", RIDescriptor::sampledImage(
                          &mpGraphics->device, pyramid.sampleView[image].Get(),
                          RI_RESOURCE_STATE_UNORDERED_ACCESS)));
    bindings.push_back(RIProgram::DescriptorBinding(
        "gHiZTarget",
        RIDescriptor::storageImage(&mpGraphics->device,
                                   pyramid.mipView[image][mip].Get())));

    const hash_t hash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_program->bindComputePipeline(&mpGraphics->device, cmd, hash,
                                   "Standard.hiz.cs:hizReduce");
    m_program->bindBindlessDescriptorSet(cmd,
                                         &mpGraphics->globalset->m_bindlessSet,
                                         0, VK_PIPELINE_BIND_POINT_COMPUTE);
    m_program->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                               bindings.data(), bindings.size(),
                               VK_PIPELINE_BIND_POINT_COMPUTE);
    cmd->vk_d3d12_setPushConstants(&mpGraphics->device, *m_program, 0,
                                   sizeof(constants), &constants);
    cmd->dispatch(&mpGraphics->device, (targetWidth + 7u) / 8u,
                  (targetHeight + 7u) / 8u, 1);

    // The next level reads what this one just wrote.
    if (mip + 1u < pyramid.mipCount) {
      cmd->vk_d3d12_textureBarrier(RITextureBarrier(
          pyramid.texture[image].Get(), RI_RESOURCE_STATE_UNORDERED_ACCESS,
          RI_RESOURCE_STATE_UNORDERED_ACCESS, RI_STAGE_COMPUTE,
          RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_COLOR));
    }
  }

  cmd->vk_d3d12_textureBarrier(RITextureBarrier(
      pyramid.texture[image].Get(), RI_RESOURCE_STATE_UNORDERED_ACCESS,
      RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE,
      RI_BARRIER_ASPECT_COLOR));
  return true;
}

} // namespace hpl
