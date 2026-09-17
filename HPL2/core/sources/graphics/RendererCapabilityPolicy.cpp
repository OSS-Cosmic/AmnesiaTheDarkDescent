#include "graphics/RendererCapabilityPolicy.h"

RendererCapabilityDecision RendererEvaluateCapabilityPolicy(
    RendererCapabilityMode_e mode, const RendererCapabilitySet &c) {
  RendererCapabilityDecision result = {mode, false, false, 0, nullptr};
  if (mode != RENDERER_CAPABILITY_RAYTRACED && mode != RENDERER_CAPABILITY_RASTER) {
    result.missingRequirement = "known renderer capability mode";
    return result;
  }

#define REQUIRE(field, label)                                                    \
  do {                                                                           \
    if (!c.field) {                                                              \
      result.missingRequirement = label;                                         \
      return result;                                                             \
    }                                                                            \
  } while (false)

  // These are consumed by the raster shaders and command paths.  Interlock
  // and barycentrics are optional paths (VBuffer raster has a fallback).
  REQUIRE(swapchain, "swapchain");
  REQUIRE(descriptorIndexing, "descriptor indexing");
  REQUIRE(bufferDeviceAddress, "buffer device address");
  REQUIRE(scalarBlockLayout, "scalar block layout");
  // SceneTypes buffer addresses and shared particle/triangle shaders use uint64_t.
  REQUIRE(shaderInt64, "shaderInt64");
  REQUIRE(dynamicRendering, "dynamic rendering");
  REQUIRE(descriptorBindingPartiallyBound, "descriptor binding partially bound");
  REQUIRE(shaderSampledImageArrayNonUniformIndexing,
          "sampled image non-uniform indexing");
  REQUIRE(descriptorBindingSampledImageUpdateAfterBind,
          "sampled image update after bind");

  if (mode == RENDERER_CAPABILITY_RAYTRACED) {
    REQUIRE(accelerationStructureExtension, "acceleration structure extension");
    REQUIRE(accelerationStructure, "acceleration structure feature");
    REQUIRE(rayTracingPipelineExtension, "ray tracing pipeline extension");
    REQUIRE(rayTracingPipeline, "ray tracing pipeline feature");
    REQUIRE(rayQueryExtension, "ray query extension");
    REQUIRE(rayQuery, "ray query feature");
    REQUIRE(bufferDeviceAddress, "buffer device address feature");
    REQUIRE(spirv14, "SPIR-V 1.4");
    REQUIRE(shaderFloatControls, "shader float controls");
    REQUIRE(deferredHostOperationsExtension, "deferred host operations extension");
    REQUIRE(deferredHostOperations, "deferred host operations");
    result.rayTracingEnabled = true;
    result.rayTracingTier = RendererEvaluateRayTracingTier(c);
  }
  result.supported = true;
  return result;
#undef REQUIRE
}

uint8_t RendererEvaluateRayTracingTier(const RendererCapabilitySet &c) {
  // Tier 1 is the RT pipeline/AS foundation. Inline ray query is required by
  // raytraced shaders, but is not part of the tier-1 classification.
  const bool tier1 = c.accelerationStructureExtension && c.accelerationStructure &&
                     c.rayTracingPipelineExtension && c.rayTracingPipeline &&
                     c.bufferDeviceAddress && c.deferredHostOperationsExtension &&
                     c.deferredHostOperations && c.spirv14 &&
                     c.shaderFloatControls;
  if (!tier1)
    return 0;
  const bool tier2 = c.rayQueryExtension && c.rayQuery &&
                     c.rayTracingPipelineTraceRaysIndirect;
  return tier2 ? 2 : 1;
}
