#include "graphics/WaterReflectionJitterMath.h"
#include "utest.h"

#include <cstdint>

UTEST(WaterReflectionJitter, ExtentRounding) {
  EXPECT_EQ_MSG(hpl::WaterReflectionHalfResExtent(1280), 640u,
                "even full extent uses half extent");
  EXPECT_EQ_MSG(hpl::WaterReflectionHalfResExtent(1281), 641u,
                "odd full extent uses ceil half extent");
  EXPECT_EQ_MSG(hpl::WaterReflectionHalfResExtent(1), 1u,
                "one-pixel extent remains one pixel");
}

UTEST(WaterReflectionJitter, EvenScaleOnce) {
  const float evenJitter[2] = {0.5f, -0.5f};
  const float evenExpected[2] = {0.25f, -0.25f};
  float evenActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(evenJitter, 1280, 720,
                                           evenActual);
  EXPECT_NEAR_MSG(evenActual[0], evenExpected[0], 2.0e-5f,
                  "even jitter scales once to 0.25, not 0.125");
  EXPECT_NEAR_MSG(evenActual[1], evenExpected[1], 2.0e-5f,
                  "even jitter scales once to 0.25, not 0.125");
  EXPECT_NE_MSG(evenActual[0], 0.125f,
                "even jitter is not scaled twice on x");
  EXPECT_NE_MSG(evenActual[1], -0.125f,
                "even jitter is not scaled twice on y");
}

UTEST(WaterReflectionJitter, OddExtent) {
  const float oddJitter[2] = {0.5f, -0.5f};
  const float oddRatioX = 641.0f / 1281.0f;
  const float oddRatioY = 361.0f / 721.0f;
  const float oddExpected[2] = {0.5f * oddRatioX, -0.5f * oddRatioY};
  float oddActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(oddJitter, 1281, 721, oddActual);
  EXPECT_NEAR_MSG(oddActual[0], oddExpected[0], 1.0e-7f,
                  "odd extent uses 641/1281 and 361/721 ratios");
  EXPECT_NEAR_MSG(oddActual[1], oddExpected[1], 1.0e-7f,
                  "odd extent uses 641/1281 and 361/721 ratios");
}

UTEST(WaterReflectionJitter, OnePixelExtent) {
  const float oneJitter[2] = {0.37f, -0.41f};
  const float oneExpected[2] = {0.37f, -0.41f};
  float oneActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(oneJitter, 1, 1, oneActual);
  EXPECT_NEAR_MSG(oneActual[0], oneExpected[0], 2.0e-5f,
                  "one-pixel extent keeps jitter unchanged");
  EXPECT_NEAR_MSG(oneActual[1], oneExpected[1], 2.0e-5f,
                  "one-pixel extent keeps jitter unchanged");
}

UTEST(WaterReflectionJitter, ZeroExtentsAndAxes) {
  const float zeroJitter[2] = {0.5f, -0.5f};
  const float zeroExpected[2] = {0.0f, 0.0f};
  float zeroActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(zeroJitter, 0, 0, zeroActual);
  EXPECT_NEAR_MSG(zeroActual[0], zeroExpected[0], 2.0e-5f,
                  "zero extent returns zero jitter without division");
  EXPECT_NEAR_MSG(zeroActual[1], zeroExpected[1], 2.0e-5f,
                  "zero extent returns zero jitter without division");

  const float zeroWidthExpected[2] = {0.0f, -0.5f * (361.0f / 721.0f)};
  float zeroWidthActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(zeroJitter, 0, 721,
                                            zeroWidthActual);
  EXPECT_NEAR_MSG(zeroWidthActual[0], zeroWidthExpected[0], 2.0e-5f,
                  "zero width only clears the x jitter axis");
  EXPECT_NEAR_MSG(zeroWidthActual[1], zeroWidthExpected[1], 2.0e-5f,
                  "zero width only clears the x jitter axis");
}

UTEST(WaterReflectionJitter, IndependentAxisScaling) {
  const float independentJitter[2] = {0.4f, -0.3f};
  const float independentExpected[2] = {0.4f * (640.0f / 1280.0f),
                                         -0.3f * (361.0f / 721.0f)};
  float independentActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(independentJitter, 1280, 721,
                                            independentActual);
  EXPECT_NEAR_MSG(independentActual[0], independentExpected[0], 2.0e-5f,
                  "width and height scale jitter independently");
  EXPECT_NEAR_MSG(independentActual[1], independentExpected[1], 2.0e-5f,
                  "width and height scale jitter independently");
}

UTEST(WaterReflectionJitter, CurrentAndPrevious) {
  const float oddRatioX = 641.0f / 1281.0f;
  const float oddRatioY = 361.0f / 721.0f;
  const float currentJitter[2] = {0.25f, -0.4f};
  const float previousJitter[2] = {-0.5f, 0.5f};
  const float currentExpected[2] = {0.25f * oddRatioX,
                                    -0.4f * oddRatioY};
  const float previousExpected[2] = {-0.5f * oddRatioX, 0.5f * oddRatioY};
  float currentActual[2] = {};
  float previousActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(currentJitter, 1281, 721,
                                            currentActual);
  hpl::WaterReflectionScaleJitterToHalfRes(previousJitter, 1281, 721,
                                            previousActual);
  EXPECT_NEAR_MSG(currentActual[0], currentExpected[0], 2.0e-5f,
                  "cameraJitterPrev uses the current frame ratios");
  EXPECT_NEAR_MSG(currentActual[1], currentExpected[1], 2.0e-5f,
                  "cameraJitterPrev uses the current frame ratios");
  EXPECT_NEAR_MSG(previousActual[0], previousExpected[0], 2.0e-5f,
                  "cameraJitterPrev uses the current frame ratios");
  EXPECT_NEAR_MSG(previousActual[1], previousExpected[1], 2.0e-5f,
                  "cameraJitterPrev uses the current frame ratios");
}

UTEST(WaterReflectionJitter, NRDBounds) {
  const float inputs[] = {-0.5f, -0.37f, -0.125f, 0.0f,
                          0.125f, 0.37f, 0.5f};
  for (float input : inputs) {
    const float pair[2] = {input, -input};
    float actual[2] = {};
    hpl::WaterReflectionScaleJitterToHalfRes(pair, 1, 1281, actual);
    EXPECT_GE_MSG(actual[0], -0.5f, "jitter x in NRD range");
    EXPECT_LE_MSG(actual[0], 0.5f, "jitter x in NRD range");
    EXPECT_GE_MSG(actual[1], -0.5f, "jitter y in NRD range");
    EXPECT_LE_MSG(actual[1], 0.5f, "jitter y in NRD range");
  }
}
