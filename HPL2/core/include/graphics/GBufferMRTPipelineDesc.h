#ifndef HPL_GBUFFER_MRT_PIPELINE_DESC_H
#define HPL_GBUFFER_MRT_PIPELINE_DESC_H

#include "graphics/RITypes.h"   // RI_Format_e

#include "graphics/RIPreamble.h"

#include "system/Hasher.h"      // hash_t

namespace hpl {

// Holder for the static portion of the "VBufferRaster.3d"
// VkGraphicsPipelineCreateInfo. Owns every sub-struct so the pointer
// chain stays valid as long as the holder lives. Non-copyable /
// non-movable - the pNext / pXxxState pointers would dangle.
struct GBufferMRTPipelineDesc {
  VkPipelineVertexInputStateCreateInfo vertexInputState;
  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState;
  VkPipelineRasterizationStateCreateInfo rasterizationState;
  VkDynamicState dynamicStates[2];
  VkPipelineDynamicStateCreateInfo dynamicState;
  // Hybrid: [0] packed visibility (uint4), [1] velocity (RG16F).
  // Standard: [0] packed visibility, [1] diagnostic colour, [2] velocity.
  VkFormat colorFormats[3];
  VkPipelineRenderingCreateInfo pipelineRendering;
  VkPipelineViewportStateCreateInfo viewportState;
  VkPipelineMultisampleStateCreateInfo multisampleState;
  VkPipelineDepthStencilStateCreateInfo depthStencilState;
  VkPipelineColorBlendAttachmentState blendAttachments[3];
  VkPipelineColorBlendStateCreateInfo colorBlendState;
  VkGraphicsPipelineCreateInfo createInfo;
  hash_t hash;

  GBufferMRTPipelineDesc(RI_Format_e visibilityFormat, RI_Format_e velocityFormat,
                         RI_Format_e depthFormat);
  // Three colour targets, in attachment order. The count must match the
  // VkRenderingInfo the pass begins with.
  GBufferMRTPipelineDesc(RI_Format_e target0Format, RI_Format_e target1Format,
                         RI_Format_e target2Format, RI_Format_e depthFormat);

  GBufferMRTPipelineDesc(const GBufferMRTPipelineDesc &) = delete;
  GBufferMRTPipelineDesc &operator=(const GBufferMRTPipelineDesc &) = delete;

private:
  void Init(const RI_Format_e *colorFormatList, uint32_t colorCount,
            RI_Format_e depthFormat);
};

} // namespace hpl

#endif
