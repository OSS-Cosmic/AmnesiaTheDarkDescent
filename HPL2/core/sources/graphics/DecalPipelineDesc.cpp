#include "graphics/DecalPipelineDesc.h"

#include "graphics/TranslucentMeshPipelineDesc.h" // MakeMeshVertexInputDesc

namespace hpl {

RIGraphicsPipelineDesc MakeDecalPipelineDesc(RI_Format_e colorFormat,
                                             RI_Format_e depthFormat,
                                             DecalPipelineDesc::BlendMode mode,
                                             uint32_t vertexPresentMask) {
  RIGraphicsPipelineDesc desc = {};
  // Decal.vert.slang reuses the translucent 5-stream layout; normal and
  // tangent are bound but ignored by the shader.
  desc.vertexInput = MakeMeshVertexInputDesc(vertexPresentMask);
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  // NONE: decals are thin clipped meshes with arbitrary winding -- draw both
  // faces so they never vanish. frontFace stays CLOCKWISE to match the
  // GBuffer/translucent passes under the same Y-flipped viewport (irrelevant
  // while culling is off, but kept consistent if it is later tightened).
  desc.raster.cullMode = RI_CULL_MODE_NONE;
  desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;

  desc.depthStencil.depthTest = true;
  desc.depthStencil.depthWrite = false;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;

  desc.renderTarget.colorCount = 1;
  desc.renderTarget.colorFormats[0] = colorFormat;
  desc.renderTarget.depthFormat = depthFormat;

  desc.blendCount = 1;
  RIBlendAttachmentDesc &blend = desc.blend[0];
  blend.blendEnable = true;
  // RGB only -- the decal pass renders into the decalMul / decalAdd
  // accumulators, whose RGB the composite reads
  // (albedo = albedo*decalMul + decalAdd). Alpha is unused; leave it at the
  // clear identity rather than accumulating it.
  blend.writeMask = RI_COLOR_WRITE_RGB;
  blend.colorOp = RI_BLEND_OP_ADD;
  blend.alphaOp = RI_BLEND_OP_ADD;
  // MUL/MULX2 use SRC_ALPHA / DST_ALPHA where the translucent table uses ONE.
  // The two are deliberately not unified.
  switch (mode) {
  case DecalPipelineDesc::BLEND_ADD:
    blend.srcColor = RI_BLEND_ONE;
    blend.dstColor = RI_BLEND_ONE;
    blend.srcAlpha = RI_BLEND_ONE;
    blend.dstAlpha = RI_BLEND_ONE;
    break;
  case DecalPipelineDesc::BLEND_MUL:
    blend.srcColor = RI_BLEND_ZERO;
    blend.dstColor = RI_BLEND_SRC_COLOR;
    blend.srcAlpha = RI_BLEND_ZERO;
    blend.dstAlpha = RI_BLEND_SRC_ALPHA;
    break;
  case DecalPipelineDesc::BLEND_MULX2:
    blend.srcColor = RI_BLEND_DST_COLOR;
    blend.dstColor = RI_BLEND_SRC_COLOR;
    blend.srcAlpha = RI_BLEND_DST_ALPHA;
    blend.dstAlpha = RI_BLEND_SRC_ALPHA;
    break;
  case DecalPipelineDesc::BLEND_ALPHA:
    // Premultiplied: the shader outputs rgb*pow(a,kPerceptualBlendExp) and
    // a = 1-pow(1-a,k) so the powered weights approximate the legacy
    // display-space lerp in the linear HDR target (see Decal.frag.slang).
    // Bit-identical to PREMUL_ALPHA; the difference is shader-side, so the
    // two now share one cached pipeline instead of two identical ones.
  case DecalPipelineDesc::BLEND_PREMUL_ALPHA:
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
