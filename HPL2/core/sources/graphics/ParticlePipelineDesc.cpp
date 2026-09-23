#include "graphics/ParticlePipelineDesc.h"

namespace hpl {

RIGraphicsPipelineDesc
MakeParticlePipelineDesc(RI_Format_e swapchainFormat, RI_Format_e depthFormat,
                         ParticlePipelineDesc::BlendMode mode, bool depthTest) {
  RIGraphicsPipelineDesc desc = {};
  // No vertex input: the VS pulls per-vertex data via BDA.
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = RI_CULL_MODE_NONE;
  desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;

  desc.depthStencil.depthTest = depthTest;
  desc.depthStencil.depthWrite = false;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;

  desc.renderTarget.colorCount = 1;
  desc.renderTarget.colorFormats[0] = swapchainFormat;
  desc.renderTarget.depthFormat = depthFormat;

  desc.blendCount = 1;
  RIBlendAttachmentDesc &blend = desc.blend[0];
  blend.blendEnable = true;
  blend.writeMask = RI_COLOR_WRITE_RGBA;
  blend.colorOp = RI_BLEND_OP_ADD;
  blend.alphaOp = RI_BLEND_OP_ADD;
  switch (mode) {
  case ParticlePipelineDesc::BLEND_ADD:
    blend.srcColor = RI_BLEND_ONE;
    blend.dstColor = RI_BLEND_ONE;
    blend.srcAlpha = RI_BLEND_ONE;
    blend.dstAlpha = RI_BLEND_ONE;
    break;
  case ParticlePipelineDesc::BLEND_MUL:
    blend.srcColor = RI_BLEND_ZERO;
    blend.dstColor = RI_BLEND_SRC_COLOR;
    blend.srcAlpha = RI_BLEND_ZERO;
    blend.dstAlpha = RI_BLEND_ONE;
    break;
  case ParticlePipelineDesc::BLEND_MULX2:
    blend.srcColor = RI_BLEND_DST_COLOR;
    blend.dstColor = RI_BLEND_SRC_COLOR;
    blend.srcAlpha = RI_BLEND_ONE;
    blend.dstAlpha = RI_BLEND_ONE;
    break;
  case ParticlePipelineDesc::BLEND_ALPHA:
  case ParticlePipelineDesc::BLEND_PREMUL_ALPHA:
    blend.srcColor = RI_BLEND_ONE;
    blend.dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.srcAlpha = RI_BLEND_ONE;
    blend.dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    break;
  default:
    break;
  }
  return desc;
}

} // namespace hpl
