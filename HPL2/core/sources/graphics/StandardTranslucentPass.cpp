#include "graphics/StandardTranslucentPass.h"

#include "graphics/GlobalManagedSets.h"
#include "graphics/Material.h"
#include "graphics/RIVK.h"
#include "graphics/StandardLightData.h"
#include "graphics/VertexBuffer.h"
#include "math/Frustum.h"
#include "graphics/Renderable.h"
#include "graphics/Renderer.h"
#include "scene/World.h"
#include "scene/Light.h"
#include "math/BoundingVolume.h"
#include "math/Math.h"
#include "resources/Resources.h"

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace hpl {
namespace {

static TranslucentMeshPipelineDesc::BlendMode
remapBlend(eMaterialBlendMode mode) {
  switch (mode) {
  case eMaterialBlendMode_Mul:
    return TranslucentMeshPipelineDesc::BLEND_MUL;
  case eMaterialBlendMode_MulX2:
    return TranslucentMeshPipelineDesc::BLEND_MULX2;
  case eMaterialBlendMode_Alpha:
    return TranslucentMeshPipelineDesc::BLEND_ALPHA;
  case eMaterialBlendMode_PremulAlpha:
    return TranslucentMeshPipelineDesc::BLEND_PREMUL_ALPHA;
  case eMaterialBlendMode_Add:
  default:
    return TranslucentMeshPipelineDesc::BLEND_ADD;
  }
}

// Every translucent indirect command occupies a VkDrawIndexedIndirectCommand
// sized slot, indexed or not, so one uniform stride covers both layouts. Each
// draw is its own drawCount-1 indirect call, so Vulkan reads only the offset.
static constexpr uint64_t kStandardTranslucentCommandStride =
    sizeof(VkDrawIndexedIndirectCommand);

static bool bindVertexStreams(cGraphics *graphics, RICmd *cmd,
                              cVertexBuffer *vb, uint32_t *mask,
                              bool *indexed) {
  auto buffer = [vb](eVertexBufferElement element) -> RIBuffer * {
    const auto *data = vb->GetElement(element);
    return data ? data->GetBuffer() : nullptr;
  };
  RIBuffer *position = buffer(eVertexBufferElement_Position);
  RIBuffer *index = vb->GetIndexRIBuffer();
  if (!position)
    return false;
  RIBuffer *normal = buffer(eVertexBufferElement_Normal);
  RIBuffer *tangent = buffer(eVertexBufferElement_Texture1Tangent);
  RIBuffer *color = buffer(eVertexBufferElement_Color0);
  RIBuffer *uv = buffer(eVertexBufferElement_Texture0);
  uint32_t present = eVertexElementFlag_Position;
  if (normal)
    present |= eVertexElementFlag_Normal;
  if (tangent)
    present |= eVertexElementFlag_Texture1;
  if (color)
    present |= eVertexElementFlag_Color0;
  if (uv)
    present |= eVertexElementFlag_Texture0;
  RIBuffer *streams[5] = {position,
                          normal ? normal : &graphics->fallbackNormalVertex,
                          tangent ? tangent : &graphics->fallbackTangentVertex,
                          color ? color : &graphics->fallbackColorVertex,
                          uv ? uv : &graphics->fallbackUv0Vertex};
  cmd->bindVertexBuffers<5>(0, 5, streams);
  if (index) {
    cmd->bindIndexBuffer(&graphics->device, index, 0, RI_INDEX_TYPE_32);
    if (indexed)
      *indexed = true;
  } else if (indexed) {
    *indexed = false;
  }
  if (mask)
    *mask = present;
  return true;
}

static void transition(cGraphics *graphics, RITexture *texture, uint32_t before,
                       uint32_t after, RIStageBits_e srcStage,
                       RIStageBits_e dstStage, RIBarrierAspect_e aspect) {
  graphics->primary.cmds[0].vk_d3d12_textureBarrier(
      RITextureBarrier(texture, before, after, srcStage, dstStage, aspect));
}

} // namespace

cStandardTranslucentPass::cStandardTranslucentPass(cGraphics *graphics,
                                                   cResources *resources)
    : mpGraphics(graphics), mpResources(resources) {}

cStandardTranslucentPass::~cStandardTranslucentPass() { DestroyData(); }

bool cStandardTranslucentPass::LoadData() {
  if (m_loaded)
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;
  auto vertBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                            "Standard.translucent.3d", "vsMain");
  auto fragBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                            "Standard.translucent.3d", "psMain");
  if (vertBin.empty() || fragBin.empty())
    return false;
  auto program = std::make_shared<RIProgram>();
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vertBin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fragBin,
                             "psMain"}};
  const RIBindlessLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.layout()};
  program->initialize(&mpGraphics->device, stages, external,
                      "Standard.translucent");
  auto old = std::move(m_program);
  m_program = std::move(program);
  if (old)
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [old = std::move(old), device = &mpGraphics->device]() mutable {
          old->dispose(device);
        }));
  m_loaded = true;
  return true;
}

void cStandardTranslucentPass::DestroyData() {
  auto old = std::move(m_program);
  if (old)
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [old = std::move(old), device = &mpGraphics->device]() mutable {
          old->dispose(device);
        }));
  m_loaded = false;
}

// Legacy RendererDeferred per-object light level for AffectedByLightLevel
// materials: at the bounding-volume centre, box lights add their peak colour
// and point/spot lights add peak colour x linear radial falloff, capped at 1.
float StandardTranslucentLightLevel(cWorld *world, iRenderable *object) {
  if (!world || !object || !object->GetBoundingVolume())
    return 0.0f;
  const cVector3f centre = object->GetBoundingVolume()->GetWorldCenter();
  float level = 0.0f;
  for (iLight *light : *world->GetLightList()) {
    if (!light || light->GetLightType() == eLightType_Area ||
        !light->GetVisibleVar() || !light->IsLegacyRendererEnabled() ||
        !light->CheckObjectIntersection(object))
      continue;
    const cColor color = light->GetDiffuseColor();
    const float peak = std::max(std::max(color.r, color.g), color.b);
    if (light->GetLightType() == eLightType_Box) {
      level += peak;
    } else {
      const float radius = light->GetRadius();
      if (!(radius > 0.0f))
        continue;
      const float distance =
          cMath::Vector3Dist(light->GetWorldPosition(), centre);
      level += peak * std::max(1.0f - distance / radius, 0.0f);
    }
    if (level >= 1.0f)
      return 1.0f;
  }
  return std::max(level, 0.0f);
}

bool cStandardTranslucentPass::Draw(
    cGraphics::FrameContext *frame, cViewport::StandardViewportState *state,
    uint32_t imageIndex, std::span<iRenderable *> renderables,
    cFrustum *frustum, cWorld *world,
    RIProgram::DescriptorBinding *frameBinding,
    RIProgram::DescriptorBinding *fogBinding, RITextureView *standardShadowView,
    RISharedPointer<RIBuffer> *pointLights,
    RISharedPointer<RIBuffer> *spotLights, uint32_t pointLightCount,
    uint32_t spotLightCount, const OcclusionCull *cullInput) {
  if (!state || imageIndex >= RI_MAX_SWAPCHAIN_IMAGES ||
      state->renderTarget[imageIndex].isEmpty() ||
      state->renderTargetAttachmentView[imageIndex].isEmpty() ||
      state->depthTextures[imageIndex].isEmpty() ||
      state->depthView[imageIndex].isEmpty() ||
      state->depthSampleView[imageIndex].isEmpty() ||
      state->translucentSceneCopy[imageIndex].isEmpty() ||
      state->translucentSceneCopyView[imageIndex].isEmpty())
    return false;
  Targets targets;
  targets.color = state->renderTarget[imageIndex].Get();
  targets.colorAttachmentView =
      state->renderTargetAttachmentView[imageIndex].Get();
  targets.depth = state->depthTextures[imageIndex].Get();
  targets.depthAttachmentView = state->depthView[imageIndex].Get();
  targets.depthSampleView = state->depthSampleView[imageIndex].Get();
  targets.sceneCopy = state->translucentSceneCopy[imageIndex].Get();
  targets.sceneCopyView = state->translucentSceneCopyView[imageIndex].Get();
  targets.sceneCopyInitialized =
      &state->translucentSceneCopyInitialized[imageIndex];
  targets.width = state->width;
  targets.height = state->height;
  targets.slotSalt =
      hash_u64(HASH_INITIAL_VALUE, reinterpret_cast<uintptr_t>(state));
  return Draw(frame, targets, renderables, frustum, world, frameBinding,
              fogBinding, standardShadowView, pointLights, spotLights,
              pointLightCount, spotLightCount, cullInput);
}

bool cStandardTranslucentPass::Draw(
    cGraphics::FrameContext *frame, const Targets &targets,
    std::span<iRenderable *> renderables, cFrustum *frustum, cWorld *world,
    RIProgram::DescriptorBinding *frameBinding,
    RIProgram::DescriptorBinding *fogBinding, RITextureView *standardShadowView,
    RISharedPointer<RIBuffer> *pointLights,
    RISharedPointer<RIBuffer> *spotLights, uint32_t pointLightCount,
    uint32_t spotLightCount, const OcclusionCull *cullInput) {
  if (!m_loaded || !m_program || !frame || !frustum || !world ||
      !frameBinding || !standardShadowView || standardShadowView->isEmpty() ||
      targets.width == 0 || targets.height == 0 || targets.width > 32767 ||
      targets.height > 32767 || pointLights == nullptr ||
      spotLights == nullptr || pointLights->isEmpty() ||
      spotLights->isEmpty() || !targets.color ||
      !targets.colorAttachmentView || !targets.depth ||
      !targets.depthAttachmentView || !targets.depthSampleView ||
      !targets.sceneCopyView || targets.slotSalt == 0)
    return false;

  // The renderer supplies the already sorted m_rendererList.GetRenderableItems(eRenderListType_Translucent)
  // span; this pass intentionally does not rebuild or resort that list.
  // The filter keeps required GetVertexBuffer() and GetMaterial() checks together.
  std::vector<iRenderable *> meshes;
  for (iRenderable *object : renderables) {
    // Standard owns ordinary sub-meshes only. Other translucent producers have
    // dedicated passes (or deliberately remain unsupported here).
    if (!object || object->GetRenderType() != eRenderableType_SubMesh ||
        !object->GetVertexBuffer() ||
        (object->GetVertexBuffer()->GetIndexNum() <= 0 &&
         object->GetVertexBuffer()->GetVertexNum() <= 0))
      continue;
    cMaterial *material = object->GetMaterial();
    if (!material || material->GetMaterialID() != MaterialID::Translucent ||
        material->GetBlendMode() == eMaterialBlendMode_None ||
        material->GetBlendMode() >= eMaterialBlendMode_LastEnum)
      continue;
    meshes.push_back(object);
  }
  if (meshes.empty())
    return true;

  RICmd *cmd = &mpGraphics->primary.cmds[0];
  RITexture *target = targets.color;
  RITexture *copy = targets.sceneCopy;
  RITextureView *targetView = targets.colorAttachmentView;
  RITextureView *copyView = targets.sceneCopyView;

  // Resolve every descriptor and sampler before recording any transition. A
  // failed pass must leave both attachments in the caller's readable state.
  if (frameBinding->descriptor.isEmpty() || !fogBinding ||
      fogBinding->descriptor.isEmpty())
    return false;
  auto sceneColorSampler = mpGraphics->resolve_filter_descriptor(
      eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
      eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
  auto ramp = mpGraphics->resolve_filter_descriptor(
      eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
      eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
  auto gobo = mpGraphics->resolve_filter_descriptor(
      eTextureWrap_ClampToBorder, eTextureWrap_ClampToBorder,
      eTextureWrap_ClampToBorder, eTextureFilter_Trilinear);
  if (!sceneColorSampler || !ramp || !gobo)
    return false;

  // Fixed-function vertex input reads the uploaded VB buffers. Upload before
  // submitObject publishes handles and before any rendering scope binds them.
  for (iRenderable *object : meshes) {
    auto *vb = static_cast<cVertexBuffer *>(object->GetVertexBuffer());
    if (vb)
      vb->SubmitToGPU(&mpGraphics->device);
  }

  struct DrawItem {
    iRenderable *object;
    cMaterial *material;
    uint32_t materialId;
    uint32_t slot;
    bool refractive;
    float lightLevel;
    // Filled by the cull pre-pass below. kNoCommand means "draw directly",
    // which is what every item gets when there is no cull this frame.
    uint32_t commandSlot = UINT32_MAX;
    bool indexed = false;
  };
  static constexpr uint32_t kNoCommand = UINT32_MAX;
  std::vector<DrawItem> items;
  items.reserve(meshes.size());
  const hash_t paneSalt = targets.slotSalt;
  for (iRenderable *object : meshes) {
    // Legacy RendererDeferred skips a translucent whose viewport update fails.
    if (!object->UpdateGraphicsForViewport(frustum, 0.0f))
      continue;
    cMaterial *material = object->GetMaterial();
    const uint32_t materialId =
        mpGraphics->globalset
            ->submitMaterial(frame, material,
                             static_cast<uint32_t>(mpGraphics->frameIndex))
            .materialId;
    if (materialId == UINT32_MAX)
      continue;
    ObjectSubmitDesc objectData;
    objectData.modelMatrix = object->GetModelMatrix(frustum);
    objectData.uvMatrix = material->GetUvMatrix();
    objectData.materialId = materialId;
    objectData.dissolveAmount = object->GetCoverageAmount();
    objectData.illuminationAmount = object->GetIlluminationAmount();
    objectData.renderFlags = object->GetRenderFlags();
    const hash_t cookie = hash_u32(
        hash_u64(HASH_INITIAL_VALUE, object->GetUniqueCookie()), paneSalt);
    const uint32_t slot = mpGraphics->globalset->submitObject(
        cookie, static_cast<uint32_t>(mpGraphics->frameIndex),
        static_cast<cVertexBuffer *>(object->GetVertexBuffer()), objectData);
    if (slot == UINT32_MAX)
      continue;
    const MaterialTranslucent *translucentData =
        std::get_if<MaterialTranslucent>(&material->Data());
    const float lightLevel =
        translucentData && translucentData->m_isAffectedByLightLevel
            ? StandardTranslucentLightLevel(world, object)
            : 1.0f;
    // Honour the global refraction setting, as the legacy renderer did.
    const bool refractive = copy != nullptr &&
                            iRenderer::GetRefractionEnabled() &&
                            material->HasRefraction();
    items.push_back(
        {object, material, materialId, slot, refractive, lightLevel});
  }
  if (items.empty())
    return true;
  // submitObject writes staged global records; publish them before any draw
  // scope begins so the translucent pass never submits descriptors mid-render.
  mpGraphics->globalset->flushMirrors(&mpGraphics->device);

  // ---------------------------------------------------------------------
  // GPU occlusion cull.
  //
  // One 5-word command slot per draw this pass is about to record, written
  // here with instanceCount = 1; the kernel rewrites that one word to 0 for
  // anything it can prove is hidden behind the depth pyramid. The slots are
  // visited below in the caller's back-to-front order and every per-item bind
  // still happens on the CPU, so nothing about ordering or per-item state
  // changes -- a culled draw simply rasterizes nothing.
  //
  // All or nothing: if the reservation cannot hold every draw, the whole pass
  // falls back to direct draws rather than culling part of the list.
  // ---------------------------------------------------------------------
  uint32_t commandCount = 0;
  bool cullDispatched = false;
  const OcclusionCull emptyCull{};
  const OcclusionCull &cull = cullInput ? *cullInput : emptyCull;
  if (cull.IsUsable()) {
    bool fits = true;
    for (DrawItem &item : items) {
      auto *vb = static_cast<cVertexBuffer *>(item.object->GetVertexBuffer());
      // Exactly what bindVertexStreams decides below, from the same source.
      item.indexed = vb->GetIndexRIBuffer() != nullptr;
      const uint32_t elementCount =
          item.indexed ? static_cast<uint32_t>(vb->GetIndexNum())
                       : static_cast<uint32_t>(vb->GetVertexNum());
      // The cube-map reflection variant is a second draw of the same mesh.
      const uint32_t draws =
          (!item.refractive && item.material->GetImage(eMaterialTexture_CubeMap))
              ? 2u
              : 1u;
      if (commandCount + draws > cull.capacity) {
        fits = false;
        break;
      }
      cBoundingVolume *bounds = item.object->GetBoundingVolume();
      if (!bounds) {
        fits = false;
        break;
      }
      const cVector3f boundsMin = bounds->GetMin();
      const cVector3f boundsMax = bounds->GetMax();

      item.commandSlot = commandCount;
      for (uint32_t draw = 0; draw < draws; ++draw) {
        const uint32_t slot = commandCount + draw;
        const size_t wordBase = static_cast<size_t>(cull.commandBase + slot) * 5u;
        uint32_t *words = cull.commandWords + wordBase;
        if (item.indexed) {
          // VkDrawIndexedIndirectCommand
          words[0] = elementCount;
          words[1] = 1u;
          words[2] = 0u;
          words[3] = 0u;
          words[4] = item.slot;
        } else {
          // VkDrawIndirectCommand, in the first four words of the slot.
          words[0] = elementCount;
          words[1] = 1u;
          words[2] = 0u;
          words[3] = item.slot;
          words[4] = 0u;
        }

        // One candidate per DRAW, not per item: a candidate owns exactly one
        // instanceCount word, and the reflection variant has its own.
        StandardCullCandidate &candidate = cull.candidateSlots[slot];
        candidate = StandardCullCandidate{};
        candidate.aabbMinX = boundsMin.x;
        candidate.aabbMinY = boundsMin.y;
        candidate.aabbMinZ = boundsMin.z;
        candidate.aabbMaxX = boundsMax.x;
        candidate.aabbMaxY = boundsMax.y;
        candidate.aabbMaxZ = boundsMax.z;
        candidate.objectSlot = item.slot;
        candidate.vertexCount = elementCount;
        // The shared predicate gates on the shadow-caster bit and the
        // static/dynamic variability mask before it ever reaches the frustum
        // or occlusion test. Neither gate means anything for a camera tile, so
        // the host sets the bits that let every candidate through and the tile
        // (filled by the caller) accepts both variabilities.
        candidate.renderFlags =
            kStandardCullShadowCasterBit |
            (item.object->IsStatic() ? kStandardCullStaticBit : 0u);
        // A refractive draw ends the render scope and copies the whole target
        // whether or not it draws, so culling it saves the cheap half and
        // keeps the expensive half. Leave it visible until that copy can be
        // skipped too.
        candidate.cullFlags =
            item.refractive ? kStandardCullFlagNeverOcclude : 0u;
        candidate.commandWordOffset =
            static_cast<uint32_t>(wordBase) + 1u;  // instanceCount
      }
      commandCount += draws;
    }

    const uint32_t groupCount =
        (commandCount + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;
    if (fits && commandCount > 0 && groupCount <= cull.groupCapacity) {
      // The caller filled the tile's planes, camera and masks; only the
      // candidate range depends on what this pass produced.
      cull.tileSlot->candidateBase = cull.candidateBase;
      cull.tileSlot->candidateCount = commandCount;
      for (uint32_t group = 0; group < groupCount; ++group) {
        cull.groupSlots[group].tileIndex = cull.tileBase;
        cull.groupSlots[group].candidateOffset = group * kStandardCullGroupSize;
      }
      cullDispatched = cull.pass->Dispatch(
          cmd, mpGraphics->frameIndex, cull.buffers, cull.tileBase, 1,
          cull.groupBase, groupCount, kStandardCullModeInstanceMask);
    }
    if (!cullDispatched) {
      for (DrawItem &item : items)
        item.commandSlot = kNoCommand;
    }
  }

  RIProgram::DescriptorBinding sceneBinding(
      "sceneColorInput",
      RIDescriptor::sampledImage(&mpGraphics->device, copyView,
                                 RI_RESOURCE_STATE_SHADER_RESOURCE));
  // The descriptor must declare the state the barrier below actually
  // establishes (DEPTH_READ | SHADER_RESOURCE). DEPTH_READ alone still selects
  // DEPTH_READ_ONLY_OPTIMAL on Vulkan, but on D3D12 a resource is only
  // samplable with the shader-resource bit, so the bare state makes
  // bindDescriptors reject this binding and skip the whole set.
  RIProgram::DescriptorBinding sceneDepthBinding(
      "sceneDepthInput",
      RIDescriptor::sampledImage(
          &mpGraphics->device, targets.depthSampleView,
          static_cast<RIResourceState_e>(RI_RESOURCE_STATE_DEPTH_READ |
                                         RI_RESOURCE_STATE_SHADER_RESOURCE)));
  std::vector<RIProgram::DescriptorBinding> bindings;
  bindings.reserve(11);
  bindings.push_back(*frameBinding);
  if (fogBinding)
    bindings.push_back(*fogBinding);
  bindings.push_back(sceneBinding);
  bindings.push_back(sceneDepthBinding);
  bindings.emplace_back(
      "standardPointLights",
      RIDescriptor::storageBuffer(&mpGraphics->device, pointLights->Get(), 0,
                                  std::max(pointLightCount, 1u) *
                                      sizeof(StandardPointLightData)));
  bindings.emplace_back(
      "standardSpotLights",
      RIDescriptor::storageBuffer(&mpGraphics->device, spotLights->Get(), 0,
                                  std::max(spotLightCount, 1u) *
                                      sizeof(StandardSpotLightData)));
  StandardLightCounts counts{pointLightCount, spotLightCount,
                             0}; // Box lights not used in translucent pass
  RIProgram::DescriptorBinding countBinding("standardLightCounts",
                                            RIDescriptor(), 0, false);
  mpGraphics->UpdateFrameUBO(&countBinding.descriptor, &counts, sizeof(counts));
  bindings.push_back(countBinding);
  bindings.emplace_back("sceneColorSampler", *sceneColorSampler);
  bindings.emplace_back("standardRampSampler", *ramp);
  bindings.emplace_back("standardGoboSampler", *gobo);
  bindings.emplace_back(
      "standardShadowMap",
      RIDescriptor::sampledImage(&mpGraphics->device, standardShadowView));

  // All descriptors are resolved before recording attachment transitions. The
  // copy is sampled by every translucent draw, including the first
  // non-refractive one; refractive draws overwrite it only between scopes.
  for (const auto &binding : bindings)
    if (binding.descriptor.isEmpty())
      return false;
  transition(mpGraphics, target, RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_FRAGMENT,
             RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR);
  transition(mpGraphics, targets.depth, RI_RESOURCE_STATE_SHADER_RESOURCE,
             (RI_RESOURCE_STATE_DEPTH_READ | RI_RESOURCE_STATE_SHADER_RESOURCE),
             RI_STAGE_FRAGMENT, RI_STAGE_ALL_GRAPHICS, RI_BARRIER_ASPECT_DEPTH);
  if (copy && targets.sceneCopyInitialized &&
      !*targets.sceneCopyInitialized) {
    transition(mpGraphics, copy, RI_RESOURCE_STATE_UNDEFINED,
               RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
               RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR);
    *targets.sceneCopyInitialized = true;
  }

  auto begin = [&]() {
    RIRenderingAttachment color = {};
    color.view = *targetView;
    color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
    RIRenderingAttachment depth = {};
    depth.view = *targets.depthAttachmentView;
    depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
    depth.readOnly = true;
    RIBeginRenderingDesc desc = {};
    desc.renderArea.width = static_cast<int16_t>(targets.width);
    desc.renderArea.height = static_cast<int16_t>(targets.height);
    desc.colorCount = 1;
    desc.colors = &color;
    desc.depthStencil = &depth;
    cmd->vk_d3d12_beginRendering(&mpGraphics->device, desc);
    RIViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(targets.height);
    viewport.width = static_cast<float>(targets.width);
    viewport.height = -static_cast<float>(targets.height);
    viewport.depthMin = 0.0f;
    viewport.depthMax = 1.0f;
    RIRect scissor = {};
    if (targets.scissor.width > 0 && targets.scissor.height > 0) {
      scissor = targets.scissor;
    } else {
      scissor.x = 0;
      scissor.y = 0;
      scissor.width = static_cast<int16_t>(targets.width);
      scissor.height = static_cast<int16_t>(targets.height);
    }
    cmd->setViewport(&mpGraphics->device, viewport);
    cmd->setScissor(&mpGraphics->device, scissor);
    m_program->bindBindlessDescriptorSet(
        cmd, &mpGraphics->globalset->m_bindlessSet, 0);
    m_program->bindDescriptors(&mpGraphics->device, cmd, mpGraphics->frameIndex,
                               bindings.data(), bindings.size());
  };
  begin();

  uint32_t recordedDraw = 0;
  for (const DrawItem &item : items) {
    iRenderable *object = item.object;
    cMaterial *material = item.material;
    const bool refractive = item.refractive;
    if (refractive) {
      // The active rendering scope must be closed before the image copy. The
      // copy is taken after all earlier sorted meshes, so overlap sees the
      // exact preceding composite rather than a frame-global stale snapshot.
      cmd->vk_d3d12_endRendering(&mpGraphics->device);
      transition(mpGraphics, target, RI_RESOURCE_STATE_RENDER_TARGET_READ,
                 RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_FRAGMENT, RI_STAGE_COPY,
                 RI_BARRIER_ASPECT_COLOR);
      transition(mpGraphics, copy, RI_RESOURCE_STATE_SHADER_RESOURCE,
                 RI_RESOURCE_STATE_COPY_DST, RI_STAGE_FRAGMENT, RI_STAGE_COPY,
                 RI_BARRIER_ASPECT_COLOR);
      RIImageCopyDesc image = {};
      image.width = targets.width;
      image.height = targets.height;
      image.depth = 1;
      cmd->copyImage(&mpGraphics->device, target, copy, image);
      transition(mpGraphics, copy, RI_RESOURCE_STATE_COPY_DST,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COPY,
                 RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR);
      transition(mpGraphics, target, RI_RESOURCE_STATE_COPY_SRC,
                 RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_COPY,
                 RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR);
      begin();
    }

    recordedDraw = 0;
    uint32_t vertexMask = 0;
    bool indexed = false;
    if (!bindVertexStreams(
            mpGraphics, cmd,
            static_cast<cVertexBuffer *>(object->GetVertexBuffer()),
            &vertexMask, &indexed))
      continue;
    struct Push {
      uint32_t blendMode;
      float sceneAlpha;
      uint32_t refractive;
      float lightLevel;
      uint32_t reflectionOnly;
      // Appended, never reordered: the slang Push must match field for field.
      float clipPlane[4];
    };
    // A draw goes through the cull only when the pre-pass claimed a slot for
    // it AND bindVertexStreams agrees about indexing. The second half is a
    // guard, not an expectation: the command's layout is chosen from `indexed`
    // and a disagreement would submit the wrong five words.
    const bool culled =
        cullDispatched && item.commandSlot != kNoCommand && item.indexed == indexed;
    auto drawWith = [&](eMaterialBlendMode blendMode, bool reflectionOnly) {
      // Refractive surfaces blend in the shader against the scene copy, as
      // the legacy renderer's eMaterialBlendMode_None refraction draw did.
      m_program->bindPipeline(
          &mpGraphics->device, cmd, HASH_INITIAL_VALUE, "Standard.translucent",
          MakeTranslucentMeshPipelineDesc(
              cGraphics::PogoColorFormat, cGraphics::DepthFormat,
              refractive && !reflectionOnly
                  ? TranslucentMeshPipelineDesc::BLEND_REPLACE
                  : remapBlend(blendMode),
              vertexMask, material->GetDepthTest()));
      Push push{static_cast<uint32_t>(remapBlend(blendMode)),
                1.0f,
                refractive ? 1u : 0u,
                item.lightLevel,
                reflectionOnly ? 1u : 0u,
                {targets.clipPlane.a, targets.clipPlane.b, targets.clipPlane.c,
                 targets.clipPlane.d}};
      cmd->vk_d3d12_setPushConstants(&mpGraphics->device, *m_program, 0,
                                     sizeof(push), &push);
      if (culled) {
        // drawCount is 1, so Vulkan never reads the stride and the uniform
        // 5-word slot serves both command layouts.
        const uint64_t offset =
            static_cast<uint64_t>(cull.commandBase + item.commandSlot +
                                  recordedDraw) *
            kStandardTranslucentCommandStride;
        if (indexed)
          cmd->drawIndexedIndirect(&mpGraphics->device, cull.buffers.indirect,
                                   offset, 1, kStandardTranslucentCommandStride);
        else
          cmd->drawIndirect(&mpGraphics->device, cull.buffers.indirect, offset,
                            1, kStandardTranslucentCommandStride);
      } else if (indexed) {
        cmd->drawIndexed(
            &mpGraphics->device,
            static_cast<uint32_t>(object->GetVertexBuffer()->GetIndexNum()), 1,
            0, 0, item.slot);
      } else {
        cmd->draw(
            &mpGraphics->device,
            static_cast<uint32_t>(object->GetVertexBuffer()->GetVertexNum()), 1,
            0, item.slot);
      }
      ++recordedDraw;
    };
    drawWith(material->GetBlendMode(), false);
    // Legacy RendererDeferred repeats a non-refractive cube-mapped translucent
    // with additive blending (UseIlluminationTrans) so the reflection is added
    // on top of the blended surface.
    if (!refractive && material->GetImage(eMaterialTexture_CubeMap))
      drawWith(eMaterialBlendMode_Add, true);
  }
  cmd->vk_d3d12_endRendering(&mpGraphics->device);
  transition(mpGraphics, target, RI_RESOURCE_STATE_RENDER_TARGET_READ,
             RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
             RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR);
  transition(mpGraphics, targets.depth,
             (RI_RESOURCE_STATE_DEPTH_READ | RI_RESOURCE_STATE_SHADER_RESOURCE),
             RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_ALL_GRAPHICS,
             RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH);
  return true;
}

} // namespace hpl
