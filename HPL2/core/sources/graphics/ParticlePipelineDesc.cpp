#include "graphics/ParticlePipelineDesc.h"

#include "graphics/RIVK.h"     // RIFormatToVK
#include "system/Hasher.h"     // hash_u32 / HASH_INITIAL_VALUE
#include "system/Types.h"      // ARRAY_COUNT

namespace hpl {

ParticlePipelineDesc::ParticlePipelineDesc(RI_Format_e swapchainFormat,
                                           RI_Format_e depthFormat,
                                           BlendMode mode, bool depthTest) {
  vertexInputState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInputState.vertexBindingDescriptionCount = 0;
  vertexInputState.pVertexBindingDescriptions = nullptr;
  vertexInputState.vertexAttributeDescriptionCount = 0;
  vertexInputState.pVertexAttributeDescriptions = nullptr;

  inputAssemblyState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  rasterizationState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizationState.cullMode = VK_CULL_MODE_NONE;
  rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizationState.lineWidth = 1.0f;

  dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
  dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
  dynamicState.pDynamicStates = dynamicStates;

  colorFormats[0] = RIFormatToVK(swapchainFormat);
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

  // Match the legacy translucencyBlendTable mapping
  // (RendererDeferred.cpp:3948-3954). The FS applies per-pixel color
  // transforms that prepare its output for these hardware factors.
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

  hash = hash_u32(HASH_INITIAL_VALUE, swapchainFormat);
  hash = hash_u32(hash, depthFormat);
  hash = hash_u32(hash, (uint32_t)mode);
  // A no-depth-test sprite must not reuse a depth-tested pipeline.
  hash = hash_u32(hash, depthTest ? 1u : 0u);
}

} // namespace hpl
