#include "graphics/WaterGuidePipelineDesc.h"

#include "graphics/TranslucentMeshPipelineDesc.h"

namespace hpl {

RIGraphicsPipelineDesc MakeWaterGuidePipelineDesc(
    RI_Format_e positionViewZFormat, RI_Format_e normalWeightFormat,
    RI_Format_e velocityFormat, RI_Format_e depthFormat,
    uint32_t vertexPresentMask) {
  // Borrow the translucent mesh coverage state so vertex layout, raster, depth
  // test and sampling match the final water draw. Its blend mode and single
  // colour format are placeholders -- both are replaced below.
  RIGraphicsPipelineDesc desc = MakeTranslucentMeshPipelineDesc(
      positionViewZFormat, depthFormat, TranslucentMeshPipelineDesc::BLEND_ADD,
      vertexPresentMask);

  desc.renderTarget.colorCount = 3;
  desc.renderTarget.colorFormats[0] = positionViewZFormat;
  desc.renderTarget.colorFormats[1] = normalWeightFormat;
  desc.renderTarget.colorFormats[2] = velocityFormat;
  desc.renderTarget.depthFormat = depthFormat;

  // The guide writes raw values to all three MRTs. A default-constructed
  // RIBlendAttachmentDesc is exactly that: blending off, ONE/ZERO, write RGBA.
  desc.blendCount = 3;
  for (uint32_t i = 0; i < desc.blendCount; ++i)
    desc.blend[i] = RIBlendAttachmentDesc{};

  return desc;
}

} // namespace hpl
