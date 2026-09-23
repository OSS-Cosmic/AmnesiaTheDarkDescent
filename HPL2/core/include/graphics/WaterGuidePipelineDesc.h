#ifndef HPL_WATER_GUIDE_PIPELINE_DESC_H
#define HPL_WATER_GUIDE_PIPELINE_DESC_H

#include "graphics/RIFormat.h"       // RI_Format_e
#include "graphics/RIPipelineDesc.h" // RIGraphicsPipelineDesc

#include <cstdint>

namespace hpl {

// Pipeline for the water reflection-guide raster pass. The guide writes three
// unblended MRTs and borrows the translucent mesh coverage state, so its vertex
// layout, rasterization and depth test match the final water draw.
//
// `vertexPresentMask` names the optional streams the renderable supplies; see
// MakeMeshVertexInputDesc in TranslucentMeshPipelineDesc.h.
RIGraphicsPipelineDesc MakeWaterGuidePipelineDesc(
    RI_Format_e positionViewZFormat, RI_Format_e normalWeightFormat,
    RI_Format_e velocityFormat, RI_Format_e depthFormat,
    uint32_t vertexPresentMask);

} // namespace hpl

#endif
