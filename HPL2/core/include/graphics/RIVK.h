#ifndef RI_VK_H
#define RI_VK_H

// Vulkan translation helpers: the ri_vk_RI*ToVK / RI_VK_* inline mappings from
// RI vocabulary onto Vulkan enums. RID3D12.h holds the symmetric D3D12 set.

#include "RITypes.h"
#include "RIFormat.h"
#include "graphics/GraphicsTypes.h"
#include <cassert>
#include <vulkan/vulkan_core.h>

#if DEVICE_IMPL_VULKAN
#define RI_VK_DESCRIPTOR_IS_IMAGE(desc)                                        \
  ((desc).type == RI_DESCRIPTOR_TYPE_SAMPLER ||                                \
   (desc).type == RI_DESCRIPTOR_TYPE_STORAGE_IMAGE ||                          \
   (desc).type == RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE)

namespace hpl {
static inline VkSamplerAddressMode RI_VK_TextureWrap(eTextureWrap wrap) {
  switch (wrap) {
  case eTextureWrap_Repeat:
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  case eTextureWrap_Clamp:
  case eTextureWrap_ClampToEdge:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case eTextureWrap_ClampToBorder:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  case eTextureWrap_LastEnum:
    break;
  }
  assert(false);
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}
} // namespace hpl

static inline void RI_VK_FillColorAttachment(VkRenderingAttachmentInfo *info,
                                             struct RIDescriptor *desc,
                                             bool attachAndClear) {
  info->imageView = desc->vkImageView();
  info->imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  info->resolveMode = VK_RESOLVE_MODE_NONE;
  info->resolveImageView = VK_NULL_HANDLE;
  info->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info->loadOp =
      attachAndClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  info->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
}

static inline void
RI_VK_FillColorAttachmentView(VkRenderingAttachmentInfo *info,
                              struct RITextureView *view, bool attachAndClear) {
  info->imageView = view->vk.image;
  info->imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  info->resolveMode = VK_RESOLVE_MODE_NONE;
  info->resolveImageView = VK_NULL_HANDLE;
  info->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info->loadOp =
      attachAndClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  info->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
}

static inline void RI_VK_FillDepthAttachment(VkRenderingAttachmentInfo *info,
                                             struct RITextureView *view,
                                             bool attachAndClear) {
  info->imageView = view->vk.image;
  info->imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  info->resolveMode = VK_RESOLVE_MODE_NONE;
  info->resolveImageView = VK_NULL_HANDLE;
  info->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info->loadOp =
      attachAndClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  info->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  info->clearValue.depthStencil.depth = 1.0f;
}

const VkFormat RIFormatToVK(uint32_t format);
const enum RI_Format_e VKToRIFormat(VkFormat);

static inline VkImageViewType
ri_vk_RITextureViewTypeToVK(enum RITextureViewType_e v) {
  switch (v) {
  case RI_VIEWTYPE_SHADER_RESOURCE_1D:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D:
    return VK_IMAGE_VIEW_TYPE_1D;
  case RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY:
    return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
  case RI_VIEWTYPE_SHADER_RESOURCE_2D:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D:
  case RI_VIEWTYPE_COLOR_ATTACHMENT:
  case RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT:
  case RI_VIEWTYPE_DEPTH_READONLY_STENCIL_ATTACHMENT:
  case RI_VIEWTYPE_DEPTH_ATTACHMENT_STENCIL_READONLY:
  case RI_VIEWTYPE_DEPTH_STENCIL_READONLY:
  case RI_VIEWTYPE_SHADING_RATE_ATTACHMENT:
    return VK_IMAGE_VIEW_TYPE_2D;
  case RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY:
    return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  case RI_VIEWTYPE_SHADER_RESOURCE_CUBE:
    return VK_IMAGE_VIEW_TYPE_CUBE;
  case RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY:
    return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
  case RI_VIEWTYPE_SHADER_RESOURCE_3D:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D:
    return VK_IMAGE_VIEW_TYPE_3D;
  }
  assert(false);
  return VK_IMAGE_VIEW_TYPE_2D;
}

static inline VkImageAspectFlags
ri_vk_RITextureViewAspect(enum RITextureViewType_e v) {
  switch (v) {
  case RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT:
  case RI_VIEWTYPE_DEPTH_READONLY_STENCIL_ATTACHMENT:
  case RI_VIEWTYPE_DEPTH_ATTACHMENT_STENCIL_READONLY:
  case RI_VIEWTYPE_DEPTH_STENCIL_READONLY:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  default:
    return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

static inline VkAccelerationStructureTypeKHR
RI_VK_AccelStructureType(enum RIAccelStructureType_e type) {
  switch (type) {
  case RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL:
    return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  case RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL:
    return VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  }
  assert(false);
  return VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
}

static inline VkBuildAccelerationStructureFlagsKHR
RI_VK_AccelBuildFlags(uint32_t flags) {
  VkBuildAccelerationStructureFlagsKHR out = 0;
  if (flags & RI_ACCEL_BUILD_ALLOW_UPDATE)
    out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
  if (flags & RI_ACCEL_BUILD_ALLOW_COMPACTION)
    out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
  if (flags & RI_ACCEL_BUILD_ALLOW_DATA_ACCESS)
    out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_KHR;
  if (flags & RI_ACCEL_BUILD_PREFER_FAST_TRACE)
    out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  if (flags & RI_ACCEL_BUILD_PREFER_FAST_BUILD)
    out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  if (flags & RI_ACCEL_BUILD_MINIMIZE_MEMORY)
    out |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
  return out;
}

static inline VkGeometryFlagsKHR RI_VK_AccelGeometryFlags(uint32_t flags) {
  VkGeometryFlagsKHR out = 0;
  if (flags & RI_ACCEL_GEOMETRY_OPAQUE)
    out |= VK_GEOMETRY_OPAQUE_BIT_KHR;
  if (flags & RI_ACCEL_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION)
    out |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
  return out;
}

static inline VkGeometryInstanceFlagsKHR
RI_VK_AccelInstanceFlags(uint32_t flags) {
  VkGeometryInstanceFlagsKHR out = 0;
  if (flags & RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE)
    out |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
  if (flags & RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING)
    out |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
  if (flags & RI_ACCEL_INSTANCE_FORCE_OPAQUE)
    out |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
  if (flags & RI_ACCEL_INSTANCE_FORCE_NON_OPAQUE)
    out |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
  return out;
}

static inline VkBufferUsageFlags ri_vk_RIBufferUsageToVK(uint32_t usage) {
  VkBufferUsageFlags out = 0;
  if (usage & RI_BUFFER_USAGE_SHADER_RESOURCE)
    out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (usage & RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE)
    out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (usage & RI_BUFFER_USAGE_VERTEX_BUFFER)
    out |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (usage & RI_BUFFER_USAGE_INDEX_BUFFER)
    out |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (usage & RI_BUFFER_USAGE_CONSTANT_BUFFER)
    out |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (usage & RI_BUFFER_USAGE_ARGUMENT_BUFFER)
    out |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (usage & RI_BUFFER_USAGE_SCRATCH)
    out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (usage & RI_BUFFER_USAGE_BINDING_TABLE)
    out |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
  if (usage & RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT)
    out |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  if (usage & RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE)
    out |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
  if (usage & RI_BUFFER_USAGE_TRANSFER_SRC)
    out |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (usage & RI_BUFFER_USAGE_TRANSFER_DST)
    out |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (usage & RI_BUFFER_USAGE_INDIRECT)
    out |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (usage & RI_BUFFER_USAGE_DEVICE_ADDRESS)
    out |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  return out;
}

static inline VkImageUsageFlags ri_vk_RITextureUsageToVK(uint32_t usage) {
  VkImageUsageFlags out = 0;
  if (usage & RI_USAGE_SHADER_RESOURCE)
    out |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (usage & RI_USAGE_SHADER_RESOURCE_STORAGE)
    out |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (usage & RI_USAGE_COLOR_ATTACHMENT)
    out |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (usage & RI_USAGE_DEPTH_STENCIL_ATTACHMENT)
    out |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (usage & RI_USAGE_SHADING_RATE)
    out |= VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
  if (usage & RI_USAGE_TRANSFER_SRC)
    out |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (usage & RI_USAGE_TRANSFER_DST)
    out |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  return out;
}

static inline VkRect2D RIToVKRect2D(const struct RIRect *in) {
  VkRect2D out = {};
  out.extent.width = in->width;
  out.extent.height = in->height;
  out.offset.x = in->x;
  out.offset.y = in->y;
  return out;
}

static inline VkRect2D RIViewportToRect2D(struct RIViewport *in) {
  VkRect2D out;
  out.extent.width = static_cast<uint32_t>(in->width);
  out.extent.height = static_cast<uint32_t>(in->height);
  out.offset.x = static_cast<int32_t>(in->x);
  out.offset.y = static_cast<int32_t>(in->y);
  return out;
}

static inline VkCompareOp ri_vk_RICompareOpToVK(enum RICompareFunc_e func) {
  switch (func) {
  case RI_COMPARE_NONE:
    return VK_COMPARE_OP_NEVER;
  case RI_COMPARE_ALWAYS:
    return VK_COMPARE_OP_ALWAYS;
  case RI_COMPARE_NEVER:
    return VK_COMPARE_OP_NEVER;
  case RI_COMPARE_EQUAL:
    return VK_COMPARE_OP_EQUAL;
  case RI_COMPARE_NOT_EQUAL:
    return VK_COMPARE_OP_NOT_EQUAL;
  case RI_COMPARE_LESS:
    return VK_COMPARE_OP_LESS;
  case RI_COMPARE_LESS_EQUAL:
    return VK_COMPARE_OP_LESS_OR_EQUAL;
  case RI_COMPARE_GREATER:
    return VK_COMPARE_OP_GREATER;
  case RI_COMPARE_GREATER_EQUAL:
    return VK_COMPARE_OP_GREATER_OR_EQUAL;
  default:
    break;
  }
  assert(false);
  return VK_COMPARE_OP_NEVER;
}

static inline VkIndexType ri_vk_RIIndexTypeToVK(enum RIIndexType_e type) {
  switch (type) {
  case RI_INDEX_TYPE_16:
    return VK_INDEX_TYPE_UINT16;
  case RI_INDEX_TYPE_32:
    return VK_INDEX_TYPE_UINT32;
  }
  assert(false);
  return VK_INDEX_TYPE_UINT32;
}

static inline VkCullModeFlagBits ri_vk_RICullModeToVK(enum RICullMode_e mask) {
  uint32_t flags = VK_CULL_MODE_NONE;
  if (mask & RI_CULL_MODE_FRONT)
    flags |= VK_CULL_MODE_FRONT_BIT;
  if (mask & RI_CULL_MODE_BACK)
    flags |= VK_CULL_MODE_BACK_BIT;
  return (VkCullModeFlagBits)flags;
}

static inline VkColorComponentFlags
ri_vk_RIColorWriteMaskToVK(enum RIColorWriteMask_e mask) {
  VkColorComponentFlags ret = 0;
  if (mask & RI_COLOR_WRITE_R) {
    ret |= VK_COLOR_COMPONENT_R_BIT;
  }
  if (mask & RI_COLOR_WRITE_G) {
    ret |= VK_COLOR_COMPONENT_G_BIT;
  }
  if (mask & RI_COLOR_WRITE_B) {
    ret |= VK_COLOR_COMPONENT_B_BIT;
  }
  if (mask & RI_COLOR_WRITE_A) {
    ret |= VK_COLOR_COMPONENT_A_BIT;
  }
  return ret;
}

static inline VkPrimitiveTopology
ri_vk_RITopologyToVK(enum RITopology_e topology) {
  switch (topology) {
  case RI_TOPOLOGY_POINT_LIST:
    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case RI_TOPOLOGY_LINE_LIST:
    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case RI_TOPOLOGY_LINE_STRIP:
    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  case RI_TOPOLOGY_TRIANGLE_LIST:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case RI_TOPOLOGY_TRIANGLE_STRIP:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  case RI_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
  case RI_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
  case RI_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
  case RI_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
  case RI_TOPOLOGY_PATCH_LIST:
    return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
  }
  // this shouldn't happen
  assert(false);
  return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
}

static inline VkBlendFactor
ri_vk_RIBlendFactorToVK(enum RIBlendFactor_e factor) {
  switch (factor) {
  case RI_BLEND_ZERO:
    return VK_BLEND_FACTOR_ZERO;
  case RI_BLEND_ONE:
    return VK_BLEND_FACTOR_ONE;
  case RI_BLEND_SRC_COLOR:
    return VK_BLEND_FACTOR_SRC_COLOR;
  case RI_BLEND_ONE_MINUS_SRC_COLOR:
    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case RI_BLEND_DST_COLOR:
    return VK_BLEND_FACTOR_DST_COLOR;
  case RI_BLEND_ONE_MINUS_DST_COLOR:
    return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case RI_BLEND_SRC_ALPHA:
    return VK_BLEND_FACTOR_SRC_ALPHA;
  case RI_BLEND_ONE_MINUS_SRC_ALPHA:
    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case RI_BLEND_DST_ALPHA:
    return VK_BLEND_FACTOR_DST_ALPHA;
  case RI_BLEND_ONE_MINUS_DST_ALPHA:
    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  case RI_BLEND_CONSTANT_COLOR:
    return VK_BLEND_FACTOR_CONSTANT_COLOR;
  case RI_BLEND_ONE_MINUS_CONSTANT_COLOR:
    return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
  case RI_BLEND_CONSTANT_ALPHA:
    return VK_BLEND_FACTOR_CONSTANT_ALPHA;
  case RI_BLEND_ONE_MINUS_CONSTANT_ALPHA:
    return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
  case RI_BLEND_SRC_ALPHA_SATURATE:
    return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
  case RI_BLEND_SRC1_COLOR:
    return VK_BLEND_FACTOR_SRC1_COLOR;
  case RI_BLEND_ONE_MINUS_SRC1_COLOR:
    return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
  case RI_BLEND_SRC1_ALPHA:
    return VK_BLEND_FACTOR_SRC1_ALPHA;
  case RI_BLEND_ONE_MINUS_SRC1_ALPHA:
    return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
  }
  return VK_BLEND_FACTOR_ZERO;
}

static inline VkBlendOp ri_vk_RIBlendOpToVK(enum RIBlendOp_e op) {
  switch (op) {
  case RI_BLEND_OP_ADD:
    return VK_BLEND_OP_ADD;
  case RI_BLEND_OP_SUBTRACT:
    return VK_BLEND_OP_SUBTRACT;
  case RI_BLEND_OP_REVERSE_SUBTRACT:
    return VK_BLEND_OP_REVERSE_SUBTRACT;
  case RI_BLEND_OP_MIN:
    return VK_BLEND_OP_MIN;
  case RI_BLEND_OP_MAX:
    return VK_BLEND_OP_MAX;
  }
  assert(false);
  return VK_BLEND_OP_ADD;
}

static inline VkStencilOp ri_vk_RIStencilOpToVK(enum RIStencilOp_e op) {
  switch (op) {
  case RI_STENCIL_OP_KEEP:
    return VK_STENCIL_OP_KEEP;
  case RI_STENCIL_OP_ZERO:
    return VK_STENCIL_OP_ZERO;
  case RI_STENCIL_OP_REPLACE:
    return VK_STENCIL_OP_REPLACE;
  case RI_STENCIL_OP_INCREMENT_AND_CLAMP:
    return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
  case RI_STENCIL_OP_DECREMENT_AND_CLAMP:
    return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
  case RI_STENCIL_OP_INVERT:
    return VK_STENCIL_OP_INVERT;
  case RI_STENCIL_OP_INCREMENT_AND_WRAP:
    return VK_STENCIL_OP_INCREMENT_AND_WRAP;
  case RI_STENCIL_OP_DECREMENT_AND_WRAP:
    return VK_STENCIL_OP_DECREMENT_AND_WRAP;
  }
  assert(false);
  return VK_STENCIL_OP_KEEP;
}

static inline VkPolygonMode ri_vk_RIPolygonModeToVK(enum RIPolygonMode_e mode) {
  switch (mode) {
  case RI_POLYGON_MODE_FILL:
    return VK_POLYGON_MODE_FILL;
  case RI_POLYGON_MODE_LINE:
    return VK_POLYGON_MODE_LINE;
  }
  assert(false);
  return VK_POLYGON_MODE_FILL;
}

static inline VkFrontFace ri_vk_RIFrontFaceToVK(enum RIFrontFace_e face) {
  switch (face) {
  case RI_FRONT_FACE_COUNTER_CLOCKWISE:
    return VK_FRONT_FACE_COUNTER_CLOCKWISE;
  case RI_FRONT_FACE_CLOCKWISE:
    return VK_FRONT_FACE_CLOCKWISE;
  }
  assert(false);
  return VK_FRONT_FACE_CLOCKWISE;
}

static inline VkVertexInputRate
ri_vk_RIVertexInputRateToVK(enum RIVertexInputRate_e rate) {
  switch (rate) {
  case RI_VERTEX_INPUT_RATE_VERTEX:
    return VK_VERTEX_INPUT_RATE_VERTEX;
  case RI_VERTEX_INPUT_RATE_INSTANCE:
    return VK_VERTEX_INPUT_RATE_INSTANCE;
  }
  assert(false);
  return VK_VERTEX_INPUT_RATE_VERTEX;
}

// RISampleCount_e / RIGraphicsPipelineDesc::sampleCount -> VkSampleCountFlagBits.
// 0 and 1 both mean "no MSAA", matching the RITextureDesc convention.
static inline VkSampleCountFlagBits
ri_vk_RISampleCountToVK(uint32_t sampleCount) {
  switch (sampleCount) {
  case 0:
  case 1:
    return VK_SAMPLE_COUNT_1_BIT;
  case 2:
    return VK_SAMPLE_COUNT_2_BIT;
  case 4:
    return VK_SAMPLE_COUNT_4_BIT;
  case 8:
    return VK_SAMPLE_COUNT_8_BIT;
  case 16:
    return VK_SAMPLE_COUNT_16_BIT;
  }
  assert(false);
  return VK_SAMPLE_COUNT_1_BIT;
}

static inline VkImageType
ri_vk_RITextureTypeToVKImageType(enum RITextureType_e e) {
  switch (e) {
  case RI_TEXTURE_1D:
    return VK_IMAGE_TYPE_1D;
  case RI_TEXTURE_2D:
    return VK_IMAGE_TYPE_2D;
  case RI_TEXTURE_3D:
    return VK_IMAGE_TYPE_3D;
  }
  // this shouldn't happen
  assert(false);
  return VK_IMAGE_TYPE_MAX_ENUM;
}

#endif

#endif
