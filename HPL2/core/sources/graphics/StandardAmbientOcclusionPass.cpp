#include "graphics/StandardAmbientOcclusionPass.h"

#include "graphics/Graphics.h"
#include "graphics/GlobalManagedSets.h"
#include "math/Frustum.h"
#include "graphics/RIProgram.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIBarrier.h"
#include "graphics/RIGpuProfiler.h"
#include "resources/Resources.h"
#include "system/Hasher.h"

#include <array>
#include <functional>
#include <vector>

namespace hpl {

cStandardAmbientOcclusionPass::cStandardAmbientOcclusionPass(
    cGraphics *graphics, cResources *resources)
    : mpGraphics(graphics), mpResources(resources) {}

cStandardAmbientOcclusionPass::~cStandardAmbientOcclusionPass() {
  DestroyData();
}

bool cStandardAmbientOcclusionPass::LoadData() {
  if (m_loaded && m_programs[0])
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;

  const RIBindlessLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.layout()};

  auto loadSlangCompute = [&](int idx, const char *name,
                              const char *entryPoint) {
    auto bin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(), name,
                                          entryPoint);
    if (bin.empty())
      return false;
    auto program = std::make_shared<RIProgram>();
    std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
        RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
    program->initialize(&mpGraphics->device, stages, external, name);
    auto old = std::move(m_programs[idx]);
    m_programs[idx] = std::move(program);
    if (old) {
      mpGraphics->graphicsDefer.push(std::function<void()>(
          [old = std::move(old), device = &mpGraphics->device]() mutable {
            old->dispose(device);
          }));
    }
    return true;
  };

  if (!loadSlangCompute(0, "Standard.aoPrepareDepths.cs", "csMain"))
    return false;
  if (!loadSlangCompute(1, "Standard.aoCoarse.cs", "csMain"))
    return false;
  if (!loadSlangCompute(2, "Standard.aoReinterleave.cs", "csMain"))
    return false;

  m_loaded = true;
  return true;
}

void cStandardAmbientOcclusionPass::DestroyData() {
  for (int i = 0; i < 3; ++i) {
    auto old = std::move(m_programs[i]);
    if (old) {
      mpGraphics->graphicsDefer.push(std::function<void()>(
          [old = std::move(old), device = &mpGraphics->device]() mutable {
            old->dispose(device);
          }));
    }
  }
  m_loaded = false;
}

// Returns true only when aoTexture holds this frame's AO in SHADER_RESOURCE;
// on false the renderer binds its cleared "unoccluded" fallback instead.
bool cStandardAmbientOcclusionPass::Render(
    cGraphics::FrameContext *frame, RICmd *cmd, uint32_t frameIndex,
    cViewport::StandardViewportState *state, uint32_t image, cFrustum *frustum,
    RIProgram::DescriptorBinding *frameBinding) {
  (void)frame;
  if (!IsLoaded() || !m_programs[1] || !m_programs[2] || !cmd || !state ||
      !frustum || !frameBinding || state->width == 0 || state->height == 0)
    return false;
  if (state->aoTexture[image].isEmpty() ||
      state->aoPreparedDepthTexture[image].isEmpty() ||
      state->aoQuarterTexture[image].isEmpty() ||
      state->positionTexture[image].isEmpty() ||
      state->normalTexture[image].isEmpty() ||
      state->aoPreparedDepthStorageView[image].isEmpty() ||
      state->aoQuarterStorageView[image].isEmpty() ||
      state->aoStorageView[image].isEmpty() ||
      state->positionView[image].isEmpty() ||
      state->normalView[image].isEmpty()) {
    return false;
  }
  Log("Standard AO verification: resources ready\n");

  // Must match AOConstants in amnesia/slang/Standard/StandardAmbientOcclusion.slang.
  struct AOConstants {
    float tanHalfFOVX, tanHalfFOVY;
    float ndcToViewMulX, ndcToViewMulY;
    float ndcToViewAddX, ndcToViewAddY;
    float zNear, zFar;
    uint32_t viewportWidth, viewportHeight;
    uint32_t quarterWidth, quarterHeight;
    float viewportTexelX, viewportTexelY;
  };

  const cMatrixf &projMat = frustum->GetProjectionMatrix();
  float tanHalfFOVY = 1.0f / projMat.m[1][1];
  float tanHalfFOVX = tanHalfFOVY * state->width / state->height;

  AOConstants aoConst;
  aoConst.tanHalfFOVX = tanHalfFOVX;
  aoConst.tanHalfFOVY = tanHalfFOVY;
  aoConst.ndcToViewMulX = 2.0f * tanHalfFOVX;
  aoConst.ndcToViewMulY = -2.0f * tanHalfFOVY;
  aoConst.ndcToViewAddX = -tanHalfFOVX;
  aoConst.ndcToViewAddY = tanHalfFOVY;
  aoConst.zNear = frustum->GetNearPlane();
  aoConst.zFar = frustum->GetFarPlane();
  aoConst.viewportWidth = state->width;
  aoConst.viewportHeight = state->height;
  aoConst.quarterWidth = (state->width + 3) / 4;
  aoConst.quarterHeight = (state->height + 3) / 4;
  aoConst.viewportTexelX = 1.0f / state->width;
  aoConst.viewportTexelY = 1.0f / state->height;

  const uint32_t groupsX = (aoConst.quarterWidth + 15) / 16;
  const uint32_t groupsY = (aoConst.quarterHeight + 15) / 16;

  RIProgram::DescriptorBinding constBinding;
  constBinding.handle = DescriptorBindingID::Create("gAOConstants");
  mpGraphics->UpdateFrameUBO(&constBinding.descriptor, &aoConst,
                             sizeof(aoConst));
  Log("Standard AO verification: constants uploaded\n");

  auto barrier = [cmd](RITexture *texture, uint32_t before, uint32_t after) {
    RITextureBarrier b(texture, before, after);
    cmd->vk_d3d12_resourceBarrier<0, 0, 1>(0, nullptr, 0, nullptr, 1, &b);
  };
  // Storage write -> storage read hazard between two consecutive dispatches on
  // the SAME texture. Vulkan's vkCmdPipelineBarrier carries a global execution
  // dependency, so a barrier naming any resource happens to order the
  // dispatches; D3D12 orders only the resource the barrier names, and a
  // transition out of UNDEFINED is an explicit discard that waits for nothing.
  // Each producer/consumer pair therefore needs its own barrier here.
  auto storageHazard = [cmd](RITexture *texture) {
    RITextureBarrier b(texture, RI_RESOURCE_STATE_GENERAL,
                       RI_RESOURCE_STATE_GENERAL, RI_STAGE_COMPUTE,
                       RI_STAGE_COMPUTE);
    cmd->vk_d3d12_resourceBarrier<0, 0, 1>(0, nullptr, 0, nullptr, 1, &b);
  };

  // positionTexture and normalTexture arrive in SHADER_RESOURCE from the
  // G-buffer/reconstruct step and the light pass reads them next, so they are
  // bound as-is: re-barriering from UNDEFINED would let the driver discard them.
  // The two quarter arrays stay in GENERAL across all three dispatches and are
  // read there as storage images; their contents are rebuilt every frame.
  std::vector<RIProgram::DescriptorBinding> bindings;

  {
    Log("Standard AO verification: entering prepare depths\n");
    RIGpuScope _gs(&mpGraphics->profiler, cmd, "StandardAO.prepareDepths");
    Log("Standard AO verification: prepare profiler scope opened\n");
    barrier(state->aoPreparedDepthTexture[image].Get(),
            RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_GENERAL);
    Log("Standard AO verification: prepare barrier recorded\n");

    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_programs[0]->bindComputePipeline(&mpGraphics->device, cmd, kHash,
                                       "Standard.aoPrepareDepths.cs:csMain");
    Log("Standard AO verification: prepare pipeline bound\n");
    m_programs[0]->bindBindlessDescriptorSet(
        cmd, &mpGraphics->globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    Log("Standard AO verification: prepare bindless set bound\n");

    bindings.clear();
    bindings.push_back(*frameBinding);
    Log("Standard AO verification: prepare frame binding assembled\n");
    bindings.push_back(RIProgram::DescriptorBinding(
        "positionTexture",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   state->positionView[image].Get(),
                                   RI_RESOURCE_STATE_SHADER_RESOURCE)));
    Log("Standard AO verification: prepare position binding assembled\n");
    bindings.push_back(RIProgram::DescriptorBinding(
        "aoPreparedDepthTexture",
        RIDescriptor::storageImage(
            &mpGraphics->device,
            state->aoPreparedDepthStorageView[image].Get())));
    Log("Standard AO verification: prepare storage binding assembled\n");
    m_programs[0]->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                                   bindings.data(), bindings.size(),
                                   VK_PIPELINE_BIND_POINT_COMPUTE);
    Log("Standard AO verification: prepare descriptors bound\n");

    cmd->dispatch(&mpGraphics->device, groupsX, groupsY, 1);
    Log("Standard AO verification: prepare dispatch recorded\n");
  }
  Log("Standard AO verification: prepare depths recorded\n");

  {
    Log("Standard AO verification: entering coarse pass\n");
    RIGpuScope _gs(&mpGraphics->profiler, cmd, "StandardAO.coarse");
    barrier(state->aoQuarterTexture[image].Get(), RI_RESOURCE_STATE_UNDEFINED,
            RI_RESOURCE_STATE_GENERAL);
    // prepareDepths wrote aoPreparedDepthTexture; this dispatch reads it.
    storageHazard(state->aoPreparedDepthTexture[image].Get());

    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_programs[1]->bindComputePipeline(&mpGraphics->device, cmd, kHash,
                                       "Standard.aoCoarse.cs:csMain");
    m_programs[1]->bindBindlessDescriptorSet(
        cmd, &mpGraphics->globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    bindings.clear();
    bindings.push_back(constBinding);
    bindings.push_back(RIProgram::DescriptorBinding(
        "aoPreparedDepthTexture",
        RIDescriptor::storageImage(
            &mpGraphics->device,
            state->aoPreparedDepthStorageView[image].Get())));
    bindings.push_back(RIProgram::DescriptorBinding(
        "normalTexture",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   state->normalView[image].Get(),
                                   RI_RESOURCE_STATE_SHADER_RESOURCE)));
    bindings.push_back(RIProgram::DescriptorBinding(
        "aoQuarterTexture",
        RIDescriptor::storageImage(&mpGraphics->device,
                                   state->aoQuarterStorageView[image].Get())));
    m_programs[1]->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                                   bindings.data(), bindings.size(),
                                   VK_PIPELINE_BIND_POINT_COMPUTE);

    cmd->dispatch(&mpGraphics->device, groupsX, groupsY, 16);
  }
  Log("Standard AO verification: coarse pass recorded\n");

  {
    Log("Standard AO verification: entering reinterleave\n");
    RIGpuScope _gs(&mpGraphics->profiler, cmd, "StandardAO.reinterleave");

    // The 16 slices cover every in-bounds full-resolution pixel exactly once;
    // out-of-bounds threads return without corresponding output texels. No
    // pre-clear is needed, which also avoids requiring a standalone UAV-clear
    // descriptor on D3D12.
    barrier(state->aoTexture[image].Get(), RI_RESOURCE_STATE_UNDEFINED,
            RI_RESOURCE_STATE_GENERAL);
    // The coarse pass wrote aoQuarterTexture; this dispatch reads it.
    // aoPreparedDepthTexture is read by both dispatches and written by neither,
    // so the barrier before the coarse pass still covers it.
    storageHazard(state->aoQuarterTexture[image].Get());

    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_programs[2]->bindComputePipeline(&mpGraphics->device, cmd, kHash,
                                       "Standard.aoReinterleave.cs:csMain");
    m_programs[2]->bindBindlessDescriptorSet(
        cmd, &mpGraphics->globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    bindings.clear();
    bindings.push_back(*frameBinding);
    // The resolve needs the depth guide as well as the AO itself.
    bindings.push_back(RIProgram::DescriptorBinding(
        "aoPreparedDepthTexture",
        RIDescriptor::storageImage(
            &mpGraphics->device,
            state->aoPreparedDepthStorageView[image].Get())));
    bindings.push_back(RIProgram::DescriptorBinding(
        "aoQuarterTexture",
        RIDescriptor::storageImage(&mpGraphics->device,
                                   state->aoQuarterStorageView[image].Get())));
    bindings.push_back(RIProgram::DescriptorBinding(
        "aoTexture",
        RIDescriptor::storageImage(&mpGraphics->device,
                                   state->aoStorageView[image].Get())));
    m_programs[2]->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                                   bindings.data(), bindings.size(),
                                   VK_PIPELINE_BIND_POINT_COMPUTE);

    // One thread per quarter-resolution texel and slice.
    cmd->dispatch(&mpGraphics->device, groupsX, groupsY, 16);

    barrier(state->aoTexture[image].Get(), RI_RESOURCE_STATE_GENERAL,
            RI_RESOURCE_STATE_SHADER_RESOURCE);
  }
  Log("Standard AO verification: reinterleave recorded\n");

  return true;
}

} // namespace hpl
