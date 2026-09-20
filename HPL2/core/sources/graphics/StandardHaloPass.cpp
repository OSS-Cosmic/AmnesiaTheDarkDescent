#include "graphics/StandardHaloPass.h"

#include "graphics/DecalPipelineDesc.h"
#include "graphics/GlobalManagedSets.h"
#include "graphics/MeshCreator.h"
#include "graphics/RIVK.h"
#include "graphics/StandardMeshDecalStreams.h"
#include "graphics/VertexBuffer.h"
#include "math/Math.h"
#include "scene/BillBoard.h"
#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

#include <algorithm>

namespace hpl {

StandardHaloQueryState::~StandardHaloQueryState() {
  if (!graphics)
    return;
  for (Slot &slot : slots) {
    if (slot.pool == VK_NULL_HANDLE)
      continue;
    graphics->graphicsDefer.push(
        std::function<void()>([pool = slot.pool, device = &graphics->device]() {
          vkDestroyQueryPool(device->vk.device, pool, nullptr);
        }));
  }
}

float StandardHaloVisibility(uint64_t visible, uint64_t maximum, bool precise) {
  if (maximum == 0)
    return 0.0f;
  if (!precise)
    return visible > 0 ? 1.0f : 0.0f;
  return static_cast<float>(std::min(1.0, static_cast<double>(visible) /
                                              static_cast<double>(maximum)));
}

void ResolveStandardHaloQueries(StandardHaloQueryState &state,
                                uint64_t completedTimeline,
                                const StandardHaloQueryReader &readCounts) {
  std::vector<uint64_t> counts;
  for (StandardHaloQueryState::Slot &slot : state.slots) {
    if (slot.resolved || slot.timelineValue > completedTimeline)
      continue;
    if (slot.cookies.empty()) {
      slot.resolved = true;
      continue;
    }
    counts.assign(slot.cookies.size() * 2, 0);
    // Not ready: keep the slot pending and read it on a later Draw.
    if (readCounts(slot.pool, static_cast<uint32_t>(counts.size()),
                   counts.data()) != VK_SUCCESS)
      continue;
    slot.resolved = true;
    if (slot.timelineValue < state.latestTimeline)
      continue;
    // Replace wholesale so billboards that stopped being queried drop out.
    state.latestVisibility.clear();
    for (size_t q = 0; q < slot.cookies.size(); ++q)
      state.latestVisibility[slot.cookies[q]] = StandardHaloVisibility(
          counts[q * 2], counts[q * 2 + 1], slot.precise);
    state.latestTimeline = slot.timelineValue;
  }
}

//-----------------------------------------------------------------------

cStandardHaloPass::cStandardHaloPass(cGraphics *graphics)
    : mpGraphics(graphics) {}

cStandardHaloPass::~cStandardHaloPass() { DestroyData(); }

void cStandardHaloPass::DestroyData() {
  if (m_box && mpGraphics)
    mpGraphics->graphicsDefer.push(
        std::function<void()>([box = std::move(m_box)]() {}));
  m_box.reset();
}

std::vector<cBillboard *>
cStandardHaloPass::CollectHalos(std::span<iRenderable *> translucents) const {
  std::vector<cBillboard *> halos;
  for (iRenderable *object : translucents) {
    if (halos.size() >= StandardHaloQueryState::kMaxHalos)
      break;
    if (!object || object->GetRenderType() != eRenderableType_Billboard ||
        !object->GetMaterial())
      continue;
    cBillboard *billboard = static_cast<cBillboard *>(object);
    if (billboard->IsHalo())
      halos.push_back(billboard);
  }
  return halos;
}

void cStandardHaloPass::Resolve(StandardHaloQueryState &state,
                                std::span<iRenderable *> translucents,
                                cFrustum *frustum) {
  // The halo query implementation below is Vulkan-native. D3D12 needs its
  // own query heap/readback path; never dereference the inactive Vulkan
  // device while that backend is selected.
  if (!RIIsTargetSelected(RI_DEVICE_API_VK) || !mpGraphics || !frustum)
    return;
  const VkDevice vkDevice = mpGraphics->device.vk.device;
  ResolveStandardHaloQueries(
      state, mpGraphics->graphicsTimeline.completed(&mpGraphics->device),
      [vkDevice](VkQueryPool pool, uint32_t count, uint64_t *counts) {
        return vkGetQueryPoolResults(vkDevice, pool, 0, count,
                                     count * sizeof(uint64_t), counts,
                                     sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
      });
  // Halos without a result keep their alpha until one lands, as in legacy.
  for (cBillboard *billboard : CollectHalos(translucents)) {
    const auto result =
        state.latestVisibility.find(billboard->GetUniqueCookie());
    if (result == state.latestVisibility.end())
      continue;
    billboard->SetHaloAlpha(billboard->GetHaloScreenCoverage(frustum) *
                            result->second);
  }
}

void cStandardHaloPass::Record(cGraphics::FrameContext *frame,
                               StandardHaloQueryState &state,
                               std::span<iRenderable *> translucents,
                               RIProgram *meshDecal, RITexture *depthTexture,
                               RITextureView *depthView, uint32_t width,
                               uint32_t height,
                               RIProgram::DescriptorBinding frameBinding,
                               uint32_t paneSalt) {
  if (!RIIsTargetSelected(RI_DEVICE_API_VK) || !mpGraphics || !depthTexture ||
      !depthView || width == 0 || height == 0)
    return;
  // One recording per frame: a second render of the same viewport state must
  // not reset queries the first render is still waiting on.
  const uint64_t frameIndex = mpGraphics->frameIndex;
  if (state.lastRecordedFrame == frameIndex)
    return;

  const std::vector<cBillboard *> halos = CollectHalos(translucents);
  if (halos.empty())
    return;
  if (!meshDecal) {
    if (!m_warnedUnavailable) {
      Warning("Standard renderer: Decal.vert/frag missing; billboard halo "
              "occlusion "
              "unavailable, halos stay hidden\n");
      m_warnedUnavailable = true;
    }
    return;
  }

  StandardHaloQueryState::Slot &slot =
      state.slots[frameIndex % RI_NUMBER_FRAMES_FLIGHT];
  // That slot's frame has not been read back yet; keep its counts.
  if (!slot.resolved)
    return;

  const VkDevice vkDevice = mpGraphics->device.vk.device;
  if (slot.pool == VK_NULL_HANDLE) {
    VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_OCCLUSION;
    info.queryCount = StandardHaloQueryState::kMaxHalos * 2;
    if (vkCreateQueryPool(vkDevice, &info, nullptr, &slot.pool) != VK_SUCCESS) {
      slot.pool = VK_NULL_HANDLE;
      if (!m_warnedUnavailable) {
        Warning("Standard renderer: occlusion query pool creation failed; "
                "halos stay hidden\n");
        m_warnedUnavailable = true;
      }
      return;
    }
  }
  if (!m_box)
    if (cMeshCreator *meshCreator = mpGraphics->GetMeshCreator())
      m_box.reset(
          meshCreator->CreateBoxVertexBuffer(cVector3f(1.0f, 1.0f, 1.0f)));
  if (!m_box)
    return;

  m_box->SubmitToGPU(&mpGraphics->device);
  std::vector<uint64_t> cookies;
  std::vector<uint32_t> objectSlots;
  cookies.reserve(halos.size());
  objectSlots.reserve(halos.size());
  for (cBillboard *billboard : halos) {
    const uint32_t materialId =
        mpGraphics->globalset
            ->submitMaterial(frame, billboard->GetMaterial(),
                             static_cast<uint32_t>(frameIndex))
            .materialId;
    if (materialId == UINT32_MAX)
      continue;
    ObjectSubmitDesc object;
    const cMatrixf haloModel =
        cMath::MatrixMul(billboard->GetWorldMatrix(),
                         cMath::MatrixScale(billboard->GetHaloSourceSize()));
    object.modelMatrix =
        &haloModel; // submitObject copies the matrix immediately
    object.uvMatrix = cMatrixf::Identity;
    object.materialId = materialId;
    // Distinct from the particle pass's cookie for the same billboard.
    const hash_t cookie =
        hash_u32(hash_u64(HASH_INITIAL_VALUE,
                          billboard->GetUniqueCookie() ^ 0x48414c4f5155u),
                 paneSalt);
    const uint32_t objectSlot = mpGraphics->globalset->submitObject(
        cookie, static_cast<uint32_t>(frameIndex), nullptr, object,
        kSubmitData);
    if (objectSlot == UINT32_MAX)
      continue;
    cookies.push_back(billboard->GetUniqueCookie());
    objectSlots.push_back(objectSlot);
  }
  if (cookies.empty())
    return;
  mpGraphics->globalset->flushMirrors(&mpGraphics->device);

  RICmd *cmd = &mpGraphics->primary.cmds[0];
  const uint32_t queryCount = static_cast<uint32_t>(cookies.size() * 2);
  vkCmdResetQueryPool(cmd->vk.cmd, slot.pool, 0, queryCount);
  cmd->vk_d3d12_textureBarrier(
      RITextureBarrier(depthTexture, RI_RESOURCE_STATE_SHADER_RESOURCE,
                       RI_RESOURCE_STATE_DEPTH_READ, RI_STAGE_FRAGMENT,
                       RI_STAGE_NONE, RI_BARRIER_ASPECT_DEPTH));
  RIRenderingAttachment depth = {};
  depth.view = *depthView;
  depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
  depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depth.readOnly = true;
  RIBeginRenderingDesc begin = {};
  begin.renderArea.width = static_cast<int16_t>(width);
  begin.renderArea.height = static_cast<int16_t>(height);
  begin.colorCount = 0;
  begin.colors = nullptr;
  begin.depthStencil = &depth;
  cmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);
  RIViewport vp;
  vp.x = 0;
  vp.y = float(height);
  vp.width = float(width);
  vp.height = -float(height);
  vp.depthMin = 0;
  vp.depthMax = 1;
  RIRect sc;
  sc.x = 0;
  sc.y = 0;
  sc.width = int16_t(width);
  sc.height = int16_t(height);
  cmd->setViewport(&mpGraphics->device, vp);
  cmd->setScissor(&mpGraphics->device, sc);

  uint32_t presentMask = 0;
  if (BindMeshDecalStreams(cmd, mpGraphics, m_box.get(), &presentMask)) {
    meshDecal->bindBindlessDescriptorSet(
        cmd, &mpGraphics->globalset->m_bindlessSet, 0);
    meshDecal->bindDescriptors(&mpGraphics->device, cmd, mpGraphics->frameIndex,
                               &frameBinding, 1);

    // Depth-only variants of the decal pipeline: no colour output, no depth
    // write; LEQUAL counts the texels that pass the scene depth, ALWAYS every
    // texel the box covers.
    RIGraphicsPipelineDesc pipelines[2] = {
        MakeDecalPipelineDesc(cGraphics::PogoColorFormat, cGraphics::DepthFormat,
                              DecalPipelineDesc::BLEND_ADD, presentMask),
        MakeDecalPipelineDesc(cGraphics::PogoColorFormat, cGraphics::DepthFormat,
                              DecalPipelineDesc::BLEND_ADD, presentMask)};
    for (uint32_t variant = 0; variant < 2; ++variant) {
      RIGraphicsPipelineDesc &pd = pipelines[variant];
      // Depth-only: blendCount must track colorCount, both drop to zero.
      pd.renderTarget.colorCount = 0;
      pd.blendCount = 0;
      pd.depthStencil.depthCompare =
          variant ? RI_COMPARE_ALWAYS : RI_COMPARE_LESS_EQUAL;
    }
    // The two variants differ only by depthCompare, which the structural hash
    // already covers; the discriminator is kept for readability.
    const hash_t pipelineHashes[2] = {hash_u32(HASH_INITIAL_VALUE, 0u),
                                      hash_u32(HASH_INITIAL_VALUE, 1u)};
    const char *pipelineNames[2] = {"Standard.haloVisible", "Standard.haloMax"};

    // Precise queries count samples; without them any passing sample reports
    // non-zero, which still tells hidden from visible.
    const bool precise = mpGraphics->device.occlusionQueryPreciseEnabled;
    const VkQueryControlFlags queryControl =
        precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    const uint32_t indexCount = static_cast<uint32_t>(m_box->GetIndexNum());
    for (size_t h = 0; h < cookies.size(); ++h) {
      for (uint32_t variant = 0; variant < 2; ++variant) {
        meshDecal->bindPipeline(&mpGraphics->device, cmd,
                                pipelineHashes[variant], pipelineNames[variant],
                                pipelines[variant]);
        const uint32_t queryIndex = static_cast<uint32_t>(h * 2 + variant);
        vkCmdBeginQuery(cmd->vk.cmd, slot.pool, queryIndex, queryControl);
        cmd->drawIndexed(&mpGraphics->device, indexCount, 1u, 0u, 0,
                         objectSlots[h]);
        vkCmdEndQuery(cmd->vk.cmd, slot.pool, queryIndex);
      }
    }
    slot.cookies = std::move(cookies);
    slot.precise = precise;
    slot.timelineValue = mpGraphics->graphicsTimeline.pending() + 1;
    slot.resolved = false;
    state.lastRecordedFrame = frameIndex;
  }
  cmd->vk_d3d12_endRendering(&mpGraphics->device);
  cmd->vk_d3d12_textureBarrier(
      RITextureBarrier(depthTexture, RI_RESOURCE_STATE_DEPTH_READ,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                       RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH));
}

} // namespace hpl
