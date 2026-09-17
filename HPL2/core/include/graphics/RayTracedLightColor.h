#ifndef HPL_RAY_TRACED_LIGHT_COLOR_H
#define HPL_RAY_TRACED_LIGHT_COLOR_H

#include <array>
#include "graphics/Color.h"

namespace hpl {

// Art-directed response for authored light tints, not a replacement for sRGB.
// Retain 65% of linear RGB chroma around Rec.709 luminance. This preserves
// light energy on neutral receivers while reducing the warm cast of legacy
// lights. Apply once at RT upload, after backend overrides and animation.
// Textures, emissive surfaces, fog, alpha and Standard lighting stay unchanged.
inline std::array<float, 3> RayTracedLightColorToLinear(float r, float g, float b) {
  const std::array<float, 3> linear = {
      sRGBToLinear(r), sRGBToLinear(g), sRGBToLinear(b)};
  const float luminance =
      0.2126f * linear[0] + 0.7152f * linear[1] + 0.0722f * linear[2];
  constexpr float retainedChroma = 0.65f;
  return {luminance + retainedChroma * (linear[0] - luminance),
          luminance + retainedChroma * (linear[1] - luminance),
          luminance + retainedChroma * (linear[2] - luminance)};
}

} // namespace hpl

#endif
