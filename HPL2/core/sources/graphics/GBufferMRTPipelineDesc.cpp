#include "graphics/GBufferMRTPipelineDesc.h"

namespace hpl {

namespace {

RIGraphicsPipelineDesc Make(const RI_Format_e *colorFormatList,
                            uint32_t colorCount, RI_Format_e depthFormat) {
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
  desc.renderTarget.colorCount = colorCount;
  desc.blendCount = colorCount;
  for (uint32_t i = 0; i < colorCount; ++i)
    desc.renderTarget.colorFormats[i] = colorFormatList[i];
  desc.renderTarget.depthFormat = depthFormat;

  return desc;
}

} // namespace

RIGraphicsPipelineDesc MakeGBufferMRTPipelineDesc(RI_Format_e visibilityFormat,
                                                  RI_Format_e velocityFormat,
                                                  RI_Format_e depthFormat) {
  const RI_Format_e formats[2] = {visibilityFormat, velocityFormat};
  return Make(formats, 2, depthFormat);
}

RIGraphicsPipelineDesc MakeGBufferMRTPipelineDesc(RI_Format_e target0Format,
                                                  RI_Format_e target1Format,
                                                  RI_Format_e target2Format,
                                                  RI_Format_e depthFormat) {
  const RI_Format_e formats[3] = {target0Format, target1Format, target2Format};
  return Make(formats, 3, depthFormat);
}

} // namespace hpl
