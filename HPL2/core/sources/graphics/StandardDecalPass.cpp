#include "graphics/StandardDecalPass.h"

#include "graphics/GlobalManagedSets.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "resources/Resources.h"
#include "scene/World.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <functional>

namespace hpl {
namespace {
// Use the same shared record definition as World's upload allocation.
constexpr uint64_t kGpuDecalByteSize = sizeof(GpuDecal);
} // namespace

cStandardDecalPass::cStandardDecalPass(cGraphics *graphics,
                                       cResources *resources)
    : mpGraphics(graphics), mpResources(resources) {}

cStandardDecalPass::~cStandardDecalPass() { DestroyData(); }

bool cStandardDecalPass::LoadData() {
  if (m_loaded)
    return IsLoaded();
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;
  auto bin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                        "Standard.decal.3d.spv");
  if (bin.empty())
    return false;
  m_program = std::make_shared<RIProgram>();
  const VkDescriptorSetLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.vk.m_bindlessSetLayout};
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, bin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, bin, "psMain"}};
  m_program->initialize(&mpGraphics->device, stages, external,
                        "Standard.decal");
  m_loaded = true;
  return true;
}

void cStandardDecalPass::DestroyData() {
  auto old = std::move(m_program);
  if (old && mpGraphics)
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [old = std::move(old), device = &mpGraphics->device]() mutable {
          old->dispose(device);
        }));
  m_loaded = false;
}

bool cStandardDecalPass::Render(cGraphics::FrameContext *, RICmd *cmd,
                                uint32_t frameIndex, uint32_t width,
                                uint32_t height, RITextureView *colorInput,
                                RITextureView *positionInput,
                                RITextureView *normalInput,
                                RITextureView *surfaceInput,
                                RITextureView *output, cWorld *world,
                                RIProgram::DescriptorBinding *frameBinding) {
  if (!IsLoaded() || !cmd || !colorInput || !positionInput || !normalInput ||
      !surfaceInput || !output || !world || !frameBinding ||
      !world->GetDecalBuffer() || !world->GetDecalObjectIndexBuffer() ||
      world->GetDecalCount() == 0)
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
      rs.lineWidth = 1.0f;
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
  attachment.view = *output;
  attachment.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  RIBeginRenderingDesc begin = {};
  begin.renderArea.width = (int16_t)width;
  begin.renderArea.height = (int16_t)height;
  begin.colorCount = 1;
  begin.colors = &attachment;
  cmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);
  m_program->bindPipeline(&mpGraphics->device, cmd, pd.hash, "Standard.decal",
                          &pd.create);
  m_program->bindBindlessDescriptorSet(
      cmd, &mpGraphics->globalset->m_bindlessSet, 0);

  std::array<RIProgram::DescriptorBinding, 7> bindings = {
      *frameBinding,
      RIProgram::DescriptorBinding(
          "standardColorInput",
          RIDescriptor::sampledImage(&mpGraphics->device, colorInput)),
      RIProgram::DescriptorBinding(
          "standardPositionInput",
          RIDescriptor::sampledImage(&mpGraphics->device, positionInput)),
      RIProgram::DescriptorBinding(
          "standardNormalInput",
          RIDescriptor::sampledImage(&mpGraphics->device, normalInput)),
      RIProgram::DescriptorBinding(
          "standardSurfaceInput",
          RIDescriptor::sampledImage(&mpGraphics->device, surfaceInput)),
      RIProgram::DescriptorBinding(
          "gDecals", RIDescriptor::storageBuffer(
                         &mpGraphics->device, world->GetDecalBuffer(), 0,
                         std::max<uint64_t>(1, world->GetDecalCount()) *
                             kGpuDecalByteSize)),
      RIProgram::DescriptorBinding(
          "gObjectDecalIndices",
          RIDescriptor::storageBuffer(
              &mpGraphics->device, world->GetDecalObjectIndexBuffer(), 0,
              std::max<size_t>(1, world->GetDecalObjectIndices().size()) *
                  sizeof(uint32_t)))};
  m_program->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                             bindings.data(), bindings.size());
  RIViewport viewport;
  viewport.x = 0;
  viewport.y = (float)height;
  viewport.width = (float)width;
  viewport.height = -(float)height;
  viewport.depthMin = 0;
  viewport.depthMax = 1;
  RIRect scissor;
  scissor.x = 0;
  scissor.y = 0;
  scissor.width = (int16_t)width;
  scissor.height = (int16_t)height;
  cmd->setViewport(&mpGraphics->device, viewport);
  cmd->setScissor(&mpGraphics->device, scissor);
  cmd->draw(&mpGraphics->device, 3, 1, 0, 0);
  cmd->vk_d3d12_endRendering(&mpGraphics->device);
  return true;
}
} // namespace hpl
