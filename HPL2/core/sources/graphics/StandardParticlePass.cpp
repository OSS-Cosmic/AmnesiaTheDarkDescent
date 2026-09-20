#include "graphics/StandardParticlePass.h"

#include "graphics/ParticlePipelineDesc.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Material.h"
#include "graphics/GlobalManagedSets.h"
#include "scene/ParticleEmitter.h"
#include "scene/World.h"
#include "graphics/Renderable.h"
#include "resources/Resources.h"
#include "math/Frustum.h"
#include "system/Hasher.h"

#include <algorithm>
#include <cstring>

namespace hpl {
namespace {
struct PushBlock {
  uint32_t blendMode;
  float sceneAlpha;
  float lightLevel;
  uint32_t depthTest;
};

static ParticlePipelineDesc::BlendMode remapBlend(eMaterialBlendMode mode) {
  switch (mode) {
  case eMaterialBlendMode_Mul:
    return ParticlePipelineDesc::BLEND_MUL;
  case eMaterialBlendMode_MulX2:
    return ParticlePipelineDesc::BLEND_MULX2;
  case eMaterialBlendMode_Alpha:
    return ParticlePipelineDesc::BLEND_ALPHA;
  case eMaterialBlendMode_PremulAlpha:
    return ParticlePipelineDesc::BLEND_PREMUL_ALPHA;
  case eMaterialBlendMode_Add:
  default:
    return ParticlePipelineDesc::BLEND_ADD;
  }
}

struct ScratchGeometry {
  bool valid = false;
  uint32_t indexCount = 0;
  size_t pos = 0, color = 0, uv = 0, index = 0;
};

static ScratchGeometry copyVertexBuffer(cGraphics *graphics,
                                        cGraphics::FrameContext *frame,
                                        cVertexBuffer *vb) {
  ScratchGeometry out;
  if (!vb || vb->GetVertexNum() <= 0 || vb->GetIndexNum() <= 0 ||
      !vb->GetIndices())
    return out;
  const uint32_t vertices = static_cast<uint32_t>(vb->GetVertexNum());
  RISegmentReq vr = {}, ir = {};
  if (!graphics->RequestTranslucentVtx(frame, vertices * 11u, &vr) ||
      !graphics->RequestTranslucentIdx(frame, vb->GetIndexNum(), &ir))
    return out;

  auto *dst =
      reinterpret_cast<float *>(graphics->translucentVtxBuffer->mappedAddress) +
      vr.elementOffset;
  auto copy = [&](eVertexBufferElement element, float *where, size_t bytes) {
    const auto *source = vb->GetElement(element);
    if (source && source->Data().size() >= bytes)
      std::memcpy(where, source->Data().data(), bytes);
    else
      std::memset(where, 0, bytes);
  };
  copy(eVertexBufferElement_Position, dst, vertices * 4u * sizeof(float));
  copy(eVertexBufferElement_Color0, dst + vertices * 4u,
       vertices * 4u * sizeof(float));
  copy(eVertexBufferElement_Texture0, dst + vertices * 8u,
       vertices * 3u * sizeof(float));
  std::memcpy(reinterpret_cast<uint32_t *>(
                  graphics->translucentIdxBuffer->mappedAddress) +
                  ir.elementOffset,
              vb->GetIndices(), vb->GetIndexNum() * sizeof(uint32_t));
  out.valid = true;
  out.indexCount = static_cast<uint32_t>(vb->GetIndexNum());
  out.pos = vr.elementOffset * sizeof(float);
  out.color = out.pos + vertices * 4u * sizeof(float);
  out.uv = out.pos + vertices * 8u * sizeof(float);
  out.index = ir.elementOffset * sizeof(uint32_t);
  return out;
}
} // namespace

cStandardParticlePass::cStandardParticlePass(cGraphics *graphics,
                                             cResources *resources)
    : mpGraphics(graphics), mpResources(resources) {}
cStandardParticlePass::~cStandardParticlePass() { DestroyData(); }

bool cStandardParticlePass::LoadData() {
  // Draw() calls LoadData() because the standard renderer can be reloaded
  // while a frame is being assembled.  Keep the fast path truly idempotent:
  // replacing this program every draw invalidates the pipeline cache and
  // defers a live shader module unnecessarily.
  if (m_loaded && m_program)
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;
  auto vertBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                            "Standard.particle.3d", "vsMain");
  auto fragBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                            "Standard.particle.3d", "psMain");
  if (vertBin.empty() || fragBin.empty())
    return false;
  auto program = std::make_shared<RIProgram>();
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vertBin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fragBin, "psMain"}};
  const RIBindlessLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.layout()};
  program->initialize(&mpGraphics->device, stages, external,
                      "Standard.particle");
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

void cStandardParticlePass::DestroyData() {
  auto old = std::move(m_program);
  if (old)
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [old = std::move(old), device = &mpGraphics->device]() mutable {
          old->dispose(device);
        }));
  m_loaded = false;
}

bool cStandardParticlePass::Render(
    cGraphics::FrameContext *frame, RICmd *cmd, uint32_t frameIndex,
    uint32_t width, uint32_t height, RITexture *target,
    RITextureView *targetView, RITextureView *depthSampleView,
    cFrustum *frustum, float frameTime, std::span<iRenderable *> translucents,
    cWorld *world, uint32_t viewportSalt,
    RIProgram::DescriptorBinding *frameBinding,
    const RIProgram::DescriptorBinding &fogBinding,
    RISharedPointer<RIBuffer> pointLights, RISharedPointer<RIBuffer> spotLights,
    uint32_t pointLightCount, uint32_t spotLightCount) {
  if (!m_loaded || !m_program || !cmd || !target || !targetView ||
      !depthSampleView || !frustum || !frameBinding)
    return false;
  // Particles take the legacy per-object light level; the shader reads no light buffers.
  (void)pointLights;
  (void)spotLights;
  (void)pointLightCount;
  (void)spotLightCount;

  std::vector<RIProgram::DescriptorBinding> bindings;
  bindings.push_back(*frameBinding);
  // The same image is bound as a read-only depth attachment and sampled by
  // the soft-particle shader during this scope.  Keep both access classes in
  // the descriptor state; the barrier selects DEPTH_READ_ONLY_OPTIMAL while
  // still making the shader read visible.
  bindings.emplace_back(
      "standardSceneDepth",
      RIDescriptor::sampledImage(
          &mpGraphics->device, depthSampleView,
          static_cast<RIResourceState_e>(RI_RESOURCE_STATE_DEPTH_READ |
                                         RI_RESOURCE_STATE_SHADER_RESOURCE)));
  bindings.push_back(fogBinding);

  for (iRenderable *object : translucents) {
    if (!object)
      continue;
    ScratchGeometry geometry;
    // Some beam implementations dereference their material while updating
    // their dynamic geometry.  Reject an incomplete renderable before any
    // type-specific update/build work.
    cMaterial *material = object->GetMaterial();
    if (!material || material->GetBlendMode() == eMaterialBlendMode_None ||
        material->GetBlendMode() >= eMaterialBlendMode_LastEnum)
      continue;
    if (object->GetRenderType() == eRenderableType_ParticleEmitter) {
      const auto particle =
          static_cast<iParticleEmitter *>(object)->BuildScratchGeometry(
              frustum, frameTime, true);
      geometry.valid = particle.valid;
      geometry.indexCount = particle.indexCount;
      geometry.pos = particle.posByteOffset;
      geometry.color = particle.colByteOffset;
      geometry.uv = particle.uvByteOffset;
      geometry.index = particle.idxByteOffset;
    } else if (object->GetRenderType() == eRenderableType_Billboard ||
               object->GetRenderType() == eRenderableType_Beam) {
      object->UpdateGraphicsForFrame(frameTime);
      if (!object->UpdateGraphicsForViewport(frustum, frameTime))
        continue;
      geometry = copyVertexBuffer(mpGraphics, frame, object->GetVertexBuffer());
    } else
      continue;
    if (!geometry.valid)
      continue;
    const uint32_t materialId =
        mpGraphics->globalset->submitMaterial(frame, material, frameIndex)
            .materialId;
    if (materialId == UINT32_MAX)
      continue;
    ObjectSubmitDesc objectData;
    objectData.modelMatrix = object->GetModelMatrix(frustum);
    objectData.uvMatrix = material->GetUvMatrix();
    objectData.materialId = materialId;
    // Backend-neutral buffer references: submitObject resolves them to Vulkan
    // device addresses or D3D12 raw-SRV geometry handles. (Packed
    // streamHandles are Vulkan-only and rejected on D3D12.)
    objectData.streamRefs.pos = {mpGraphics->translucentVtxBuffer.Get(),
                                 geometry.pos};
    objectData.streamRefs.color = {mpGraphics->translucentVtxBuffer.Get(),
                                   geometry.color};
    objectData.streamRefs.uv0 = {mpGraphics->translucentVtxBuffer.Get(),
                                 geometry.uv};
    objectData.streamRefs.index = {mpGraphics->translucentIdxBuffer.Get(),
                                   geometry.index};
    objectData.streamRefs.set = true;
    const hash_t cookie = hash_u32(
        hash_u64(HASH_INITIAL_VALUE, object->GetUniqueCookie()), viewportSalt);
    const uint32_t slot = mpGraphics->globalset->submitObject(
        cookie, frameIndex, nullptr, objectData, kSubmitData);
    if (slot == UINT32_MAX) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        Warning("Standard particle pass: object submit rejected (slot pool "
                "exhausted or stream reference invalid); particle skipped\n");
      }
      continue;
    }
    const auto mode = remapBlend(material->GetBlendMode());
    // Legacy RendererDeferred keys particle pipelines by material DepthTest.
    const bool depthTest = material->GetDepthTest();
    m_program->bindPipeline(
        &mpGraphics->device, cmd, HASH_INITIAL_VALUE, "Standard.particle",
        MakeParticlePipelineDesc(cGraphics::PogoColorFormat,
                                 cGraphics::DepthFormat, mode, depthTest));
    m_program->bindBindlessDescriptorSet(
        cmd, &mpGraphics->globalset->m_bindlessSet, 0);
    m_program->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                               bindings.data(),
                               static_cast<uint32_t>(bindings.size()));
    const MaterialTranslucent *translucentData =
        std::get_if<MaterialTranslucent>(&material->Data());
    const float lightLevel =
        translucentData && translucentData->m_isAffectedByLightLevel
            ? StandardTranslucentLightLevel(world, object)
            : 1.0f;
    PushBlock push{static_cast<uint32_t>(mode), 1.0f, lightLevel,
                   depthTest ? 1u : 0u};
    cmd->vk_d3d12_setPushConstants(&mpGraphics->device, *m_program, 0,
                                   sizeof(push), &push);
    cmd->draw(&mpGraphics->device, geometry.indexCount, 1, 0, slot);
  }
  return true;
}
} // namespace hpl
