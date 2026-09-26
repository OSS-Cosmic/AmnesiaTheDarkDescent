#include "graphics/TemporalReactiveMaskMath.h"

namespace hpl {
namespace {

static float SanitizeComponent(float value) {
  return std::isfinite(value) ? value : 0.0f;
}

static double ClampUnit(double value) {
  if (!(value > 0.0))
    return 0.0;
  return value < 1.0 ? value : 1.0;
}

static double PositiveFloor(float value) {
  if (std::isfinite(value) && value > 0.0f)
    return value;
  return 1.0e-3;
}

} // namespace

TemporalReactiveMaskParams TemporalReactiveMaskDefaultParams(
    TemporalUpscalerProvider provider, bool standardRenderer,
    float sceneExposure) {
  TemporalReactiveMaskParams params;
  if (provider == TemporalUpscalerProvider::Fsr && standardRenderer) {
    const float exposure = std::isfinite(sceneExposure) && sceneExposure > 0.0f
                               ? sceneExposure
                               : 1.0f;
    const float floor = 0.05f / exposure;
    params.luminanceFloor = std::isfinite(floor) && floor > 0.0f ? floor : 0.05f;
    params.reactiveScale = 0.5f;
    // A same-frame transparency difference is not evidence that the opaque
    // detail beneath a flame needs its history protection removed as well.
    params.compositionScale = 0.0f;
  }
  return params;
}

TemporalReactiveMaskSample TemporalReactiveMaskEvaluate(
    const float opaqueRgb[3], const float finalRgb[3],
    const TemporalReactiveMaskParams &params) {
  double numerator = 0.0;
  double denominator = PositiveFloor(params.luminanceFloor);
  for (uint32_t channel = 0; channel < 3; ++channel) {
    const double opaque = SanitizeComponent(opaqueRgb[channel]);
    const double final = SanitizeComponent(finalRgb[channel]);
    const double opaqueMagnitude = std::fabs(opaque);
    const double finalMagnitude = std::fabs(final);
    numerator += std::fabs(final - opaque);
    denominator += opaqueMagnitude > finalMagnitude ? opaqueMagnitude
                                                     : finalMagnitude;
  }

  const double relative = ClampUnit(numerator / denominator);
  TemporalReactiveMaskSample result = {};
  result.reactive = static_cast<float>(
      ClampUnit(relative * static_cast<double>(params.reactiveScale)));

  if (relative > static_cast<double>(params.changeEpsilon)) {
    const double knee = (std::isfinite(params.compositionKnee) &&
                         params.compositionKnee > 0.0f)
                            ? static_cast<double>(params.compositionKnee)
                            : 1.0;
    const double kneeRatio = ClampUnit(relative / knee);
    result.composition = static_cast<float>(ClampUnit(
        static_cast<double>(params.compositionScale) * kneeRatio));
  }
  return result;
}

void TemporalReactiveMaskUnjitteredSampleUV(
    uint32_t x, uint32_t y, TemporalReactiveMaskMathExtent extent,
    const float jitterPixels[2], float outUV[2]) {
  if (extent.width == 0 || extent.height == 0) {
    outUV[0] = 0.0f;
    outUV[1] = 0.0f;
    return;
  }

  outUV[0] = (static_cast<float>(x) + 0.5f - jitterPixels[0]) /
             static_cast<float>(extent.width);
  outUV[1] = (static_cast<float>(y) + 0.5f - jitterPixels[1]) /
             static_cast<float>(extent.height);
}

} // namespace hpl
