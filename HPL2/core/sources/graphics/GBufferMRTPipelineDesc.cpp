#include "graphics/GBufferMRTPipelineDesc.h"

#include "graphics/RIVK.h" // RIFormatToVK
#include "system/Hasher.h" // hash_u32 / HASH_INITIAL_VALUE
#include "system/Types.h"  // ARRAY_COUNT

namespace hpl {

GBufferMRTPipelineDesc::GBufferMRTPipelineDesc(RI_Format_e visibilityFormat,
                                               RI_Format_e velocityFormat,
                                               RI_Format_e depthFormat) {
  const RI_Format_e formats[2] = {visibilityFormat, velocityFormat};
  Init(formats, 2, depthFormat);
}

GBufferMRTPipelineDesc::GBufferMRTPipelineDesc(RI_Format_e target0Format,
                                               RI_Format_e target1Format,
                                               RI_Format_e target2Format,
                                               RI_Format_e depthFormat) {
  const RI_Format_e formats[3] = {target0Format, target1Format, target2Format};
  Init(formats, 3, depthFormat);
}

void GBufferMRTPipelineDesc::Init(const RI_Format_e *colorFormatList,
                                  uint32_t colorCount,
                                  RI_Format_e depthFormat) {
  // VS pulls all per-vertex data via buffer_reference from set 0 SSBOs,
  // so the pipeline declares zero vertex input bindings and attributes.
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
  rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizationState.lineWidth = 1.0f;

  dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
  dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
  dynamicState.pDynamicStates = dynamicStates;

  // SV_TARGET0 is the packed TriangleHit (uint4); the rest follow in order.
  for (uint32_t i = 0; i < colorCount; ++i)
    colorFormats[i] = RIFormatToVK(colorFormatList[i]);
  pipelineRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  pipelineRendering.colorAttachmentCount = colorCount;
  pipelineRendering.pColorAttachmentFormats = colorFormats;
  pipelineRendering.depthAttachmentFormat = RIFormatToVK(depthFormat);

  viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  multisampleState = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  depthStencilState = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depthStencilState.depthTestEnable = VK_TRUE;
  depthStencilState.depthWriteEnable = VK_TRUE;
  depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  depthStencilState.minDepthBounds = 0.0f;
  depthStencilState.maxDepthBounds = 1.0f;

  // Every target writes raw (blendEnable VK_FALSE) — the uint visibility target
  // can't blend, and velocity wants the exact value. Factors stay identity.
  const VkPipelineColorBlendAttachmentState noBlend = {
      VK_FALSE,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD,
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
  for (uint32_t i = 0; i < colorCount; ++i)
    blendAttachments[i] = noBlend;
  colorBlendState = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlendState.attachmentCount = colorCount;
  colorBlendState.pAttachments = blendAttachments;

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

  hash = hash_u32(HASH_INITIAL_VALUE, colorCount);
  for (uint32_t i = 0; i < colorCount; ++i)
    hash = hash_u32(hash, colorFormatList[i]);
  hash = hash_u32(hash, depthFormat);
}

} // namespace hpl
