#include "graphics/StandardEnvironmentPass.h"
#include "graphics/Graphics.h"
#include "graphics/GlobalManagedSets.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/TemporalUpscalerPolicy.h"
#include "resources/Resources.h"
#include "scene/World.h"
#include "scene/Viewport.h"
#include "scene/FogArea.h"
#include "graphics/Color.h"
#include "graphics/Image.h"
#include <array>
#include <functional>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace hpl {
static uint32_t EnvironmentTextureSlot(Image *image) {
  if (!image)
    return UINT32_MAX;
  Interface<cGraphics>::Get()->graphicsDefer.push(PinResource(image));
  return image->GetBindlessSlot();
}
cStandardEnvironmentPass::cStandardEnvironmentPass(cGraphics *g, cResources *r)
    : mpGraphics(g), mpResources(r) {}
cStandardEnvironmentPass::~cStandardEnvironmentPass() { DestroyData(); }

bool cStandardEnvironmentPass::LoadData() {
  if (m_loaded)
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;
  auto bin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                        "Standard.environment.3d.spv");
  if (bin.empty())
    return false;
  m_program = std::make_shared<RIProgram>();
  const VkDescriptorSetLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.vk.m_bindlessSetLayout};
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, bin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, bin, "psMain"}};
  m_program->initialize(&mpGraphics->device, stages, external,
                        "Standard.environment");
  m_dummyFog = RISharedPointer<RIBuffer>(
      &mpGraphics->device,
      RIBuffer::create(&mpGraphics->device,
                       {sizeof(FogAreaParams),
                        RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
                        RI_MEMORY_DEVICE, 0}));
  m_loaded = !m_dummyFog.isEmpty();
  if (!m_loaded)
    DestroyData();
  return m_loaded;
}

void cStandardEnvironmentPass::DestroyData() {
  auto old = std::move(m_program);
  auto dummyFog = std::move(m_dummyFog);
  if (old && mpGraphics)
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [old = std::move(old), dummyFog = std::move(dummyFog),
         device = &mpGraphics->device]() mutable {
          if (old)
            old->dispose(device);
          // The deferred lambda owns the dummy until the GPU is finished.
          (void)dummyFog;
        }));
  m_loaded = false;
}

bool cStandardEnvironmentPass::PrepareFrame(
    cGraphics::FrameContext *, cFrustum *frustum, uint32_t width,
    uint32_t height, float time, cWorld *world,
    std::span<cFogArea *> visibleFogAreas, const cColor &clearColor,
    RIProgram::DescriptorBinding *frameBinding,
    const hpl::TemporalFrameSnapshot *temporalSnapshot) {
  if (!mpGraphics || !frustum || !world || !frameBinding)
    return false;
  SceneConstants frame = {};
  const ml::float4x4 view =
      cMath::ToFloatTranspose4x4(frustum->GetViewMatrix());
  const ml::float4x4 invView = cMath::ToFloatTranspose4x4(
      cMath::MatrixInverse(frustum->GetViewMatrix()));
  const cMatrixf projection = frustum->GetProjectionMatrix();
  const ml::float4x4 proj = cMath::ToFloatTranspose4x4(projection);
  const ml::float4x4 invProj =
      cMath::ToFloatTranspose4x4(cMath::MatrixInverse(projection));
  std::memcpy(frame.viewMat, view.a, sizeof(frame.viewMat));
  std::memcpy(frame.invViewMat, invView.a, sizeof(frame.invViewMat));
  std::memcpy(frame.projMat, proj.a, sizeof(frame.projMat));
  std::memcpy(frame.invProjMat, invProj.a, sizeof(frame.invProjMat));
  if (temporalSnapshot) {
    std::memcpy(frame.projMat, temporalSnapshot->projMat,
                sizeof(frame.projMat));
    std::memcpy(frame.invProjMat, temporalSnapshot->invProjMat,
                sizeof(frame.invProjMat));
    std::memcpy(frame.unjitteredProjMat, temporalSnapshot->unjitteredProjMat,
                sizeof(frame.unjitteredProjMat));
    std::memcpy(frame.prevViewMat, temporalSnapshot->prevViewMat,
                sizeof(frame.prevViewMat));
    std::memcpy(frame.prevProjMat, temporalSnapshot->prevUnjitteredProjMat,
                sizeof(frame.prevProjMat));
    frame.jitterX = temporalSnapshot->jitterUV[0];
    frame.jitterY = temporalSnapshot->jitterUV[1];
    frame.prevJitterX = temporalSnapshot->prevJitterUV[0];
    frame.prevJitterY = temporalSnapshot->prevJitterUV[1];
    frame.materialMipBias =
        hpl::TemporalMaterialMipBias({width, height}, {width, height}, true);
  } else {
    std::memcpy(frame.unjitteredProjMat, proj.a,
                sizeof(frame.unjitteredProjMat));
    std::memcpy(frame.prevProjMat, proj.a, sizeof(frame.prevProjMat));
  }
  std::memcpy(frame.invViewRotationMat, invView.a,
              sizeof(frame.invViewRotationMat));
  frame.invViewRotationMat[12] = frame.invViewRotationMat[13] =
      frame.invViewRotationMat[14] = 0.0f;
  frame.posW = {invView.a[12], invView.a[13], invView.a[14]};
  frame.viewportSize[0] = float(width);
  frame.viewportSize[1] = float(height);
  frame.viewTexel[0] = width ? 1.0f / width : 0.0f;
  frame.viewTexel[1] = height ? 1.0f / height : 0.0f;
  frame.cameraFov = frustum->GetFOV();
  frame.zNear = frustum->GetNearPlane();
  frame.zFar = frustum->GetFarPlane();
  frame.orthographicCamera =
      frustum->GetProjectionType() == eProjectionType_Orthographic ? 1u : 0u;
  frame.clearColor = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};
  frame.afT = time;
  frame.totalFrames = mpGraphics->frameIndex;

  const cColor sky = world->GetSkyBoxColor();
  const bool active = world->IsActive() && world->GetSkyBoxActive();
  Image *cube = world->GetSkyBoxImage();
  const bool validCube = active && cube && cube->IsBindlessCube();
  frame.skyBoxEnabled = active ? 1u : 0u;
  frame.skyBoxTextureIndex =
      validCube ? EnvironmentTextureSlot(cube) : UINT32_MAX;
  // Display-space tint, as authored: the base game multiplied the cube texel
  // by it before anything was encoded. The shaders decode the product.
  frame.skyBoxColor = {sky.r, sky.g, sky.b, sky.a};
  if (world->GetFogActive()) {
    const cColor fog = world->GetFogColor();
    frame.fogEnabled = 1u;
    frame.worldFogStart = world->GetFogStart();
    frame.worldFogLength = world->GetFogEnd() - world->GetFogStart();
    frame.fogFalloffExp = world->GetFogFalloffExp();
    frame.worldFogColor = {sRGBToLinear(fog.r), sRGBToLinear(fog.g),
                           sRGBToLinear(fog.b), fog.a};
    frame.oneMinusFogAlpha = 1.0f - fog.a;
  }
  frame.fogAreaCount = 0;
  for (cFogArea *area : visibleFogAreas) {
    if (frame.fogAreaCount >= kFogAreaCapacity)
      break;
    uint32_t n = 0;
    auto it = world->GetFogAreaIterator();
    while (it.HasNext()) {
      if (it.Next() == area && n < world->GetFogAreaCount()) {
        frame.fogAreaIndices[frame.fogAreaCount++] = n;
        break;
      }
      ++n;
    }
  }
  *frameBinding =
      RIProgram::DescriptorBinding("gPerFrame", RIDescriptor(), 0, false);
  mpGraphics->UpdateFrameUBO(&frameBinding->descriptor, &frame, sizeof(frame));
  return true;
}

void cStandardEnvironmentPass::AppendFogBindings(
    cWorld *world, std::vector<RIProgram::DescriptorBinding> &bindings) {
  RIBuffer *fog = world ? world->GetFogAreaBuffer() : nullptr;
  const uint32_t count = world ? world->GetFogAreaCount() : 0u;
  if (!fog)
    fog = m_dummyFog.Get();
  if (fog)
    bindings.emplace_back(
        "gFogAreas",
        RIDescriptor::storageBuffer(&mpGraphics->device, fog, 0,
                                    sizeof(FogAreaParams) *
                                        (world && world->GetFogAreaBuffer()
                                             ? std::max(count, 1u)
                                             : 1u)));
}

bool cStandardEnvironmentPass::Render(
    cGraphics::FrameContext *, RICmd *cmd, uint32_t frameIndex, uint32_t width,
    uint32_t height, RITexture *sceneColor, RITextureView *sceneColorView,
    RITextureView *positionView, RITextureView *outputView, cWorld *world,
    RIProgram::DescriptorBinding *frameBinding) {
  if (!m_loaded || !m_program || !cmd || !sceneColor || !sceneColorView ||
      !positionView || !outputView || !frameBinding)
    return false;
  struct Pipeline {
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                             VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo create{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkFormat format = RIFormatToVK(cGraphics::PogoColorFormat);
    hash_t hash = hash_u32(HASH_INITIAL_VALUE, cGraphics::PogoColorFormat);
    Pipeline() {
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      rs.cullMode = VK_CULL_MODE_NONE;
      rs.polygonMode = VK_POLYGON_MODE_FILL;
      rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
      rs.lineWidth = 1;
      ds.dynamicStateCount = 2;
      ds.pDynamicStates = dyn;
      rendering.colorAttachmentCount = 1;
      rendering.pColorAttachmentFormats = &format;
      vp.viewportCount = 1;
      vp.scissorCount = 1;
      ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      blend.colorWriteMask = 0xf;
      cb.attachmentCount = 1;
      cb.pAttachments = &blend;
      create.pNext = &rendering;
      create.pVertexInputState = &vi;
      create.pInputAssemblyState = &ia;
      create.pRasterizationState = &rs;
      create.pDynamicState = &ds;
      create.pViewportState = &vp;
      create.pMultisampleState = &ms;
      create.pDepthStencilState = &depth;
      create.pColorBlendState = &cb;
    }
  } pd;
  RIRenderingAttachment attachment = {};
  attachment.view = *outputView;
  attachment.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  RIBeginRenderingDesc begin = {};
  begin.renderArea.width = (int16_t)width;
  begin.renderArea.height = (int16_t)height;
  begin.colorCount = 1;
  begin.colors = &attachment;
  cmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);
  m_program->bindPipeline(&mpGraphics->device, cmd, pd.hash,
                          "Standard.environment", &pd.create);
  m_program->bindBindlessDescriptorSet(
      cmd, &mpGraphics->globalset->m_bindlessSet, 0);
  RIProgram::DescriptorBinding inputs[3];
  inputs[0] = *frameBinding;
  inputs[1].handle = DescriptorBindingID::Create("sceneColorInput");
  inputs[1].descriptor =
      RIDescriptor::sampledImage(&mpGraphics->device, sceneColorView);
  inputs[2].handle = DescriptorBindingID::Create("positionInput");
  inputs[2].descriptor =
      RIDescriptor::sampledImage(&mpGraphics->device, positionView);
  std::vector<RIProgram::DescriptorBinding> bindings = {inputs[0], inputs[1],
                                                        inputs[2]};
  AppendFogBindings(world, bindings);
  m_program->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                             bindings.data(), (uint32_t)bindings.size());
  RIViewport vp;
  vp.x = 0;
  vp.y = (float)height;
  vp.width = (float)width;
  vp.height = -(float)height;
  vp.depthMin = 0;
  vp.depthMax = 1;
  RIRect sc;
  sc.x = 0;
  sc.y = 0;
  sc.width = (int16_t)width;
  sc.height = (int16_t)height;
  cmd->setViewport(&mpGraphics->device, vp);
  cmd->setScissor(&mpGraphics->device, sc);
  cmd->draw(&mpGraphics->device, 3, 1, 0, 0);
  cmd->vk_d3d12_endRendering(&mpGraphics->device);
  return true;
}
} // namespace hpl
