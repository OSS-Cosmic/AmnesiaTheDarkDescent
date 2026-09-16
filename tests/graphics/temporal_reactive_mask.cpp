#include "graphics/TemporalReactiveMaskMath.h"
#include "utest.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

bool IsFiniteUnit(float value) {
  return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

void CheckIdenticalImage(int *utest_result, const float color[3],
                        const char *name) {
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(color, color);
  // Identical images must be exactly unchanged, including black where a
  // naive relative metric would divide zero by zero.
  EXPECT_EQ_MSG(sample.reactive, 0.0f, name);
  EXPECT_EQ_MSG(sample.composition, 0.0f, name);
  (void)utest_result;
}

void CheckUV(int *utest_result, uint32_t x, uint32_t y,
             hpl::TemporalReactiveMaskMathExtent extent,
             const float jitter[2], const float expected[2],
             const char *name) {
  float actual[2] = {};
  hpl::TemporalReactiveMaskUnjitteredSampleUV(x, y, extent, jitter, actual);
  EXPECT_NEAR_MSG(actual[0], expected[0], 2.0e-5f, name);
  EXPECT_NEAR_MSG(actual[1], expected[1], 2.0e-5f, name);
  (void)utest_result;
}

} // namespace

UTEST(TemporalReactiveMask, IdenticalImages) {
  const float midGrey[3] = {0.5f, 0.5f, 0.5f};
  const float veryBright[3] = {1.0e4f, 1.0e4f, 1.0e4f};
  const float black[3] = {0.0f, 0.0f, 0.0f};
  CheckIdenticalImage(utest_result, midGrey, "identical mid-grey images are zero");
  CheckIdenticalImage(utest_result, veryBright,
                      "identical very bright images are zero");
  CheckIdenticalImage(utest_result, black, "identical black images are exactly zero");
}

UTEST(TemporalReactiveMask, AdditiveChange) {
  const float black[3] = {0.0f, 0.0f, 0.0f};
  const float additive[3] = {2.0f, 0.0f, 0.0f};
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(black, additive);
  // Additive emission over black must remain strongly reactive without
  // producing an invalid ratio or an out-of-range mask.
  EXPECT_GT_MSG(sample.reactive, 0.9f,
                "black plus bright additive color is strongly reactive");
  EXPECT_TRUE_MSG(IsFiniteUnit(sample.reactive),
                  "additive reactive output is finite and in [0,1]");
  EXPECT_TRUE_MSG(IsFiniteUnit(sample.composition),
                  "additive composition output is finite and in [0,1]");
}

UTEST(TemporalReactiveMask, MultiplicativeTint) {
  const float black[3] = {0.0f, 0.0f, 0.0f};
  const float additive[3] = {2.0f, 0.0f, 0.0f};
  const float additiveReactive =
      hpl::TemporalReactiveMaskEvaluate(black, additive).reactive;
  const float opaque[3] = {1.0f, 1.0f, 1.0f};
  const float tinted[3] = {1.0f, 0.5f, 0.5f};
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(opaque, tinted);
  // A tint is a real color change, but is less reactive than the additive
  // emission above; a luminance-only threshold would also lose this signal.
  EXPECT_GT_MSG(sample.reactive, 0.0f,
                "multiplicative tint responds below additive response");
  EXPECT_LT_MSG(sample.reactive, additiveReactive,
                "multiplicative tint responds below additive response");
  EXPECT_TRUE_MSG(IsFiniteUnit(sample.reactive),
                  "multiplicative tint reactive output is finite and in [0,1]");
  EXPECT_TRUE_MSG(IsFiniteUnit(sample.composition),
                  "multiplicative tint composition output is finite and in [0,1]");
}

UTEST(TemporalReactiveMask, EqualLuminanceChromaticChange) {
  const float opaque[3] = {1.0f, 1.0f, 1.0f};
  const float chromatic[3] = {1.2f, 1.0f, 0.8f};
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(opaque, chromatic);
  // The channel sum, and therefore simple equal-weight luminance, is equal;
  // this L1 color metric must still respond to the chromatic change.
  EXPECT_GT_MSG(sample.reactive, 0.0f,
                "equal-luminance chromatic change is reactive");
  EXPECT_TRUE_MSG(IsFiniteUnit(sample.reactive),
                  "chromatic reactive output is finite and in [0,1]");
  EXPECT_TRUE_MSG(IsFiniteUnit(sample.composition),
                  "chromatic composition output is finite and in [0,1]");
}

UTEST(TemporalReactiveMask, BrightHDRValues) {
  const float opaque[3] = {1.0e4f, 1.0e4f, 1.0e4f};
  const float changed[3] = {1.5e4f, 1.5e4f, 1.5e4f};
  const hpl::TemporalReactiveMaskSample changedSample =
      hpl::TemporalReactiveMaskEvaluate(opaque, changed);
  const hpl::TemporalReactiveMaskSample unchangedSample =
      hpl::TemporalReactiveMaskEvaluate(opaque, opaque);
  // HDR values must not overflow the intermediate ratio or escape mask range.
  EXPECT_TRUE_MSG(IsFiniteUnit(changedSample.reactive),
                  "bright HDR change reactive stays finite and in [0,1]");
  EXPECT_TRUE_MSG(IsFiniteUnit(changedSample.composition),
                  "bright HDR change composition stays finite and in [0,1]");
  EXPECT_TRUE_MSG(IsFiniteUnit(unchangedSample.reactive),
                  "bright HDR identity reactive stays finite and in [0,1]");
  EXPECT_TRUE_MSG(IsFiniteUnit(unchangedSample.composition),
                  "bright HDR identity composition stays finite and in [0,1]");
}

UTEST(TemporalReactiveMask, NonFiniteInputs) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const float opaque[3] = {nan, 1.0f, infinity};
  const float final[3] = {0.0f, infinity, nan};
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(opaque, final);
  // NaN and Inf components are sanitized as zero rather than reaching masks.
  EXPECT_TRUE_MSG(IsFiniteUnit(sample.reactive) &&
                      IsFiniteUnit(sample.composition),
                  "non-finite components are sanitized to finite unit outputs");
}

UTEST(TemporalReactiveMask, CompositionContract) {
  const float black[3] = {0.0f, 0.0f, 0.0f};
  const float additive[3] = {2.0f, 0.0f, 0.0f};
  const hpl::TemporalReactiveMaskSample strong =
      hpl::TemporalReactiveMaskEvaluate(black, additive);
  const hpl::TemporalReactiveMaskParams params = {};
  const float unchanged[3] = {1.0f, 1.0f, 1.0f};
  const float justBelow[3] = {1.0f + params.changeEpsilon * 0.5f, 1.0f,
                              1.0f};
  const hpl::TemporalReactiveMaskSample unchangedSample =
      hpl::TemporalReactiveMaskEvaluate(unchanged, unchanged);
  const hpl::TemporalReactiveMaskSample belowEpsilon =
      hpl::TemporalReactiveMaskEvaluate(unchanged, justBelow, params);
  // Composition is deliberately conservative and independently scaled: a
  // strong change reaches compositionScale, not full history rejection.
  EXPECT_NEAR_MSG(strong.reactive, 1.0f, 1.0e-3f,
                  "strong change reaches reactive one");
  EXPECT_NEAR_MSG(strong.composition, params.compositionScale, 2.0e-5f,
                  "strong change reaches compositionScale");
  EXPECT_LT_MSG(strong.composition, 1.0f,
                "strong change composition remains below one");
  EXPECT_EQ_MSG(unchangedSample.composition, 0.0f,
                "unchanged pixel composition is exactly zero");

  // The epsilon boundary suppresses composition for changes that are just
  // below the documented threshold.
  EXPECT_EQ_MSG(belowEpsilon.composition, 0.0f,
                "change below changeEpsilon has zero composition");
}

UTEST(TemporalReactiveMask, UnjitteredSampleUV) {
  const float zeroJitter[2] = {0.0f, 0.0f};
  const hpl::TemporalReactiveMaskMathExtent oneByOne = {1, 1};
  const float oneByOneCenter[2] = {0.5f, 0.5f};
  CheckUV(utest_result, 0, 0, oneByOne, zeroJitter, oneByOneCenter,
          "1x1 zero jitter maps to pixel center");
  const hpl::TemporalReactiveMaskMathExtent oddExtent = {1279, 721};
  const float oddCenter[2] = {1278.5f / 1279.0f, 720.5f / 721.0f};
  CheckUV(utest_result, 1278, 720, oddExtent, zeroJitter, oddCenter,
          "odd extent zero jitter maps to pixel center");
  const uint32_t x = 17;
  const uint32_t y = 23;
  const float center[2] = {(static_cast<float>(x) + 0.5f) / 1279.0f,
                           (static_cast<float>(y) + 0.5f) / 721.0f};
  const float positiveXNegativeY[2] = {0.25f, -0.4f};
  const float expectedPositiveNegative[2] = {
      center[0] - positiveXNegativeY[0] / 1279.0f,
      center[1] - positiveXNegativeY[1] / 721.0f};
  float actualPositiveNegative[2] = {};
  hpl::TemporalReactiveMaskUnjitteredSampleUV(
      x, y, oddExtent, positiveXNegativeY, actualPositiveNegative);
  // A positive x jitter moves the sample left, while a negative y jitter
  // moves it down; these signs protect XeSS's responsive-mask lookup.
  EXPECT_NEAR_MSG(actualPositiveNegative[0], expectedPositiveNegative[0], 2.0e-5f,
                  "positive and negative jitter translate by negative extent");
  EXPECT_NEAR_MSG(actualPositiveNegative[1], expectedPositiveNegative[1], 2.0e-5f,
                  "positive and negative jitter translate by negative extent");
  EXPECT_LT_MSG(actualPositiveNegative[0], center[0],
                "positive x jitter has explicit sign");
  EXPECT_GT_MSG(actualPositiveNegative[1], center[1],
                "negative y jitter has explicit sign");

  const float negativeXPositiveY[2] = {-0.25f, 0.4f};
  const float expectedNegativePositive[2] = {
      center[0] - negativeXPositiveY[0] / 1279.0f,
      center[1] - negativeXPositiveY[1] / 721.0f};
  float actualNegativePositive[2] = {};
  hpl::TemporalReactiveMaskUnjitteredSampleUV(
      x, y, oddExtent, negativeXPositiveY, actualNegativePositive);
  // Reversing both jitter components must reverse both UV translations.
  EXPECT_NEAR_MSG(actualNegativePositive[0], expectedNegativePositive[0], 2.0e-5f,
                  "negative and positive jitter translate by negative extent");
  EXPECT_NEAR_MSG(actualNegativePositive[1], expectedNegativePositive[1], 2.0e-5f,
                  "negative and positive jitter translate by negative extent");
  EXPECT_GT_MSG(actualNegativePositive[0], center[0],
                "negative x jitter has explicit sign");
  EXPECT_LT_MSG(actualNegativePositive[1], center[1],
                "positive y jitter has explicit sign");

  const hpl::TemporalReactiveMaskMathExtent zeroExtent = {};
  float zeroUV[2] = {1.0f, 1.0f};
  hpl::TemporalReactiveMaskUnjitteredSampleUV(0, 0, zeroExtent, zeroJitter,
                                              zeroUV);
  // A zero extent is a safe disabled/sentinel state and must never divide.
  EXPECT_EQ_MSG(zeroUV[0], 0.0f, "zero extent returns zero x UV without division");
  EXPECT_EQ_MSG(zeroUV[1], 0.0f, "zero extent returns zero y UV without division");
}
