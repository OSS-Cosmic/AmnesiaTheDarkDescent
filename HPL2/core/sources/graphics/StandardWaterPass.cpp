#include "graphics/StandardWaterPass.h"
#include "graphics/GlobalManagedSets.h"
#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/RIVK.h"
#include "graphics/Renderable.h"
#include "graphics/Renderer.h"
#include "graphics/TranslucentMeshPipelineDesc.h"
#include "graphics/VertexBuffer.h"
#include "math/Frustum.h"
#include "resources/Resources.h"
#include "scene/World.h"
#include <array>
#include <functional>

namespace hpl {
namespace {
static bool streams(cGraphics *g, RICmd *cmd, cVertexBuffer *vb, uint32_t &mask,
                    bool &indexed) {
  if (!vb || !vb->GetElement(eVertexBufferElement_Position))
    return false;
  auto b = [vb](eVertexBufferElement e) -> RIBuffer * {
    auto *x = vb->GetElement(e);
    return x ? x->GetBuffer() : nullptr;
  };
  RIBuffer *v[5] = {
      b(eVertexBufferElement_Position), b(eVertexBufferElement_Normal),
      b(eVertexBufferElement_Texture1Tangent), b(eVertexBufferElement_Color0),
      b(eVertexBufferElement_Texture0)};
  mask = eVertexElementFlag_Position;
  if (!v[1])
    v[1] = &g->fallbackNormalVertex;
  else
    mask |= eVertexElementFlag_Normal;
  if (!v[2])
    v[2] = &g->fallbackTangentVertex;
  else
    mask |= eVertexElementFlag_Texture1;
  if (!v[3])
    v[3] = &g->fallbackColorVertex;
  else
    mask |= eVertexElementFlag_Color0;
  if (!v[4])
    v[4] = &g->fallbackUv0Vertex;
  else
    mask |= eVertexElementFlag_Texture0;
  cmd->bindVertexBuffers<5>(0, 5, v);
  indexed = vb->GetIndexRIBuffer() != nullptr;
  if (indexed)
    cmd->bindIndexBuffer(&g->device, vb->GetIndexRIBuffer(), 0,
                         RI_INDEX_TYPE_32);
  return true;
}
static void barrier(RICmd *c, RITexture *t, uint32_t a, uint32_t b,
                    RIStageBits_e sa, RIStageBits_e sb,
                    RIBarrierAspect_e aspect = RI_BARRIER_ASPECT_COLOR) {
  c->vk_d3d12_textureBarrier(RITextureBarrier(t, a, b, sa, sb, aspect));
}
} // namespace

cStandardWaterPass::cStandardWaterPass(cGraphics *g, cResources *r)
    : mpGraphics(g), mpResources(r),
      m_reflection(std::make_unique<cStandardWaterReflection>(g, r)) {}
cStandardWaterPass::~cStandardWaterPass() { DestroyData(); }

bool cStandardWaterPass::LoadData() {
  if (m_loaded)
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;
  auto vertBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                            "Standard.water.3d", "vsMain");
  auto fragBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                            "Standard.water.3d", "psMain");
  if (vertBin.empty() || fragBin.empty())
    return false;
  auto p = std::make_shared<RIProgram>();
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vertBin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fragBin, "psMain"}};
  const RIBindlessLayout ext[] = {
      mpGraphics->globalset->m_bindlessSet.layout()};
  p->initialize(&mpGraphics->device, stages, ext, "Standard.water");
  m_program = std::move(p);
  if (!m_reflection || !m_reflection->LoadData())
    return false;
  m_loaded = true;
  return true;
}
void cStandardWaterPass::DestroyData() {
  if (m_reflection)
    m_reflection->DestroyData();
  auto p = std::move(m_program);
  if (p && mpGraphics)
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [p = std::move(p), d = &mpGraphics->device]() mutable {
          p->dispose(d);
        }));
  m_loaded = false;
}

bool cStandardWaterPass::RecordSurface(
    cGraphics::FrameContext *frame, cViewport::StandardViewportState *state,
    uint32_t image, iRenderable *o, cFrustum *frustum, cWorld *world,
    RIProgram::DescriptorBinding *frameBinding,
    RIProgram::DescriptorBinding *fogBinding,
    RISharedPointer<RIBuffer> *pointLights,
    RISharedPointer<RIBuffer> *spotLights, uint32_t pointLightCount,
    uint32_t spotLightCount, RITextureView *shadowView,
    std::span<cFogArea *> visibleFogAreas, RISharedPointer<RIBuffer> *boxLights,
    uint32_t boxLightCount) {
  if (!m_loaded || !m_program || !m_reflection || !frame || !state ||
      !frustum || !world || !frameBinding || !fogBinding ||
      image >= RI_MAX_SWAPCHAIN_IMAGES ||
      state->renderTarget[image].isEmpty() ||
      state->renderTargetAttachmentView[image].isEmpty() ||
      state->depthTextures[image].isEmpty() ||
      state->depthView[image].isEmpty() ||
      state->depthSampleView[image].isEmpty() ||
      state->waterReflectionTexture[image].isEmpty() ||
      state->waterReflectionView[image].isEmpty() || !o ||
      state->waterSceneCopy[image].isEmpty() ||
      state->waterSceneCopyView[image].isEmpty() ||
      o->GetRenderType() != eRenderableType_SubMesh || !o->GetVertexBuffer() ||
      !o->GetMaterial() ||
      o->GetMaterial()->GetMaterialID() != MaterialID::Water)
    return false;
  RICmd *cmd = &mpGraphics->primary.cmds[0];
  RITexture *target = state->renderTarget[image].Get();
  RITexture *copy = state->waterSceneCopy[image].Get();
  cMaterial *material = o->GetMaterial();
  const MaterialWater *water = std::get_if<MaterialWater>(&material->Data());
  Image *cubeMap = material->GetImage(eMaterialTexture_CubeMap);
  const uint32_t cubeMapTextureIndex =
      cubeMap ? cubeMap->GetBindlessSlot() : UINT32_MAX;
  // Keep the authored water setting separate from the capture policy. A cube
  // map replaces the planar capture, but it must not disable the shader's
  // reflection branch. Only authored world reflection requests a capture.
  const bool reflectionEnabled =
      water && water->m_hasReflection && m_worldReflectionEnabled;
  const bool requestWorldReflection = reflectionEnabled && !cubeMap;

  auto sampler = mpGraphics->resolve_filter_descriptor(
      eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
      eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
  if (!sampler)
    return false;
  std::vector<RIProgram::DescriptorBinding> bindings = {*frameBinding,
                                                        *fogBinding};
  bindings.emplace_back(
      "sceneColorInput",
      RIDescriptor::sampledImage(&mpGraphics->device,
                                 state->waterSceneCopyView[image].Get(),
                                 RI_RESOURCE_STATE_SHADER_RESOURCE));
  cStandardWaterReflection::Sample reflection{};
  if (requestWorldReflection) {
    reflection = m_reflection->RecordSurface(
        frame, state, image, state->waterReflectionTexture[image].Get(), o,
        frustum, world, frameBinding, fogBinding, pointLights, spotLights,
        pointLightCount, spotLightCount, shadowView, visibleFogAreas, boxLights,
        boxLightCount);
  }
  bindings.emplace_back(
      "sceneDepthInput",
      // Matches the DEPTH_READ | SHADER_RESOURCE state the barriers below put
      // the depth in. D3D12 only allows sampling with the shader-resource bit;
      // Vulkan still picks DEPTH_READ_ONLY_OPTIMAL because DEPTH_READ wins.
      RIDescriptor::sampledImage(
          &mpGraphics->device, state->depthSampleView[image].Get(),
          static_cast<RIResourceState_e>(RI_RESOURCE_STATE_DEPTH_READ |
                                         RI_RESOURCE_STATE_SHADER_RESOURCE)));
  bindings.emplace_back(
      "waterReflectionInput",
      RIDescriptor::sampledImage(&mpGraphics->device,
                                 reflection.available
                                     ? reflection.view
                                     : state->waterSceneCopyView[image].Get(),
                                 RI_RESOURCE_STATE_SHADER_RESOURCE));
  bindings.emplace_back("waterSampler", *sampler);
  for (auto &b : bindings)
    if (b.descriptor.isEmpty())
      return false;
  // Object/material records are immutable for the remainder of this pass.
  // Publish them before descriptor binding; submitting from inside a rendering
  // scope would make the first water layer race the host mirror flush.
  uint32_t slot = UINT32_MAX;
  {
    auto *vb = static_cast<cVertexBuffer *>(o->GetVertexBuffer());
    vb->SubmitToGPU(&mpGraphics->device);
    o->UpdateGraphicsForViewport(frustum, 0.0f);
    ObjectSubmitDesc od{};
    od.modelMatrix = o->GetModelMatrix(frustum);
    od.uvMatrix = o->GetMaterial()->GetUvMatrix();
    od.materialId =
        mpGraphics->globalset
            ->submitMaterial(frame, o->GetMaterial(), mpGraphics->frameIndex)
            .materialId;
    od.dissolveAmount = o->GetCoverageAmount();
    od.illuminationAmount = o->GetIlluminationAmount();
    od.renderFlags = o->GetRenderFlags();
    if (od.materialId != UINT32_MAX)
      slot = mpGraphics->globalset->submitObject(
          hash_u32(hash_u64(HASH_INITIAL_VALUE, o->GetUniqueCookie()),
                   hash_u32(hash_u64(HASH_INITIAL_VALUE,
                                     reinterpret_cast<uintptr_t>(state)),
                            image)),
          mpGraphics->frameIndex, vb, od);
  }
  if (slot == UINT32_MAX)
    return false;
  mpGraphics->globalset->flushMirrors(&mpGraphics->device);
  // Validate all descriptors before recording any transition.
  barrier(cmd, target, RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_FRAGMENT, RI_STAGE_COPY);
  barrier(cmd, copy,
          state->waterSceneCopyInitialized[image]
              ? RI_RESOURCE_STATE_SHADER_RESOURCE
              : RI_RESOURCE_STATE_UNDEFINED,
          RI_RESOURCE_STATE_COPY_DST, RI_STAGE_FRAGMENT, RI_STAGE_COPY);
  // Named fields, not a braced list: RIImageCopyDesc starts with the source
  // mip / array / offset members, so {width, height, 1} lands in srcMipLevel,
  // srcArrayLayer and srcX and leaves the extent at zero -- a copy with
  // extent.depth == 0, which is invalid for a 2D image.
  RIImageCopyDesc sceneCopy = {};
  sceneCopy.width = state->width;
  sceneCopy.height = state->height;
  sceneCopy.depth = 1;
  cmd->copyImage(&mpGraphics->device, target, copy, sceneCopy);
  barrier(cmd, copy, RI_RESOURCE_STATE_COPY_DST,
          RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COPY, RI_STAGE_FRAGMENT);
  barrier(cmd, target, RI_RESOURCE_STATE_COPY_SRC,
          RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_COPY,
          RI_STAGE_FRAGMENT);
  state->waterSceneCopyInitialized[image] = true;
  barrier(cmd, state->depthTextures[image].Get(),
          RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_RESOURCE_STATE_DEPTH_READ | RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_STAGE_FRAGMENT, RI_STAGE_ALL_GRAPHICS, RI_BARRIER_ASPECT_DEPTH);
  RIRenderingAttachment color{};
  color.view = *state->renderTargetAttachmentView[image];
  color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  RIRenderingAttachment depth{};
  depth.view = *state->depthView[image];
  depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
  depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depth.readOnly = true;
  RIBeginRenderingDesc begin{};
  begin.renderArea.width = state->width;
  begin.renderArea.height = state->height;
  begin.colorCount = 1;
  begin.colors = &color;
  begin.depthStencil = &depth;
  cmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);
  RIViewport vp{};
  vp.y = state->height;
  vp.width = state->width;
  vp.height = -float(state->height);
  vp.depthMax = 1;
  RIRect sc{};
  sc.width = state->width;
  sc.height = state->height;
  cmd->setViewport(&mpGraphics->device, vp);
  cmd->setScissor(&mpGraphics->device, sc);
  m_program->bindBindlessDescriptorSet(
      cmd, &mpGraphics->globalset->m_bindlessSet, 0);
  m_program->bindDescriptors(&mpGraphics->device, cmd, mpGraphics->frameIndex,
                             bindings.data(), bindings.size());
  {
    auto *vb = static_cast<cVertexBuffer *>(o->GetVertexBuffer());
    uint32_t mask;
    bool indexed;
    if (!streams(mpGraphics, cmd, vb, mask, indexed)) {
      cmd->vk_d3d12_endRendering(&mpGraphics->device);
      barrier(cmd, state->depthTextures[image].Get(),
              RI_RESOURCE_STATE_DEPTH_READ | RI_RESOURCE_STATE_SHADER_RESOURCE,
              RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_ALL_GRAPHICS,
              RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH);
      barrier(cmd, target, RI_RESOURCE_STATE_RENDER_TARGET_READ,
              RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
              RI_STAGE_FRAGMENT);
      return false;
    }
    m_program->bindPipeline(
        &mpGraphics->device, cmd, HASH_INITIAL_VALUE, "Standard.water",
        MakeTranslucentMeshPipelineDesc(
            cGraphics::PogoColorFormat, cGraphics::DepthFormat,
            TranslucentMeshPipelineDesc::BLEND_ALPHA, mask, true));
    struct Push {
      uint32_t reflectionAvailable;
      uint32_t refractionEnabled;
      uint32_t reflectionEnabled;
      uint32_t cubeMapTextureIndex;
      float reflectionViewProjection[16];
    };
    Push push{};
    push.refractionEnabled =
        iRenderer::GetRefractionEnabled() && o->GetMaterial()->HasRefraction()
            ? 1u
            : 0u;
    push.reflectionAvailable = reflection.available ? 1u : 0u;
    push.reflectionEnabled = reflectionEnabled ? 1u : 0u;
    push.cubeMapTextureIndex = cubeMapTextureIndex;
    const ml::float4x4 reflectionMatrix =
        cMath::ToFloatTranspose4x4(reflection.viewProjection);
    std::copy(reflectionMatrix.a, reflectionMatrix.a + 16,
              push.reflectionViewProjection);
    cmd->vk_d3d12_setPushConstants(&mpGraphics->device, *m_program, 0,
                                   sizeof(push), &push);
    if (indexed)
      cmd->drawIndexed(&mpGraphics->device, vb->GetIndexNum(), 1, 0, 0, slot);
    else
      cmd->draw(&mpGraphics->device, vb->GetVertexNum(), 1, 0, slot);
  }
  cmd->vk_d3d12_endRendering(&mpGraphics->device);
  barrier(cmd, state->depthTextures[image].Get(),
          RI_RESOURCE_STATE_DEPTH_READ | RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_ALL_GRAPHICS,
          RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH);
  barrier(cmd, target, RI_RESOURCE_STATE_RENDER_TARGET_READ,
          RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
          RI_STAGE_FRAGMENT);
  return true;
}
} // namespace hpl
