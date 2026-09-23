#include "graphics/StandardWaterReflection.h"
#include "graphics/GlobalManagedSets.h"
#include "graphics/GraphicUtils.h"
#include "graphics/Material.h"
#include "graphics/StandardTranslucentPass.h"
#include "graphics/StandardWaterReflectionClip.h"
#include "graphics/StandardWaterReflectionSort.h"
#include "graphics/RIVK.h"
#include "graphics/StandardEnvironmentPass.h"
#include "graphics/StandardLightData.h"
#include "graphics/SubMesh.h"
#include "graphics/VertexBuffer.h"
#include "math/Frustum.h"
#include "math/BoundingVolume.h"
#include "resources/Resources.h"
#include "scene/RenderableSet.h"
#include "scene/SubMeshEntity.h"
#include "scene/World.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace hpl {
struct StandardWaterReflectionState {
  uint64_t frame = UINT64_MAX;
  uint32_t captures = 0;
  uint32_t image = UINT32_MAX;
  cStandardWaterPlane plane;
  cStandardWaterReflection::Sample sample;
};
namespace {
RIGraphicsPipelineDesc MakeReflectionPipelineDesc() {
  RIGraphicsPipelineDesc desc = {};
  desc.vertexInput.bindingCount = 5;
  desc.vertexInput.bindings[0] = {0, 16, RI_VERTEX_INPUT_RATE_VERTEX};
  desc.vertexInput.bindings[1] = {1, 12, RI_VERTEX_INPUT_RATE_VERTEX};
  desc.vertexInput.bindings[2] = {2, 16, RI_VERTEX_INPUT_RATE_VERTEX};
  desc.vertexInput.bindings[3] = {3, 16, RI_VERTEX_INPUT_RATE_VERTEX};
  desc.vertexInput.bindings[4] = {4, 12, RI_VERTEX_INPUT_RATE_VERTEX};
  desc.vertexInput.attributeCount = 5;
  desc.vertexInput.attributes[0] = {0, 0, RI_FORMAT_RGB32_SFLOAT, 0};
  desc.vertexInput.attributes[1] = {1, 1, RI_FORMAT_RGB32_SFLOAT, 0};
  desc.vertexInput.attributes[2] = {2, 2, RI_FORMAT_RGBA32_SFLOAT, 0};
  desc.vertexInput.attributes[3] = {3, 3, RI_FORMAT_RGBA32_SFLOAT, 0};
  desc.vertexInput.attributes[4] = {4, 4, RI_FORMAT_RG32_SFLOAT, 0};
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = RI_CULL_MODE_BACK;
  // The reflection view is mirrored through the water plane, which flips the
  // winding: with BACK culling this has to stay COUNTER_CLOCKWISE (the RI
  // default is CLOCKWISE, unlike the zero-initialised Vk create-info).
  desc.raster.frontFace = RI_FRONT_FACE_COUNTER_CLOCKWISE;
  desc.raster.lineWidth = 1.0f;
  desc.renderTarget.colorCount = 2;
  desc.renderTarget.colorFormats[0] = cGraphics::PogoColorFormat;
  desc.renderTarget.colorFormats[1] = RI_FORMAT_RGBA32_SFLOAT;
  desc.renderTarget.depthFormat = cGraphics::DepthFormat;
  desc.depthStencil.depthTest = true;
  desc.depthStencil.depthWrite = true;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;
  desc.blendCount = 2; // must match renderTarget.colorCount
  for (uint32_t i = 0; i < desc.blendCount; ++i)
    desc.blend[i].writeMask = RI_COLOR_WRITE_RGBA; // was colorWriteMask = 0xf
  return desc;
}
static bool samePlane(const cStandardWaterPlane &a,
                      const cStandardWaterPlane &b) {
  return std::fabs(a.normal.x - b.normal.x) < 1e-4f &&
         std::fabs(a.normal.y - b.normal.y) < 1e-4f &&
         std::fabs(a.normal.z - b.normal.z) < 1e-4f &&
         std::fabs(a.distance - b.distance) < 1e-3f;
}
static void bindStreams(cGraphics *g, RICmd *c, cVertexBuffer *v) {
  RIBuffer *x[5] = {};
  auto get = [v](eVertexBufferElement e) {
    auto *p = v->GetElement(e);
    return p ? p->GetBuffer() : nullptr;
  };
  x[0] = get(eVertexBufferElement_Position);
  x[1] = get(eVertexBufferElement_Normal);
  x[2] = get(eVertexBufferElement_Texture1Tangent);
  x[3] = get(eVertexBufferElement_Color0);
  x[4] = get(eVertexBufferElement_Texture0);
  if (!x[1])
    x[1] = &g->fallbackNormalVertex;
  if (!x[2])
    x[2] = &g->fallbackTangentVertex;
  if (!x[3])
    x[3] = &g->fallbackColorVertex;
  if (!x[4])
    x[4] = &g->fallbackUv0Vertex;
  c->bindVertexBuffers<5>(0, 5, x);
  if (v->GetIndexRIBuffer())
    c->bindIndexBuffer(&g->device, v->GetIndexRIBuffer(), 0, RI_INDEX_TYPE_32);
}
} // namespace
struct cStandardWaterReflection::Impl {
  cGraphics *g;
  cResources *r;
  std::shared_ptr<RIProgram> program;
  std::unique_ptr<cStandardEnvironmentPass> environment;
  // Borrowed from cStandardRenderer through cStandardWaterPass. Never owned:
  // a second instance would duplicate the translucent program and its pipeline
  // cache. Null means the reflection skips its translucent sub-pass.
  cStandardTranslucentPass *translucent = nullptr;
  bool clipScreenRect = true;
  // A capture must never re-enter itself. The material filters below already
  // reject water, but the flag makes recursion structurally impossible.
  bool capturing = false;
  bool loaded = false;
  Impl(cGraphics *x, cResources *y)
      : g(x), r(y),
        environment(std::make_unique<cStandardEnvironmentPass>(x, y)) {}
};
cStandardWaterReflection::cStandardWaterReflection(cGraphics *g, cResources *r)
    : m_impl(std::make_unique<Impl>(g, r)) {}
cStandardWaterReflection::~cStandardWaterReflection() { DestroyData(); }
void cStandardWaterReflection::SetTranslucentPass(
    cStandardTranslucentPass *pass) {
  m_impl->translucent = pass;
}
void cStandardWaterReflection::SetClipReflectionScreenRect(bool enabled) {
  m_impl->clipScreenRect = enabled;
}
bool cStandardWaterReflection::LoadData() {
  if (m_impl->loaded)
    return true;
  if (!m_impl->g || !m_impl->r || !m_impl->g->globalset ||
      !m_impl->environment->LoadData())
    return false;
  auto vertBin = RIProgram::loadShaderStage(m_impl->r->GetFileSearcher(),
                                            "Standard.waterReflection.3d", "vsMain");
  auto fragBin = RIProgram::loadShaderStage(m_impl->r->GetFileSearcher(),
                                            "Standard.waterReflection.3d", "psMain");
  if (vertBin.empty() || fragBin.empty())
    return false;
  const RIBindlessLayout ext[] = {
      m_impl->g->globalset->m_bindlessSet.layout()};
  std::array<RIProgram::ModuleStage, 2> s = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vertBin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fragBin, "psMain"}};
  m_impl->program = std::make_shared<RIProgram>();
  m_impl->program->initialize(&m_impl->g->device, s, ext,
                              "Standard.waterReflection");
  m_impl->loaded = true;
  return true;
}
void cStandardWaterReflection::DestroyData() {
  if (m_impl->environment)
    m_impl->environment->DestroyData();
  auto p = std::move(m_impl->program);
  if (p && m_impl->g)
    m_impl->g->graphicsDefer.push(std::function<void()>(
        [p = std::move(p), d = &m_impl->g->device]() mutable {
          p->dispose(d);
        }));
  m_impl->loaded = false;
}
cStandardWaterReflection::Sample cStandardWaterReflection::RecordSurface(
    cGraphics::FrameContext *frame, cViewport::StandardViewportState *state,
    uint32_t image, RITexture *target, iRenderable *surface, cFrustum *main,
    cWorld *world, RIProgram::DescriptorBinding *frameBinding,
    RIProgram::DescriptorBinding *fogBinding, RISharedPointer<RIBuffer> *points,
    RISharedPointer<RIBuffer> *spots, uint32_t pointCount, uint32_t spotCount,
    RITextureView *shadow, std::span<cFogArea *> visibleFogAreas,
    RISharedPointer<RIBuffer> *boxes, uint32_t boxCount) {
  Sample no;
  if (!frame || !state || image >= RI_MAX_SWAPCHAIN_IMAGES || !target ||
      !surface || !main || !world || !frameBinding || !fogBinding ||
      !LoadData() || surface->GetRenderType() != eRenderableType_SubMesh)
    return no;
  if (m_impl->capturing)
    return no;
  auto *w = static_cast<cSubMeshEntity *>(surface);
  if (!w->GetSubMesh() || !w->GetSubMesh()->GetIsOneSided())
    return no;
  const cMatrixf &wm = w->GetWorldMatrix();
  cVector3f n = cMath::Vector3Normalize(
      cMath::MatrixMul3x3(cMath::MatrixInverse(wm.GetRotation()).GetTranspose(),
                          w->GetSubMesh()->GetOneSidedNormal()));
  cVector3f p = cMath::MatrixMul(wm, w->GetSubMesh()->GetOneSidedPoint());
  cStandardWaterPlane plane{n, -cMath::Vector3Dot(n, p)};
  if (!IsValidStandardWaterPlane(plane))
    return no;
  if (!state->waterReflection)
    state->waterReflection = std::make_shared<StandardWaterReflectionState>();
  auto &cache = *state->waterReflection;
  if (cache.frame != m_impl->g->frameIndex) {
    cache.frame = m_impl->g->frameIndex;
    cache.captures = 0;
    cache.image = UINT32_MAX;
    cache.sample = Sample{};
  }
  if (cache.image == image && samePlane(cache.plane, plane) &&
      cache.sample.available)
    return cache.sample;
  if (cache.captures >= kMaxCapturesPerFrame)
    return no;
  if (cMath::Vector3Dot(plane.normal, main->GetOrigin()) + plane.distance < 0) {
    plane.normal = plane.normal * -1;
    plane.distance = -plane.distance;
  }
  cFrustum rf;
  auto rv = MakeStandardWaterReflectedView(main->GetViewMatrix(), plane);
  if (main->GetProjectionType() == eProjectionType_Orthographic)
    rf.SetupOrthoProj(main->GetProjectionMatrix(), rv, main->GetFarPlane(),
                      main->GetNearPlane(), main->GetOrthoViewSize(),
                      ReflectStandardWaterPoint(main->GetOrigin(), plane),
                      main->GetInfFarPlane());
  else
    rf.SetupPerspectiveProj(main->GetProjectionMatrix(), rv,
                            main->GetFarPlane(), main->GetNearPlane(),
                            main->GetFOV(), main->GetAspect(),
                            ReflectStandardWaterPoint(main->GetOrigin(), plane),
                            main->GetInfFarPlane());
  rf.SetInvertsCullMode(true);
  auto &opaque = state->waterReflectionOpaqueTexture[image];
  auto &opaqueView = state->waterReflectionOpaqueView[image];
  auto &opaqueAttachment = state->waterReflectionOpaqueAttachmentView[image];
  auto &pos = state->waterReflectionPositionTexture[image];
  auto &posView = state->waterReflectionPositionView[image];
  auto &posAttachment = state->waterReflectionPositionAttachmentView[image];
  auto &depth = state->waterReflectionDepthTexture[image];
  auto &depthAttachment = state->waterReflectionDepthAttachmentView[image];
  // The reflection colour needs both views of the one texture, as every other
  // capture target here does: the sampled view is what the water surface (and
  // the refraction fallback) reads, the attachment view is the only one D3D12
  // will build an RTV from.
  auto &targetView = state->waterReflectionView[image];
  auto &targetAttachment = state->waterReflectionAttachmentView[image];
  if (opaque.isEmpty() || opaqueView.isEmpty() || opaqueAttachment.isEmpty() ||
      pos.isEmpty() || posView.isEmpty() || posAttachment.isEmpty() ||
      depth.isEmpty() || depthAttachment.isEmpty() || targetView.isEmpty() ||
      targetAttachment.isEmpty())
    return no;
  uint32_t width = std::max(1u, state->width / 2),
           height = std::max(1u, state->height / 2);

  ///////////////////////////
  // Reflection bounds. The authored ReflectionFadeEnd is the same number the
  // legacy material exposed as GetMaxReflectionDistance, so it drives the
  // end-of-reflection plane; beyond it the surface's reflection term is zero.
  float maxReflectionDistance = 0.0f;
  if (auto *waterData =
          std::get_if<MaterialWater>(&surface->GetMaterial()->Data()))
    maxReflectionDistance = waterData->m_reflectionFadeEnd;
  cPlanef surfacePlane;
  surfacePlane.a = plane.normal.x;
  surfacePlane.b = plane.normal.y;
  surfacePlane.c = plane.normal.z;
  surfacePlane.d = plane.distance;
  auto clip = BuildStandardWaterReflectionClip(
      main, &rf, *surface->GetBoundingVolume(), surfacePlane,
      maxReflectionDistance, m_impl->clipScreenRect,
      cVector2l(static_cast<int>(width), static_cast<int>(height)));
  // Legacy skipped the whole capture once the surface itself was past the
  // fade distance (RendererDeferred.cpp:3071-3085).
  if (clip.surfaceOutOfRange)
    return no;

  // Everything past this point records GPU work for this capture.
  m_impl->capturing = true;
  struct CaptureGuard {
    bool *flag;
    ~CaptureGuard() { *flag = false; }
  } captureGuard{&m_impl->capturing};

  RIProgram::DescriptorBinding reflectedFrame;
  if (!m_impl->environment->PrepareFrame(
          frame, &rf, width, height, 0, world, visibleFogAreas,
          world->GetSkyBoxColor(), &reflectedFrame))
    return no;
  std::vector<RIProgram::DescriptorBinding> b = {reflectedFrame, *fogBinding};
  auto ramp = m_impl->g->resolve_filter_descriptor(
      eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
      eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
  auto gobo = m_impl->g->resolve_filter_descriptor(
      eTextureWrap_ClampToBorder, eTextureWrap_ClampToBorder,
      eTextureWrap_ClampToBorder, eTextureFilter_Trilinear);
  if (!ramp || !gobo || !points || !spots || !boxes || points->isEmpty() ||
      spots->isEmpty() || boxes->isEmpty() || !shadow || shadow->isEmpty())
    return no;
  b.emplace_back(
      "standardPointLights",
      RIDescriptor::storageBuffer(&m_impl->g->device, points->Get(), 0,
                                  std::max(pointCount, 1u) *
                                      sizeof(StandardPointLightData)));
  b.emplace_back("standardSpotLights",
                 RIDescriptor::storageBuffer(
                     &m_impl->g->device, spots->Get(), 0,
                     std::max(spotCount, 1u) * sizeof(StandardSpotLightData)));
  b.emplace_back("standardBoxLights",
                 RIDescriptor::storageBuffer(
                     &m_impl->g->device, boxes->Get(), 0,
                     std::max(boxCount, 1u) * sizeof(StandardBoxLightData)));
  StandardLightCounts counts{pointCount, spotCount, boxCount};
  RIProgram::DescriptorBinding cb("standardLightCounts", RIDescriptor(), 0,
                                  false);
  m_impl->g->UpdateFrameUBO(&cb.descriptor, &counts, sizeof(counts));
  b.push_back(cb);
  b.emplace_back("standardRampSampler", *ramp);
  b.emplace_back("standardGoboSampler", *gobo);
  b.emplace_back("standardShadowMap",
                 RIDescriptor::sampledImage(&m_impl->g->device, shadow));
  for (auto &x : b)
    if (x.descriptor.isEmpty())
      return no;
  struct Item {
    iRenderable *o;
    cVertexBuffer *v;
    uint32_t slot;
    bool indexed;
  };
  std::vector<Item> items;
  std::vector<iRenderable *> translucents;
  // Object slots are salted per capture so a reflected model matrix can never
  // land in the slot the main pass published for the same renderable.
  const auto salt = hash_u32(
      hash_u64(hash_u64(HASH_INITIAL_VALUE, reinterpret_cast<uintptr_t>(state)),
               image),
      cache.captures);
  auto admit = [&](iRenderable *o) {
    if (!o || o == surface || o->GetRenderType() != eRenderableType_SubMesh ||
        !o->GetVertexBuffer() || !o->GetMaterial())
      return;
    const MaterialID id = o->GetMaterial()->GetMaterialID();
    if (id == MaterialID::Unknown)
      return;
    // Recursion guard. Legacy rejected only water that itself had a world
    // reflection, so cube-map water did appear inside reflections; rejecting
    // all of it avoids a nested scene copy and a nested capture.
    if (id == MaterialID::Water)
      return;
    if (cMaterial::IsTranslucent(id)) {
      // Decals are not handled by the reflection yet; they would need the
      // mesh-decal program and its accumulators.
      if (id != MaterialID::Decal)
        translucents.push_back(o);
      return;
    }
    o->UpdateGraphicsForViewport(&rf, 0);
    auto *v = static_cast<cVertexBuffer *>(o->GetVertexBuffer());
    v->SubmitToGPU(&m_impl->g->device);
    ObjectSubmitDesc d{};
    d.modelMatrix = o->GetModelMatrix(&rf);
    d.uvMatrix = o->GetMaterial()->GetUvMatrix();
    d.materialId =
        m_impl->g->globalset
            ->submitMaterial(frame, o->GetMaterial(), m_impl->g->frameIndex)
            .materialId;
    d.renderFlags = o->GetRenderFlags();
    d.illuminationAmount = o->GetIlluminationAmount();
    // Without this the shader's alpha test sees a dissolving surface: the
    // struct default is 0, while iRenderable's real default is 1.
    d.dissolveAmount = o->GetCoverageAmount();
    auto slot = m_impl->g->globalset->submitObject(
        hash_u32(hash_u64(HASH_INITIAL_VALUE, o->GetUniqueCookie()), salt),
        m_impl->g->frameIndex, v, d);
    if (slot != UINT32_MAX)
      items.push_back({o, v, slot, v->GetIndexRIBuffer() != nullptr});
  };
  // The walker supplies what the raw container scan never did: IsVisible(),
  // the reflection render flag, the reflected-frustum cull, and the clip
  // planes. cStandardRenderer::Draw already ran UpdateBeforeRendering on both
  // sets this frame.
  for (int t = eWorldContainerType_Static; t <= eWorldContainerType_Dynamic;
       ++t) {
    auto *set = world->GetRenderableSet(static_cast<eWorldContainerType>(t));
    if (!set)
      continue;
    rendering::WalkAndPrepareRenderList(set, &rf, admit,
                                        eRenderableFlag_VisibleInReflection,
                                        false, clip.Planes());
  }
  // Back-to-front, the same ordering the main translucent list uses.
  std::stable_sort(translucents.begin(), translucents.end(),
                   [&](iRenderable *a, iRenderable *b) {
                     return StandardReflectionTranslucentViewZ(
                                &rf, *a->GetBoundingVolume()) <
                            StandardReflectionTranslucentViewZ(
                                &rf, *b->GetBoundingVolume());
                   });
  m_impl->g->globalset->flushMirrors(&m_impl->g->device);
  RICmd *cmd = &m_impl->g->primary.cmds[0];
  // The reflection colour is the environment resolve's attachment. Nothing
  // else brings it into RENDER_TARGET, so do it here; after the first capture
  // it comes back from SHADER_RESOURCE rather than UNDEFINED.
  const bool reflectionInitialized = state->waterReflectionInitialized[image];
  cmd->vk_d3d12_textureBarrier(RITextureBarrier(
      target,
      reflectionInitialized ? RI_RESOURCE_STATE_SHADER_RESOURCE
                            : RI_RESOURCE_STATE_UNDEFINED,
      RI_RESOURCE_STATE_RENDER_TARGET,
      reflectionInitialized ? RI_STAGE_FRAGMENT : RI_STAGE_NONE,
      RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
  state->waterReflectionInitialized[image] = true;
  cmd->vk_d3d12_textureBarrier(
      RITextureBarrier(opaque.Get(), RI_RESOURCE_STATE_UNDEFINED,
                       RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
                       RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
  cmd->vk_d3d12_textureBarrier(RITextureBarrier(
      pos.Get(), RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET,
      RI_STAGE_NONE, RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
  cmd->vk_d3d12_textureBarrier(RITextureBarrier(
      depth.Get(), RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_DEPTH_WRITE,
      RI_STAGE_NONE, RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH));
  RIRenderingAttachment ca[2]{};
  ca[0].view = *opaqueAttachment;
  ca[1].view = *posAttachment;
  for (auto &x : ca) {
    x.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
    x.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  }
  RIRenderingAttachment da{};
  da.view = *depthAttachment;
  da.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  da.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  da.clearValue.depth = 1;
  RIBeginRenderingDesc begin{};
  begin.renderArea.width = width;
  begin.renderArea.height = height;
  begin.colorCount = 2;
  begin.colors = ca;
  begin.depthStencil = &da;
  cmd->vk_d3d12_beginRendering(&m_impl->g->device, begin);
  m_impl->program->bindBindlessDescriptorSet(
      cmd, &m_impl->g->globalset->m_bindlessSet, 0);
  m_impl->program->bindDescriptors(&m_impl->g->device, cmd,
                                   m_impl->g->frameIndex, b.data(), b.size());
  const RIGraphicsPipelineDesc pipe = MakeReflectionPipelineDesc();
  m_impl->program->bindPipeline(&m_impl->g->device, cmd, HASH_INITIAL_VALUE,
                                "Standard.waterReflection", pipe);
  RIViewport vp{};
  vp.y = height;
  vp.width = width;
  vp.height = -float(height);
  vp.depthMax = 1;
  // The viewport stays full-extent; only the scissor narrows to the water
  // surface's screen footprint.
  RIRect sc{};
  if (clip.hasScissor) {
    sc.x = static_cast<int16_t>(clip.scissor.x);
    sc.y = static_cast<int16_t>(clip.scissor.y);
    sc.width = static_cast<int16_t>(clip.scissor.w);
    sc.height = static_cast<int16_t>(clip.scissor.h);
  } else {
    sc.width = width;
    sc.height = height;
  }
  cmd->setViewport(&m_impl->g->device, vp);
  cmd->setScissor(&m_impl->g->device, sc);
  struct Push {
    float p[4];
  } push{{plane.normal.x, plane.normal.y, plane.normal.z, plane.distance}};
  cmd->vk_d3d12_setPushConstants(&m_impl->g->device, *m_impl->program, 0,
                                 sizeof(push), &push);
  for (auto &i : items) {
    bindStreams(m_impl->g, cmd, i.v);
    if (i.indexed)
      cmd->drawIndexed(&m_impl->g->device, i.v->GetIndexNum(), 1, 0, 0, i.slot);
    else
      cmd->draw(&m_impl->g->device, i.v->GetVertexNum(), 1, 0, i.slot);
  }
  cmd->vk_d3d12_endRendering(&m_impl->g->device);
  cmd->vk_d3d12_textureBarrier(
      RITextureBarrier(opaque.Get(), RI_RESOURCE_STATE_RENDER_TARGET,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                       RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
  cmd->vk_d3d12_textureBarrier(
      RITextureBarrier(pos.Get(), RI_RESOURCE_STATE_RENDER_TARGET,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                       RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
  cmd->vk_d3d12_textureBarrier(
      RITextureBarrier(depth.Get(), RI_RESOURCE_STATE_DEPTH_WRITE,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                       RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH));
  auto *out = targetView.Get();
  if (!m_impl->environment->Render(frame, cmd, m_impl->g->frameIndex, width,
                                   height, opaque.Get(), opaqueView.Get(),
                                   posView.Get(), targetAttachment.Get(), world,
                                   &reflectedFrame)) {
    // Hand the colour back in the state the flag now claims for it.
    cmd->vk_d3d12_textureBarrier(
        RITextureBarrier(target, RI_RESOURCE_STATE_RENDER_TARGET,
                         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                         RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
    return no;
  }
  cmd->vk_d3d12_textureBarrier(
      RITextureBarrier(target, RI_RESOURCE_STATE_RENDER_TARGET,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                       RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
  ///////////////////////////
  // Reflected translucency. The legacy reflection recursed into the whole
  // renderer, translucent pass included, so glass and other blended meshes
  // reflect blended rather than as opaque blobs. The pass enters and leaves
  // both colour and depth in SHADER_RESOURCE, which is exactly the state the
  // environment resolve and the opaque barriers above leave behind.
  if (m_impl->translucent && !translucents.empty() &&
      !state->waterReflectionDepthSampleView[image].isEmpty()) {
    cStandardTranslucentPass::Targets t;
    t.color = target;
    t.colorAttachmentView = targetAttachment.Get();
    t.depth = depth.Get();
    t.depthAttachmentView = depthAttachment.Get();
    t.depthSampleView = state->waterReflectionDepthSampleView[image].Get();
    if (!state->waterReflectionSceneCopy[image].isEmpty() &&
        !state->waterReflectionSceneCopyView[image].isEmpty()) {
      t.sceneCopy = state->waterReflectionSceneCopy[image].Get();
      t.sceneCopyView = state->waterReflectionSceneCopyView[image].Get();
      t.sceneCopyInitialized =
          &state->waterReflectionSceneCopyInitialized[image];
    } else {
      // No copy: refraction is skipped and the authored blend is used.
      t.sceneCopyView = out;
    }
    t.width = width;
    t.height = height;
    t.scissor = sc;
    t.clipPlane = surfacePlane;
    t.slotSalt = salt;
    m_impl->translucent->Draw(frame, t, translucents, &rf, world,
                              &reflectedFrame, fogBinding, shadow, points,
                              spots, pointCount, spotCount, nullptr);
  }
  cache.captures++;
  cache.image = image;
  cache.plane = plane;
  cache.sample = {
      out,
      MakeStandardWaterReflectedViewProjection(main->GetProjectionMatrix(),
                                               main->GetViewMatrix(), plane),
      out != nullptr};
  return cache.sample;
}
} // namespace hpl
