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
  auto vertBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                            "Standard.shadow.3d", "vsMain");
  auto fragBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                            "Standard.shadow.3d", "psMain");
  if (vertBin.empty() || fragBin.empty())
    return false;
  const RIBindlessLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.layout()};
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vertBin, "vsMain"},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fragBin, "psMain"}};
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
// Depth-only shadow render: the VS pulls vertex data through the bindless set,
// so there is no vertex input, and nothing is written to colour -- colorCount
// and blendCount both stay 0 (the desc asserts they agree).
RIGraphicsPipelineDesc MakeShadowPipelineDesc(float slopeScaleBias) {
  RIGraphicsPipelineDesc desc = {};
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = RI_CULL_MODE_BACK;
  desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;
  desc.raster.lineWidth = 1.0f;
  // Static depth bias, slope-scaled only (no constant term).
  desc.raster.depthBiasEnable = slopeScaleBias != 0.0f;
  desc.raster.depthBiasSlope = slopeScaleBias;
  desc.depthStencil.depthTest = true;
  desc.depthStencil.depthWrite = true;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;
  desc.renderTarget.colorCount = 0;
  desc.blendCount = 0;
  desc.renderTarget.depthFormat = cStandardShadowPass::kAtlasFormat;
  return desc;
}

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
      // slopeScaleBias and the atlas format are both hashed structurally
      // (hash_f32 on depthBiasSlope / the depth format), so the variant hash
      // no longer has to fold them in.
      const RIGraphicsPipelineDesc pd =
          MakeShadowPipelineDesc(tile.slopeScaleBias);
      m_program->bindPipeline(&mpGraphics->device, cmd, HASH_INITIAL_VALUE,
                              "Standard.shadow", pd);
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
