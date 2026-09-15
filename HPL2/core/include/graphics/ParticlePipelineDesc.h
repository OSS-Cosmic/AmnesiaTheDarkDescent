#ifndef HPL_PARTICLE_PIPELINE_DESC_H
#define HPL_PARTICLE_PIPELINE_DESC_H

#include "graphics/RITypes.h"   // RI_Format_e

#include "graphics/RIPreamble.h"

#include "system/Hasher.h"      // hash_t

#include <cstdint>

namespace hpl {

// Pipeline descriptor for the particle (translucent) pass. One instance per
// blend mode — the hardware blend factors come from the legacy
// translucencyBlendTable mapping in RendererDeferred. Depth test is on but
// depth write is off so particles sort against opaque geometry without
// occluding each other in the wrong order. Cull mode is NONE because
// particle billboards may face the camera either way; legacy renderer
// behaves the same. No vertex input bindings — VS pulls via BDA from
// opaque*Handles[].
struct ParticlePipelineDesc {
  VkPipelineVertexInputStateCreateInfo vertexInputState;
  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState;
  VkPipelineRasterizationStateCreateInfo rasterizationState;
  VkDynamicState dynamicStates[2];
  VkPipelineDynamicStateCreateInfo dynamicState;
  VkFormat colorFormats[1];
  VkPipelineRenderingCreateInfo pipelineRendering;
  VkPipelineViewportStateCreateInfo viewportState;
  VkPipelineMultisampleStateCreateInfo multisampleState;
  VkPipelineDepthStencilStateCreateInfo depthStencilState;
  VkPipelineColorBlendAttachmentState blendAttachment;
  VkPipelineColorBlendStateCreateInfo colorBlendState;
  VkGraphicsPipelineCreateInfo createInfo;
  hash_t hash;

  enum BlendMode : uint32_t {
    BLEND_ADD = 0,
    BLEND_MUL = 1,
    BLEND_MULX2 = 2,
    BLEND_ALPHA = 3,
    BLEND_PREMUL_ALPHA = 4,
    BLEND_LAST_ENUM = 5,
  };

  // depthTest=false mirrors materials with DepthTest="False" (torch halos,
  // nodepth fog sprites): the sprite ignores scene depth entirely.
  ParticlePipelineDesc(RI_Format_e swapchainFormat, RI_Format_e depthFormat,
                       BlendMode mode, bool depthTest = true);

  ParticlePipelineDesc(const ParticlePipelineDesc &) = delete;
  ParticlePipelineDesc &operator=(const ParticlePipelineDesc &) = delete;
};

} // namespace hpl

#endif
