#include "graphics/GBufferMRTPipelineDesc.h"

namespace hpl {

RIGraphicsPipelineDesc MakeGBufferMRTPipelineDesc(RI_Format_e visibilityFormat,
                                                  RI_Format_e velocityFormat,
                                                  RI_Format_e depthFormat) {
  RIGraphicsPipelineDesc desc = {};
  // VS pulls all per-vertex data via buffer_reference from set 0 SSBOs, so the
  // pipeline declares zero vertex input bindings and attributes.
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = RI_CULL_MODE_BACK;
  desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;

  desc.depthStencil.depthTest = true;
  desc.depthStencil.depthWrite = true;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;

  // SV_TARGET0 is the packed TriangleHit (uint4); the rest follow in order.
  // Every target writes raw (no blending) -- the uint visibility target cannot
  // blend, and velocity wants the exact value.
  desc.renderTarget.colorCount = 2;
  desc.blendCount = 2;
  desc.renderTarget.colorFormats[0] = visibilityFormat;
  desc.renderTarget.colorFormats[1] = velocityFormat;
  desc.renderTarget.depthFormat = depthFormat;

  return desc;
}

RIGraphicsPipelineDesc MakeGBufferMRTPipelineDesc(RI_Format_e target0Format,
                                                  RI_Format_e target1Format,
                                                  RI_Format_e target2Format,
                                                  RI_Format_e depthFormat) {
  RIGraphicsPipelineDesc desc = {};
  // VS pulls all per-vertex data via buffer_reference from set 0 SSBOs, so the
  // pipeline declares zero vertex input bindings and attributes.
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = RI_CULL_MODE_BACK;
  desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;

  desc.depthStencil.depthTest = true;
  desc.depthStencil.depthWrite = true;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;

  // SV_TARGET0 is the packed TriangleHit (uint4); the rest follow in order.
  // Every target writes raw (no blending) -- the uint visibility target cannot
  // blend, and velocity wants the exact value.
  desc.renderTarget.colorCount = 3;
  desc.blendCount = 3;
  desc.renderTarget.colorFormats[0] = target0Format;
  desc.renderTarget.colorFormats[1] = target1Format;
  desc.renderTarget.colorFormats[2] = target2Format;
  desc.renderTarget.depthFormat = depthFormat;

  return desc;
}

} // namespace hpl
