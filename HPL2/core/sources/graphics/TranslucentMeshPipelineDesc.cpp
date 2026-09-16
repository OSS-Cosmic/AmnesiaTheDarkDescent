#include "graphics/TranslucentMeshPipelineDesc.h"

#include "graphics/GraphicsTypes.h" // eVertexElementFlag_*
#include "graphics/RIVK.h"     // RIFormatToVK
#include "system/Hasher.h"     // hash_u32 / HASH_INITIAL_VALUE
#include "system/Types.h"      // ARRAY_COUNT

namespace hpl {

TranslucentMeshPipelineDesc::TranslucentMeshPipelineDesc(
    RI_Format_e colorFormat, RI_Format_e depthFormat, BlendMode mode,
    uint32_t vertexPresentMask, bool depthTest) {
  // Strides match cVertexBuffer: position/tangent/color stored as float4
  // (16 B), normal as float3 (12 B), texcoord as float3 with only .xy
  // consumed (stride 12 B, R32G32 format reads the first two floats). An
  // optional stream the renderable omits (absent from vertexPresentMask) gets
  // its stride zeroed below so the bound single-vertex fallback feeds the same
  // default to every vertex.
  vertexBindings[0] = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX}; // position (always present)
  vertexBindings[1] = {1, (vertexPresentMask & eVertexElementFlag_Normal)   ? 12u : 0u, VK_VERTEX_INPUT_RATE_VERTEX}; // normal
  vertexBindings[2] = {2, (vertexPresentMask & eVertexElementFlag_Texture1) ? 16u : 0u, VK_VERTEX_INPUT_RATE_VERTEX}; // tangent (w = handedness)
  vertexBindings[3] = {3, (vertexPresentMask & eVertexElementFlag_Color0)   ? 16u : 0u, VK_VERTEX_INPUT_RATE_VERTEX}; // color
  vertexBindings[4] = {4, (vertexPresentMask & eVertexElementFlag_Texture0) ? 12u : 0u, VK_VERTEX_INPUT_RATE_VERTEX}; // texcoord
  vertexAttributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0}; // POSITION
  vertexAttributes[1] = {1, 1, VK_FORMAT_R32G32B32_SFLOAT,    0}; // NORMAL
  vertexAttributes[2] = {2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0}; // TANGENT
  vertexAttributes[3] = {3, 3, VK_FORMAT_R32G32B32A32_SFLOAT, 0}; // COLOR
  vertexAttributes[4] = {4, 4, VK_FORMAT_R32G32_SFLOAT,       0}; // TEXCOORD0
  vertexInputState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInputState.vertexBindingDescriptionCount = ARRAY_COUNT(vertexBindings);
  vertexInputState.pVertexBindingDescriptions = vertexBindings;
  vertexInputState.vertexAttributeDescriptionCount = ARRAY_COUNT(vertexAttributes);
  vertexInputState.pVertexAttributeDescriptions = vertexAttributes;

  inputAssemblyState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  rasterizationState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
  // CLOCKWISE to match GBufferMRTPipelineDesc — both raster passes run under
  // the same Y-flipped viewport (negative height), so the same mesh winding
  // must declare the same front face. (Was COUNTER_CLOCKWISE, which inverted
  // the cull and showed glass back-faces.)
  rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizationState.lineWidth = 1.0f;

  dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
  dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
  dynamicState.pDynamicStates = dynamicStates;

  colorFormats[0] = RIFormatToVK(colorFormat);
  pipelineRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  pipelineRendering.colorAttachmentCount = 1;
  pipelineRendering.pColorAttachmentFormats = colorFormats;
  pipelineRendering.depthAttachmentFormat = RIFormatToVK(depthFormat);

  viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  multisampleState = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  depthStencilState = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depthStencilState.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
  depthStencilState.depthWriteEnable = VK_FALSE;
  depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  depthStencilState.minDepthBounds = 0.0f;
  depthStencilState.maxDepthBounds = 1.0f;

  blendAttachment = {};
  blendAttachment.blendEnable = VK_TRUE;
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
  switch (mode) {
  case BLEND_ADD:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    break;
  case BLEND_MUL:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    break;
  case BLEND_MULX2:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    break;
  case BLEND_ALPHA:
    // Shader premultiplies linear RGB once; opacity is independent of gamma.
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    break;
  case BLEND_PREMUL_ALPHA:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    break;
  case BLEND_REPLACE:
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    break;
  default:
    break;
  }
  colorBlendState = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlendState.attachmentCount = 1;
  colorBlendState.pAttachments = &blendAttachment;

  createInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  createInfo.pNext = &pipelineRendering;
  createInfo.pVertexInputState = &vertexInputState;
  createInfo.pInputAssemblyState = &inputAssemblyState;
  createInfo.pRasterizationState = &rasterizationState;
  createInfo.pDynamicState = &dynamicState;
  createInfo.pViewportState = &viewportState;
  createInfo.pMultisampleState = &multisampleState;
  createInfo.pDepthStencilState = &depthStencilState;
  createInfo.pColorBlendState = &colorBlendState;

  // Pipeline cache lives on the program (RIProgram::PipelineSlot), and
  // m_translucentMesh is a separate program from m_particle — collisions on
  // (colorFormat, depthFormat, mode) across the two programs are impossible.
  // Same seed shape as ParticlePipelineDesc is fine.
  hash = hash_u32(HASH_INITIAL_VALUE, (uint32_t)colorFormat);
  hash = hash_u32(hash, (uint32_t)depthFormat);
  hash = hash_u32(hash, (uint32_t)mode);
  // Distinct vertex-binding strides per presence combination must not alias in
  // the program's pipeline cache.
  hash = hash_u32(hash, vertexPresentMask);
  // A no-depth-test halo must not reuse a depth-tested material's pipeline.
  hash = hash_u32(hash, depthTest ? 1u : 0u);
}

} // namespace hpl
