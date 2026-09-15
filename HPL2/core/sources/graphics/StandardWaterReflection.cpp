#include "graphics/StandardWaterReflection.h"
#include "graphics/GlobalManagedSets.h"
#include "graphics/Material.h"
#include "graphics/RIVK.h"
#include "graphics/StandardEnvironmentPass.h"
#include "graphics/StandardLightData.h"
#include "graphics/SubMesh.h"
#include "graphics/VertexBuffer.h"
#include "math/Frustum.h"
#include "resources/Resources.h"
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
struct ReflectionPipeline {
  VkVertexInputBindingDescription b[5]{};
  VkVertexInputAttributeDescription a[5]{};
  VkPipelineVertexInputStateCreateInfo vi{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  VkDynamicState ds[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  VkFormat formats[2]{};
  VkPipelineRenderingCreateInfo rendering{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState blend[2]{};
  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  VkGraphicsPipelineCreateInfo create{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  hash_t hash = 0;
  ReflectionPipeline() {
    b[0] = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    b[1] = {1, 12, VK_VERTEX_INPUT_RATE_VERTEX};
    b[2] = {2, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    b[3] = {3, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    b[4] = {4, 12, VK_VERTEX_INPUT_RATE_VERTEX};
    a[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    a[1] = {1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0};
    a[2] = {2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    a[3] = {3, 3, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    a[4] = {4, 4, VK_FORMAT_R32G32_SFLOAT, 0};
    vi.vertexBindingDescriptionCount = 5;
    vi.pVertexBindingDescriptions = b;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = a;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = ds;
    formats[0] = RIFormatToVK(cGraphics::PogoColorFormat);
    formats[1] = RIFormatToVK(RI_FORMAT_RGBA32_SFLOAT);
    rendering.colorAttachmentCount = 2;
    rendering.pColorAttachmentFormats = formats;
    rendering.depthAttachmentFormat = RIFormatToVK(cGraphics::DepthFormat);
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    for (auto &x : blend)
      x.colorWriteMask = 0xf;
    cb.attachmentCount = 2;
    cb.pAttachments = blend;
    create.pNext = &rendering;
    create.pVertexInputState = &vi;
    create.pInputAssemblyState = &ia;
    create.pRasterizationState = &rs;
    create.pDynamicState = &dyn;
    create.pViewportState = &vp;
    create.pMultisampleState = &ms;
    create.pDepthStencilState = &depth;
    create.pColorBlendState = &cb;
    hash = hash_u32(hash_u32(HASH_INITIAL_VALUE, cGraphics::PogoColorFormat),
                    RI_FORMAT_RGBA32_SFLOAT);
  }
};
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
  bool loaded = false;
  Impl(cGraphics *x, cResources *y)
      : g(x), r(y),
        environment(std::make_unique<cStandardEnvironmentPass>(x, y)) {}
};
cStandardWaterReflection::cStandardWaterReflection(cGraphics *g, cResources *r)
    : m_impl(std::make_unique<Impl>(g, r)) {}
cStandardWaterReflection::~cStandardWaterReflection() { DestroyData(); }
bool cStandardWaterReflection::LoadData() {
  if (m_impl->loaded)
    return true;
  if (!m_impl->g || !m_impl->r || !m_impl->g->globalset ||
      !m_impl->environment->LoadData())
    return false;
  auto bin = RIProgram::loadShaderStage(m_impl->r->GetFileSearcher(),
                                        "Standard.waterReflection.3d.spv");
  if (bin.empty())
    return false;
  const VkDescriptorSetLayout ext[] = {
      m_impl->g->globalset->m_bindlessSet.vk.m_bindlessSetLayout};
  std::array<RIProgram::ModuleStage, 2> s = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, bin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, bin, "psMain"}};
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
  if (opaque.isEmpty() || opaqueView.isEmpty() || opaqueAttachment.isEmpty() ||
      pos.isEmpty() || posView.isEmpty() || posAttachment.isEmpty() ||
      depth.isEmpty() || depthAttachment.isEmpty())
    return no;
  uint32_t width = std::max(1u, state->width / 2),
           height = std::max(1u, state->height / 2);
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
  for (int t = eWorldContainerType_Static; t <= eWorldContainerType_Dynamic;
       ++t) {
    auto *set = world->GetRenderableSet(static_cast<eWorldContainerType>(t));
    if (!set)
      continue;
    for (auto *o : set->GetObjects()) {
      if (!o || o == surface || o->GetRenderType() != eRenderableType_SubMesh ||
          !o->GetVertexBuffer() || !o->GetMaterial() ||
          o->GetMaterial()->GetMaterialID() == MaterialID::Water ||
          !o->GetRenderFlagBit(eRenderableFlag_VisibleInReflection))
        continue;
      o->UpdateGraphicsForViewport(&rf, 0);
      auto *v = static_cast<cVertexBuffer *>(o->GetVertexBuffer());
      v->SubmitToGPU(&m_impl->g->blasSubmit.cmds[0], &m_impl->g->device, frame);
      ObjectSubmitDesc d{};
      d.modelMatrix = o->GetModelMatrix(&rf);
      d.uvMatrix = o->GetMaterial()->GetUvMatrix();
      d.materialId =
          m_impl->g->globalset
              ->submitMaterial(frame, o->GetMaterial(), m_impl->g->frameIndex)
              .materialId;
      d.renderFlags = o->GetRenderFlags();
      d.illuminationAmount = o->GetIlluminationAmount();
      auto salt =
          hash_u32(hash_u64(hash_u64(HASH_INITIAL_VALUE,
                                     reinterpret_cast<uintptr_t>(state)),
                            image),
                   cache.captures);
      auto slot = m_impl->g->globalset->submitObject(
          hash_u32(hash_u64(HASH_INITIAL_VALUE, o->GetUniqueCookie()), salt),
          m_impl->g->frameIndex, v, d);
      if (slot != UINT32_MAX)
        items.push_back({o, v, slot, v->GetIndexRIBuffer() != nullptr});
    }
  }
  m_impl->g->globalset->flushMirrors(&m_impl->g->device);
  RICmd *cmd = &m_impl->g->primary.cmds[0];
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
  ReflectionPipeline pipe;
  m_impl->program->bindPipeline(&m_impl->g->device, cmd, pipe.hash,
                                "Standard.waterReflection", &pipe.create);
  RIViewport vp{};
  vp.y = height;
  vp.width = width;
  vp.height = -float(height);
  vp.depthMax = 1;
  RIRect sc{};
  sc.width = width;
  sc.height = height;
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
  auto *out = state->waterReflectionView[image].Get();
  if (!m_impl->environment->Render(frame, cmd, m_impl->g->frameIndex, width,
                                   height, opaque.Get(), opaqueView.Get(),
                                   posView.Get(), out, world, &reflectedFrame))
    return no;
  cmd->vk_d3d12_textureBarrier(
      RITextureBarrier(target, RI_RESOURCE_STATE_RENDER_TARGET,
                       RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                       RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
  cache.captures++;
  cache.image = image;
  cache.plane = plane;
  cache.sample = {
      out,
      MakeStandardWaterReflectedViewProjection(main->GetProjectionMatrix(),
                                               main->GetViewMatrix(), plane),
      out != nullptr};
  state->waterReflectionInitialized[image] = cache.sample.available;
  return cache.sample;
}
} // namespace hpl
