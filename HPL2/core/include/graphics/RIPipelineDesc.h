#ifndef RI_PIPELINE_DESC_H
#define RI_PIPELINE_DESC_H

// Backend-independent description of a graphics / compute / ray-tracing
// pipeline, consumed by the RIProgram::bind*Pipeline overloads.
//
// This header must stay free of Vulkan and D3D12 headers, directly and
// transitively -- that is the whole point of it. Both backends translate FROM
// these structs; neither backend's vocabulary appears in one. The include set
// below is the enforced budget (see tests/graphics/ri_pipeline_desc_isolation).
//
// Everything here is a flat, copyable POD: fixed-capacity inline arrays, no
// pointers, no pNext chain. That is deliberate. The Vulkan create-info it
// replaces is a web of pXxxState pointers, which forced every call site to
// build a non-copyable holder struct whose only job was keeping those pointers
// alive. A desc can be passed by value, stored, compared and hashed
// structurally (RIHashGraphicsPipelineDesc) with none of that ceremony.
//
// The state modelled here is the INTERSECTION of what the backends support,
// not Vulkan's superset. Fields the D3D12 PSO path cannot express -- logic op,
// rasterizer discard, depth bounds, sample shading, alpha-to-one -- are absent
// by design rather than present and rejected at bind time.

#include "graphics/RIFormat.h"   // RI_Format_e
#include "graphics/RIPipeline.h" // RITopology_e, RIBlendFactor_e, ...
#include "system/Hasher.h"       // hash_t

#include <stdint.h>

namespace hpl {

// Matches the Vulkan/D3D12 simultaneous render-target limit.
inline constexpr uint32_t RI_MAX_COLOR_ATTACHMENTS = 8;
// Kept in step with RIProgram::MAX_VERTEX_ATTRIBUTES.
inline constexpr uint32_t RI_MAX_VERTEX_ATTRIBUTES = 16;
inline constexpr uint32_t RI_MAX_VERTEX_BINDINGS = 8;

struct RIVertexBindingDesc {
  uint32_t binding = 0;
  uint32_t stride = 0;
  RIVertexInputRate_e inputRate = RI_VERTEX_INPUT_RATE_VERTEX;
};

// Field order deliberately matches VkVertexInputAttributeDescription and
// D3D12_INPUT_ELEMENT_DESC, which both put format before offset. Porting a
// brace-initialized Vulkan attribute table is common, and format/offset sitting
// the other way round would swap two adjacent same-width fields silently.
struct RIVertexAttributeDesc {
  uint32_t location = 0;
  uint32_t binding = 0;
  RI_Format_e format = RI_FORMAT_UNKNOWN;
  uint32_t offset = 0;
};

struct RIVertexInputDesc {
  RIVertexBindingDesc bindings[RI_MAX_VERTEX_BINDINGS]{};
  uint32_t bindingCount = 0;
  RIVertexAttributeDesc attributes[RI_MAX_VERTEX_ATTRIBUTES]{};
  uint32_t attributeCount = 0;
};

struct RIRasterizationDesc {
  RIPolygonMode_e polygonMode = RI_POLYGON_MODE_FILL;
  RICullMode_e cullMode = RI_CULL_MODE_NONE;
  RIFrontFace_e frontFace = RI_FRONT_FACE_CLOCKWISE;
  bool depthClamp = false;
  bool depthBiasEnable = false;
  float depthBiasConstant = 0.0f;
  float depthBiasClamp = 0.0f;
  float depthBiasSlope = 0.0f;
  // Vulkan-only beyond 1.0 (D3D12 has no line width); kept because every
  // current call site sets it to 1.0f and Vulkan requires a valid value.
  float lineWidth = 1.0f;
};

struct RIStencilFaceDesc {
  RIStencilOp_e failOp = RI_STENCIL_OP_KEEP;
  RIStencilOp_e passOp = RI_STENCIL_OP_KEEP;
  RIStencilOp_e depthFailOp = RI_STENCIL_OP_KEEP;
  RICompareFunc_e compareFunc = RI_COMPARE_ALWAYS;
  uint32_t compareMask = 0;
  uint32_t writeMask = 0;
};

struct RIDepthStencilDesc {
  bool depthTest = false;
  bool depthWrite = false;
  RICompareFunc_e depthCompare = RI_COMPARE_LESS_EQUAL;
  bool stencilTest = false;
  // Shared by both faces rather than living in RIStencilFaceDesc, because
  // D3D12 has a single stencil reference (OMSetStencilRef) where Vulkan has
  // one per face. Modelling the intersection keeps the desc portable.
  //
  // It is pipeline state on Vulkan but command-list state on D3D12, so the
  // D3D12 path carries it through PipelineSlot and replays it at bind time --
  // the same treatment topology and vertex strides get.
  uint32_t stencilReference = 0;
  RIStencilFaceDesc front{};
  RIStencilFaceDesc back{};
};

struct RIBlendAttachmentDesc {
  bool blendEnable = false;
  RIBlendFactor_e srcColor = RI_BLEND_ONE;
  RIBlendFactor_e dstColor = RI_BLEND_ZERO;
  RIBlendOp_e colorOp = RI_BLEND_OP_ADD;
  RIBlendFactor_e srcAlpha = RI_BLEND_ONE;
  RIBlendFactor_e dstAlpha = RI_BLEND_ZERO;
  RIBlendOp_e alphaOp = RI_BLEND_OP_ADD;
  // NOTE: defaults to RGBA, where a zero-initialized
  // VkPipelineColorBlendAttachmentState means "write no channels". Writing all
  // channels is what every call site in the engine actually wants -- but when
  // porting one, do not assume a field left unset here matches a field left
  // unset there.
  RIColorWriteMask_e writeMask = RI_COLOR_WRITE_RGBA;
};

// Attachment formats the pipeline renders into. Replaces the pNext-chained
// VkPipelineRenderingCreateInfo; must agree with the RIBeginRenderingDesc the
// pass is recorded inside.
struct RIRenderTargetDesc {
  RI_Format_e colorFormats[RI_MAX_COLOR_ATTACHMENTS]{};
  uint32_t colorCount = 0;
  RI_Format_e depthFormat = RI_FORMAT_UNKNOWN;
  RI_Format_e stencilFormat = RI_FORMAT_UNKNOWN;
};

// Viewport and scissor are absent: both are dynamic state on every pipeline
// this engine creates, with a single viewport and a single scissor. The
// backend translators hardcode that. Should a pass ever need static or
// multiple viewports, add it here rather than reintroducing a dynamic-state
// array.
struct RIGraphicsPipelineDesc {
  RIVertexInputDesc vertexInput{};
  RITopology_e topology = RI_TOPOLOGY_TRIANGLE_LIST;
  bool primitiveRestart = false;
  RIRasterizationDesc raster{};
  uint32_t sampleCount = 1; // RISampleCount_e; 0/1 = no MSAA
  bool alphaToCoverage = false;
  RIDepthStencilDesc depthStencil{};
  RIBlendAttachmentDesc blend[RI_MAX_COLOR_ATTACHMENTS]{};
  uint32_t blendCount = 0;
  RIRenderTargetDesc renderTarget{};
};

// No state today: a compute pipeline is fully described by the program's
// shader stage and root/pipeline layout. Named so the three bind* entry points
// stay symmetric and so per-pipeline compute state has somewhere to land.
struct RIComputePipelineDesc {};

struct RIRayTracingPipelineDesc {
  uint32_t maxRecursionDepth = 1;
  // Largest ray payload and hit-attribute struct any shader in the pipeline
  // declares, in bytes. Vulkan reads both out of the SPIR-V, so it ignores
  // them; DXR cannot and needs them up front
  // (D3D12_RAYTRACING_SHADER_CONFIG). Under-declaring corrupts the trace
  // silently rather than failing pipeline creation, so callers state the size
  // of the payload struct their shaders actually use.
  //
  // The default attribute size is the 2 barycentrics of
  // BuiltInTriangleIntersectionAttributes, which is what every triangle hit
  // group without an intersection shader gets.
  uint32_t maxPayloadSize = 0;
  uint32_t maxAttributeSize = 2 * sizeof(float);
};

// Structural hash over every field that affects pipeline compilation. Folded
// into the pipeline cache key by the bind* overloads so two distinct states
// cannot collide on a caller-supplied variant hash.
hash_t RIHashGraphicsPipelineDesc(hash_t seed, const RIGraphicsPipelineDesc &desc);

} // namespace hpl

#endif // RI_PIPELINE_DESC_H
