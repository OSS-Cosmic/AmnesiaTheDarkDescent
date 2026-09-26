#ifndef HPL_TEMPORAL_REACTIVE_MASK_MATH_H
#define HPL_TEMPORAL_REACTIVE_MASK_MATH_H

#include "graphics/TemporalUpscalerTypes.h"

#include <cmath>
#include <cstdint>

namespace hpl {

struct TemporalReactiveMaskParams {
  float luminanceFloor = 1.0e-3f;
  float reactiveScale = 1.0f;
  float compositionScale = 0.75f;
  float compositionKnee = 0.25f;
  float changeEpsilon = 1.0e-4f;
};

// Standard's scene color is display-linear radiance divided by sceneExposure.
// Its generic color-difference mask should affect FSR accumulation without
// independently removing detail protection across additive halos. Other
// renderer/provider combinations retain the shared defaults. Invalid exposure
// or an unrepresentable floor falls back to a 0.05 scene-linear floor.
TemporalReactiveMaskParams TemporalReactiveMaskDefaultParams(
    TemporalUpscalerProvider provider, bool standardRenderer,
    float sceneExposure);

struct TemporalReactiveMaskSample {
  float reactive = 0.0f;
  float composition = 0.0f;
};

// This extent mirrors TemporalUpscalerExtent without importing the graphics
// or render-interface headers, so the pure math can be tested headlessly.
struct TemporalReactiveMaskMathExtent {
  uint32_t width = 0;
  uint32_t height = 0;
};

// Reference mask contract, evaluated from the same linear HDR values before
// any later tone-map exposure or gamma:
//
//   num = sum_c |final_c - opaque_c|
//   den = sum_c max(|opaque_c|, |final_c|) + luminanceFloor
//   relative = clamp(num / den, 0, 1)
//   reactive = clamp(relative * reactiveScale, 0, 1)
//   composition = (relative > changeEpsilon)
//       ? clamp(compositionScale * clamp(relative / compositionKnee, 0, 1),
//               0, 1)
//       : 0
//
// Non-finite input components are treated as zero. With the positive default
// luminance floor, den is always greater than zero, including on a black
// background, and identical images produce exactly zero for both outputs.
// This is a per-channel L1 ratio rather than a luminance difference, so an
// equal-luminance chromatic change still responds. Composition strength is
// independent and conservative: marking every changed pixel fully unreliable
// would throw away detail on lightly tinted water, so the composition mask
// ramps to compositionScale (< 1) instead of forcing full history rejection.
TemporalReactiveMaskSample TemporalReactiveMaskEvaluate(
    const float opaqueRgb[3], const float finalRgb[3],
    const TemporalReactiveMaskParams &params = {});

// `x` and `y` are UNJITTERED mask pixel coordinates. Return the UV at which to
// sample the JITTERED render-extent scene mask. Geometry in the jittered image
// moves by -jitter, so recovering the unjittered grid subtracts
// jitterPixels/extent:
//   outUV = ((pixel + 0.5) - jitterPixels) / extent.
void TemporalReactiveMaskUnjitteredSampleUV(
    uint32_t x, uint32_t y, TemporalReactiveMaskMathExtent extent,
    const float jitterPixels[2], float outUV[2]);

} // namespace hpl

#endif // HPL_TEMPORAL_REACTIVE_MASK_MATH_H
