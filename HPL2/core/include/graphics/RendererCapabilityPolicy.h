#ifndef HPL2_RENDERER_CAPABILITY_POLICY_H
#define HPL2_RENDERER_CAPABILITY_POLICY_H

#include <stddef.h>
#include <stdint.h>

// Kept here because the policy files are intentionally dependency-light and
// cannot grow a dependency on the Vulkan prelude.  RIDevice::init returns int.
static constexpr int RI_UNSUPPORTED = -2;

// This file deliberately contains no Vulkan (or SDK) types.  It is also the
// contract used by adapter tests, so keep the names describing capabilities,
// rather than the mechanism used to expose them.
enum RendererCapabilityMode_e : uint8_t {
  // RIDevice::init derives this from RIDeviceDesc::requestRayTracing.
  RENDERER_CAPABILITY_OVERDRIVE = 0,
  RENDERER_CAPABILITY_RASTER = 1,
};

struct RendererCapabilitySet {
  bool swapchain;
  bool descriptorIndexing;
  bool bufferDeviceAddress;
  bool scalarBlockLayout;
  bool shaderInt64;
  bool shaderBufferInt64Atomics;
  bool fragmentShaderBarycentric;
  bool fragmentShaderInterlock;
  bool dynamicRendering;
  bool accelerationStructure;
  bool rayQuery;
  bool rayTracingPipeline;
  bool spirv14;
  bool shaderFloatControls;
  bool deferredHostOperations;
  // Extension advertisement is kept separate from feature support.  Core
  // promotions must not be inferred from extension names.
  bool accelerationStructureExtension;
  bool rayQueryExtension;
  bool rayTracingPipelineExtension;
  bool deferredHostOperationsExtension;
  bool spirv14Extension;
  bool shaderFloatControlsExtension;
  bool descriptorBindingPartiallyBound;
  bool shaderSampledImageArrayNonUniformIndexing;
  bool descriptorBindingSampledImageUpdateAfterBind;
  bool descriptorBindingStorageBufferUpdateAfterBind;
  bool descriptorBindingStorageImageUpdateAfterBind;
  bool rayTracingPipelineTraceRaysIndirect;
};

struct RendererCapabilityDecision {
  RendererCapabilityMode_e mode;
  bool supported;
  bool rayTracingEnabled;
  uint8_t rayTracingTier;
  const char *missingRequirement;
};

RendererCapabilityDecision RendererEvaluateCapabilityPolicy(
    RendererCapabilityMode_e mode, const RendererCapabilitySet &capabilities);

uint8_t RendererEvaluateRayTracingTier(const RendererCapabilitySet &capabilities);

#endif
