#include "graphics/StandardShadowPass.h"
#include "graphics/GlobalManagedSets.h"
#include "resources/Resources.h"
#include <array>
#include <algorithm>
#include <cmath>

namespace hpl {
cStandardShadowPass::cStandardShadowPass(cGraphics *g, cResources *r)
    : mpGraphics(g), mpResources(r) {}
cStandardShadowPass::~cStandardShadowPass() { DestroyData(); }

bool cStandardShadowPass::LoadData() {
  if (m_loaded)
    return true;
  if (!mpGraphics || !mpResources || !mpGraphics->globalset)
    return false;
  auto bin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                        "Standard.shadow.3d.spv");
  if (bin.empty())
    return false;
  const VkDescriptorSetLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.vk.m_bindlessSetLayout};
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, bin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, bin, "psMain"}};
  m_program = std::make_shared<RIProgram>();
  m_program->initialize(&mpGraphics->device, stages, external,
                        "Standard.shadow");
  m_loaded = true;
  return true;
}

void cStandardShadowPass::DestroyData() {
  auto old = std::move(m_program);
  m_loaded = false;
  if (old && mpGraphics)
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [old = std::move(old), device = &mpGraphics->device]() mutable {
          old->dispose(device);
        }));
}

namespace {
struct ShadowPipelineDesc {
  VkPipelineVertexInputStateCreateInfo vi{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineRenderingCreateInfo rendering{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkGraphicsPipelineCreateInfo create{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  hash_t hash = 0;
  explicit ShadowPipelineDesc(float slopeScaleBias) {
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = slopeScaleBias != 0.0f ? VK_TRUE : VK_FALSE;
    rs.depthBiasSlopeFactor = slopeScaleBias;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    rendering.depthAttachmentFormat =
        RIFormatToVK(cStandardShadowPass::kAtlasFormat);
    create.pNext = &rendering;
    create.pVertexInputState = &vi;
    create.pInputAssemblyState = &ia;
    create.pRasterizationState = &rs;
    create.pDynamicState = &ds;
    create.pViewportState = &vp;
    create.pMultisampleState = &ms;
    create.pDepthStencilState = &depth;
    hash = hash_f32(
        hash_u32(HASH_INITIAL_VALUE, cStandardShadowPass::kAtlasFormat),
        slopeScaleBias);
  }
};

bool ValidTile(const cStandardShadowPass::Tile &tile, uint32_t atlasSize,
               RIBuffer *indirect) {
  if (tile.size == 0 || uint64_t(tile.x) + tile.size > atlasSize ||
      uint64_t(tile.y) + tile.size > atlasSize ||
      !std::isfinite(tile.slopeScaleBias) || (tile.maxDrawCount && !indirect))
    return false;
  for (float value : tile.viewProjection)
    if (!std::isfinite(value))
      return false;
  return true;
}
} // namespace

bool cStandardShadowPass::RenderAtlas(
    cGraphics::FrameContext *, RICmd *cmd, uint32_t frameIndex,
    RITexture *atlas, uint32_t layer, uint32_t atlasSize, RIBuffer *indirect,
    RIBuffer *drawCounts, std::span<const Tile> tiles,
    RIProgram::DescriptorBinding frameBinding) {
  if (!m_loaded || !m_program || !cmd || !atlas || atlasSize == 0 ||
      atlasSize > kMaxAtlasSize || layer > 0xffffu)
    return false;
  // A counts buffer is only usable if the device can source a draw count from
  // one; otherwise fall back to issuing every reserved command.
  const bool useDrawIndirectCount =
      drawCounts != nullptr &&
      mpGraphics->device.physicalAdapter.isDrawIndirectCountSupported != 0;
  for (const Tile &tile : tiles)
    if (!ValidTile(tile, atlasSize, indirect))
      return false;
  // Create the attachment view before the first barrier so a failure records
  // nothing against the page.
  RITextureViewDesc vd{};
  vd.viewType = RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT;
  vd.format = kAtlasFormat;
  vd.mipNum = 1;
  vd.layerNum = 1;
  vd.baseLayer = layer;
  RITextureView view = RITextureView::create(&mpGraphics->device, atlas, vd);
  if (view.isEmpty())
    return false;

  RITextureBarrier barrier(atlas, RI_RESOURCE_STATE_UNDEFINED,
                           RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_NONE,
                           RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH);
  barrier.baseLayer = static_cast<uint16_t>(layer);
  barrier.layerCount = 1;
  cmd->vk_d3d12_textureBarrier(barrier);
  RIRenderingAttachment depth{};
  depth.view = view;
  depth.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depth = 1.0f;
  RIBeginRenderingDesc begin{};
  begin.renderArea.width = static_cast<int16_t>(atlasSize);
  begin.renderArea.height = static_cast<int16_t>(atlasSize);
  begin.depthStencil = &depth;
  cmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);

  bool pipelineBound = false;
  float boundSlope = 0.0f;
  for (const Tile &tile : tiles) {
    if (!pipelineBound || tile.slopeScaleBias != boundSlope) {
      ShadowPipelineDesc pd(tile.slopeScaleBias);
      m_program->bindPipeline(&mpGraphics->device, cmd, pd.hash,
                              "Standard.shadow", &pd.create);
      m_program->bindBindlessDescriptorSet(
          cmd, &mpGraphics->globalset->m_bindlessSet, 0);
      // Set 1: gPerFrame, read by the material alpha test's animated-texture lookup.
      m_program->bindDescriptors(&mpGraphics->device, cmd, frameIndex,
                                 &frameBinding, 1);
      pipelineBound = true;
      boundSlope = tile.slopeScaleBias;
    }
    struct Constants {
      float viewProjection[16];
      uint32_t variabilityMask;
    } constants{};
    std::copy(std::begin(tile.viewProjection), std::end(tile.viewProjection),
              constants.viewProjection);
    constants.variabilityMask = tile.variabilityMask;
    cmd->vk_d3d12_setPushConstants(&mpGraphics->device, *m_program, 0,
                                   sizeof(constants), &constants);
    // Negative-height viewport over the tile rect: NDC +Y maps to the tile's
    // top texel row, as the resolve's UV convention expects.
    RIViewport viewport{};
    viewport.x = float(tile.x);
    viewport.y = float(tile.y + tile.size);
    viewport.width = float(tile.size);
    viewport.height = -float(tile.size);
    viewport.depthMin = 0;
    viewport.depthMax = 1;
    cmd->setViewport(&mpGraphics->device, viewport);
    RIRect sc{};
    sc.x = static_cast<int16_t>(tile.x);
    sc.y = static_cast<int16_t>(tile.y);
    sc.width = static_cast<int16_t>(tile.size);
    sc.height = static_cast<int16_t>(tile.size);
    cmd->setScissor(&mpGraphics->device, sc);
    if (tile.maxDrawCount) {
      if (useDrawIndirectCount) {
        cmd->drawIndirectCount(&mpGraphics->device, indirect,
                               tile.indirectOffset, drawCounts,
                               tile.drawCountOffset, tile.maxDrawCount,
                               sizeof(VkDrawIndirectCommand));
      } else {
        // No GPU-sourced count on this device: every candidate is a command
        // and the cull kernel zeroed the instance count of the culled ones.
        cmd->drawIndirect(&mpGraphics->device, indirect, tile.indirectOffset,
                          tile.maxDrawCount, sizeof(VkDrawIndirectCommand));
      }
    }
  }
  cmd->vk_d3d12_endRendering(&mpGraphics->device);
  RITextureBarrier readBarrier(
      atlas, RI_RESOURCE_STATE_DEPTH_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
      RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH);
  readBarrier.baseLayer = static_cast<uint16_t>(layer);
  readBarrier.layerCount = 1;
  cmd->vk_d3d12_textureBarrier(readBarrier);
  // Command recording may outlive this function; the attachment view must
  // remain alive until the submission retires.
  mpGraphics->graphicsDefer.push(
      RISharedPointer<RITextureView>(&mpGraphics->device, view));
  return true;
}
} // namespace hpl
