#include "graphics/TranslucentMeshPipelineDesc.h"

#include "graphics/GraphicsTypes.h" // eVertexElementFlag_*

namespace hpl {

RIVertexInputDesc MakeMeshVertexInputDesc(uint32_t vertexPresentMask) {
  RIVertexInputDesc vi = {};
  // Strides match cVertexBuffer: position/tangent/color stored as float4
  // (16 B), normal as float3 (12 B), texcoord as float3 with only .xy
  // consumed (stride 12 B, RG32 reads the first two floats). An optional
  // stream the renderable omits gets its stride zeroed here.
  vi.bindingCount = 5;
  vi.bindings[0] = {0, 16, RI_VERTEX_INPUT_RATE_VERTEX}; // position (always present)
  vi.bindings[1] = {1, (vertexPresentMask & eVertexElementFlag_Normal)   ? 12u : 0u, RI_VERTEX_INPUT_RATE_VERTEX}; // normal
  vi.bindings[2] = {2, (vertexPresentMask & eVertexElementFlag_Texture1) ? 16u : 0u, RI_VERTEX_INPUT_RATE_VERTEX}; // tangent (w = handedness)
  vi.bindings[3] = {3, (vertexPresentMask & eVertexElementFlag_Color0)   ? 16u : 0u, RI_VERTEX_INPUT_RATE_VERTEX}; // color
  vi.bindings[4] = {4, (vertexPresentMask & eVertexElementFlag_Texture0) ? 12u : 0u, RI_VERTEX_INPUT_RATE_VERTEX}; // texcoord

  vi.attributeCount = 5;
  vi.attributes[0] = {0, 0, RI_FORMAT_RGB32_SFLOAT,  0}; // POSITION
  vi.attributes[1] = {1, 1, RI_FORMAT_RGB32_SFLOAT,  0}; // NORMAL
  vi.attributes[2] = {2, 2, RI_FORMAT_RGBA32_SFLOAT, 0}; // TANGENT
  vi.attributes[3] = {3, 3, RI_FORMAT_RGBA32_SFLOAT, 0}; // COLOR
  vi.attributes[4] = {4, 4, RI_FORMAT_RG32_SFLOAT,   0}; // TEXCOORD0
  return vi;
}

RIGraphicsPipelineDesc MakeTranslucentMeshPipelineDesc(
    RI_Format_e colorFormat, RI_Format_e depthFormat,
    TranslucentMeshPipelineDesc::BlendMode mode, uint32_t vertexPresentMask,
    bool depthTest) {
  RIGraphicsPipelineDesc desc = {};
  desc.vertexInput = MakeMeshVertexInputDesc(vertexPresentMask);
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = RI_CULL_MODE_BACK;
  // CLOCKWISE to match MakeGBufferMRTPipelineDesc -- both raster passes run
  // under the same Y-flipped viewport (negative height), so the same mesh
  // winding must declare the same front face. (Was COUNTER_CLOCKWISE, which
  // inverted the cull and showed glass back-faces.)
  desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;

  desc.depthStencil.depthTest = depthTest;
  desc.depthStencil.depthWrite = false;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;

  desc.renderTarget.colorCount = 1;
  desc.renderTarget.colorFormats[0] = colorFormat;
  desc.renderTarget.depthFormat = depthFormat;

  desc.blendCount = 1;
  RIBlendAttachmentDesc &blend = desc.blend[0];
  blend.blendEnable = true;
  blend.writeMask = RI_COLOR_WRITE_RGBA;
  blend.colorOp = RI_BLEND_OP_ADD;
  blend.alphaOp = RI_BLEND_OP_ADD;
  // The alpha factors below differ from the decal table in MUL/MULX2 -- the
  // two are deliberately not unified.
  switch (mode) {
  case TranslucentMeshPipelineDesc::BLEND_ADD:
    blend.srcColor = RI_BLEND_ONE;
    blend.dstColor = RI_BLEND_ONE;
    blend.srcAlpha = RI_BLEND_ONE;
    blend.dstAlpha = RI_BLEND_ONE;
    break;
  case TranslucentMeshPipelineDesc::BLEND_MUL:
    blend.srcColor = RI_BLEND_ZERO;
    blend.dstColor = RI_BLEND_SRC_COLOR;
    blend.srcAlpha = RI_BLEND_ZERO;
    blend.dstAlpha = RI_BLEND_ONE;
    break;
  case TranslucentMeshPipelineDesc::BLEND_MULX2:
    blend.srcColor = RI_BLEND_DST_COLOR;
    blend.dstColor = RI_BLEND_SRC_COLOR;
    blend.srcAlpha = RI_BLEND_ONE;
    blend.dstAlpha = RI_BLEND_ONE;
    break;
  case TranslucentMeshPipelineDesc::BLEND_ALPHA:
    // Shader premultiplies linear RGB once; opacity is independent of gamma.
  case TranslucentMeshPipelineDesc::BLEND_PREMUL_ALPHA:
    blend.srcColor = RI_BLEND_ONE;
    blend.dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.srcAlpha = RI_BLEND_ONE;
    blend.dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    break;
  case TranslucentMeshPipelineDesc::BLEND_REPLACE:
    blend.blendEnable = false;
    blend.srcColor = RI_BLEND_ONE;
    blend.dstColor = RI_BLEND_ZERO;
    blend.srcAlpha = RI_BLEND_ONE;
    blend.dstAlpha = RI_BLEND_ZERO;
    break;
  default:
    break;
  }
  return desc;
}

} // namespace hpl
