#ifndef HPL_GBUFFER_MRT_PIPELINE_DESC_H
#define HPL_GBUFFER_MRT_PIPELINE_DESC_H

#include "graphics/RIFormat.h"      // RI_Format_e
#include "graphics/RIPipelineDesc.h" // RIGraphicsPipelineDesc

namespace hpl {

// Static pipeline state for the "VBufferRaster.3d" G-buffer pass, shared by
// the standard and hybrid renderers so the two cannot drift apart.
//
// This used to be a non-copyable holder owning a VkGraphicsPipelineCreateInfo
// and every sub-struct its pointers referenced. RIGraphicsPipelineDesc is a
// plain value, so it is now simply returned by value and the lifetime
// contract is gone.

// Hybrid: [0] packed visibility (uint4), [1] velocity (RG16F).
RIGraphicsPipelineDesc MakeGBufferMRTPipelineDesc(RI_Format_e visibilityFormat,
                                                  RI_Format_e velocityFormat,
                                                  RI_Format_e depthFormat);

// Standard: [0] packed visibility, [1] diagnostic colour, [2] velocity.
// Three colour targets, in attachment order. The count must match the
// RIBeginRenderingDesc the pass begins with.
RIGraphicsPipelineDesc MakeGBufferMRTPipelineDesc(RI_Format_e target0Format,
                                                  RI_Format_e target1Format,
                                                  RI_Format_e target2Format,
                                                  RI_Format_e depthFormat);

} // namespace hpl

#endif
