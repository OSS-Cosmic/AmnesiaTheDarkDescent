#ifndef PATH_TRACE_PAYLOAD_H
#define PATH_TRACE_PAYLOAD_H

// Sizes of the ray payload and hit attributes shared by every ray-tracing
// pipeline in the engine.
//
// These exist because DXR cannot recover them the way Vulkan can. A SPIR-V
// module carries its payload layout, so vkCreateRayTracingPipelinesKHR works
// them out on its own; a DXR state object must be told up front, through
// D3D12_RAYTRACING_SHADER_CONFIG. They are passed in
// RIRayTracingPipelineDesc::maxPayloadSize / maxAttributeSize.
//
// Under-declaring is not rejected at pipeline creation -- it corrupts the
// trace at runtime -- so this mirrors the shader struct exactly and is
// asserted against it by tests/graphics/ri_d3d12/rt_pipeline_smoke.

#include <stdint.h>

namespace hpl {

// Mirrors ScatterPayload in amnesia/slang/PathTracer/PathTraceCommon.slang,
// which both PathTracePass.rt.slang and WaterReflection.rt.slang trace with:
//
//   float3 radiance, thp, origin, direction   12 floats
//   float  firstRayLength, coneWidth, coneSpreadAngle
//   uint   currStep, status, rngHi, rngLo
//
// 19 4-byte scalars. Keep in step with that struct; a field added there and
// not here is a silent corruption, not a build break.
constexpr uint32_t kScatterPayloadSize = 19u * 4u;

// BuiltInTriangleIntersectionAttributes: the two barycentrics every triangle
// hit group reports. None of the engine's ray-tracing shaders declare an
// intersection shader -- shadow rays use inline RayQuery -- so no pipeline
// needs the larger procedural attribute struct.
constexpr uint32_t kTriangleAttributeSize = 2u * 4u;

} // namespace hpl

#endif // PATH_TRACE_PAYLOAD_H
