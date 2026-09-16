#ifndef HPL_TRANSLUCENT_MESH_PIPELINE_DESC_H
#define HPL_TRANSLUCENT_MESH_PIPELINE_DESC_H

#include "graphics/RITypes.h"   // RI_Format_e

#include "graphics/RIPreamble.h"

#include "system/Hasher.h"      // hash_t

#include <cstdint>

namespace hpl {

// Pipeline for the non-particle translucent (mesh) pass. Same shape as
// ParticlePipelineDesc, with two deliberate divergences:
//   - cullMode is BACK_BIT: meshes have orientation, unlike particle
//     billboards (which use NONE so either face draws).
//   - frontFace is CLOCKWISE to match GBufferMRTPipelineDesc: both raster
//     passes run under the same Y-flipped viewport (negative height), so the
//     same mesh winding must declare the same front face across the opaque +
//     translucent passes.
// Depth is read-only ≤ unless the material disables testing (e.g. hand-held
// lantern halos). Hardware blend per BlendMode is identical to the particle
// desc; the same fragment-side pre-bias is applied
// by Translucent.frag.slang. Unlike the particle/opaque pipelines this one
// uses traditional fixed-function vertex bindings (see Translucent.vert.slang).
struct TranslucentMeshPipelineDesc {
  // Vertex bindings + attributes for the translucent raster path. Unlike
  // the opaque / particle pipelines (which pull vertex data via BDA out of
  // the bindless OBJECT-slot handle arrays), this pipeline binds vertex
  // buffers the conventional Vulkan way. The bindless slot still carries
  // the per-object UniformObject (modelMat / uvMat / materialID); only the
  // per-vertex streams move to fixed-function fetch. See Translucent.vert.slang.
  //
  // Order + formats MUST match the input struct in Translucent.vert.slang.
  VkVertexInputBindingDescription   vertexBindings[5];
  VkVertexInputAttributeDescription vertexAttributes[5];
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

  // Same enum layout as ParticlePipelineDesc::BlendMode so the
  // eMaterialBlendMode -> pipeline-variant remap from the particle path is
  // directly reusable.
  enum BlendMode : uint32_t {
    BLEND_ADD = 0,
    BLEND_MUL = 1,
    BLEND_MULX2 = 2,
    BLEND_ALPHA = 3,
    BLEND_PREMUL_ALPHA = 4,
    // Blend disabled: refractive draws compose against the scene copy in the
    // shader, as the legacy renderer did with eMaterialBlendMode_None.
    BLEND_REPLACE = 5,
    BLEND_LAST_ENUM = 6,
  };

  // vertexPresentMask is a bitset of eVertexElementFlag_* naming the optional
  // streams (Normal / Texture1 tangent / Color0 / Texture0) the renderable
  // actually supplies. Absent streams get their binding stride zeroed so a
  // single-vertex fallback buffer (cGraphics::fallback*Vertex) reads its one default
  // for every vertex; the mask folds into `hash` so RIProgram caches a distinct
  // pipeline per presence combination. Position is always present.
  TranslucentMeshPipelineDesc(RI_Format_e colorFormat, RI_Format_e depthFormat,
                              BlendMode mode, uint32_t vertexPresentMask,
                              bool depthTest = true);

  TranslucentMeshPipelineDesc(const TranslucentMeshPipelineDesc &) = delete;
  TranslucentMeshPipelineDesc &operator=(const TranslucentMeshPipelineDesc &) = delete;
};

} // namespace hpl

#endif
