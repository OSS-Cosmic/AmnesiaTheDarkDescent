#include "graphics/RayTracedLightColor.h"
#include "utest.h"

UTEST(RayTracedLightColor, NeutralAndBlackStayNeutral) {
  for (float value : {0.0f, 0.02f, 0.5f, 1.0f, 2.0f}) {
    const auto result = hpl::RayTracedLightColorToLinear(value, value, value);
    for (float channel : result)
      ASSERT_NEAR(hpl::sRGBToLinear(value), channel, 1e-6f);
  }
}

UTEST(RayTracedLightColor, PreservesLuminanceAndNonnegativeHdrRange) {
  for (const auto input : {std::array<float, 3>{0.85f, 0.75f, 0.4f},
                           {0.8f, 0.7f, 0.6f}, {0.05f, 0.2f, 0.9f},
                           {1.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}}) {
    const auto result = hpl::RayTracedLightColorToLinear(input[0], input[1], input[2]);
    const float before = 0.2126f * hpl::sRGBToLinear(input[0]) +
                         0.7152f * hpl::sRGBToLinear(input[1]) +
                         0.0722f * hpl::sRGBToLinear(input[2]);
    const float after = 0.2126f * result[0] + 0.7152f * result[1] + 0.0722f * result[2];
    ASSERT_NEAR(before, after, 1e-6f);
    for (float channel : result) ASSERT_GE(channel, 0.0f);
  }
}

UTEST(RayTracedLightColor, CandleRemainsWarmWithLessChannelSeparation) {
  const auto result = hpl::RayTracedLightColorToLinear(0.85f, 0.75f, 0.4f);
  ASSERT_GT(result[0], result[1]);
  ASSERT_GT(result[1], result[2]);
  ASSERT_LT(result[0], hpl::sRGBToLinear(0.85f));
  ASSERT_GT(result[2], hpl::sRGBToLinear(0.4f));
  ASSERT_NEAR(result[2], hpl::sRGBToLinear(0.55829f), 1e-5f);
}
