#include "graphics/RayConeLod.h"
#include "utest.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

void CheckAssert(bool condition, int *utest_result, const char *name) {
  EXPECT_TRUE_MSG(condition, name);
}

void CheckFloatCloseAssert(float actual, float expected, float tolerance,
                           int *utest_result, const char *name) {
  EXPECT_NEAR_MSG(actual, expected, tolerance, name);
}

#define Check(condition, name)                                                 \
  ([&]() {                                                                     \
    const bool result = (condition);                                           \
    CheckAssert(result, utest_result, (name));                                 \
    return result;                                                              \
  }())
#define CheckFloatClose(actual, expected, tolerance, name)                     \
  ([&]() {                                                                     \
    const float actualValue = (actual);                                         \
    const float expectedValue = (expected);                                     \
    CheckFloatCloseAssert(actualValue, expectedValue, (tolerance),              \
                          utest_result, (name));                               \
    return std::fabs(actualValue - expectedValue) <= (tolerance);               \
  }())

bool CheckPrimarySpread(int *utest_result) {
  // 60 degrees is 1.04719755 radians. The literal result is the hand-worked
  // atan(2*tan(30 degrees)/1080), not a restatement of the implementation.
  return CheckFloatClose(
             hpl::RayConePrimarySpreadAngle(1.04719755f, 1080),
             0.0010691668f, 1.0e-8f,
             "60 degree vertical FOV at 1080 has the expected spread") &&
         CheckFloatClose(hpl::RayConePrimarySpreadAngle(1.0f, 0), 0.0f, 0.0f,
                         "zero render height has no spread") &&
         CheckFloatClose(hpl::RayConePrimarySpreadAngle(0.0f, 1080), 0.0f,
                         0.0f, "zero FOV has no spread") &&
         CheckFloatClose(hpl::RayConePrimarySpreadAngle(-1.0f, 1080), 0.0f,
                         0.0f, "negative FOV has no spread");
}

bool CheckSurfaceSpreadAngle(int *utest_result) {
  return CheckFloatClose(
             hpl::RayConeSurfaceSpreadAngle(0.0f, true), 0.0f, 0.0f,
             "perfect mirror has zero surface spread") &&
         CheckFloatClose(
             hpl::RayConeSurfaceSpreadAngle(0.04f, true), 0.079957374f,
             1.0e-7f, "minimum GGX alpha has the literal surface spread") &&
         CheckFloatClose(
             hpl::RayConeSurfaceSpreadAngle(0.5f, true), 0.927295218f, 1.0e-7f,
             "half GGX alpha has the literal surface spread") &&
         CheckFloatClose(
             hpl::RayConeSurfaceSpreadAngle(1.0f, true), 1.5707963268f, 0.0f,
             "fully rough specular spread reaches the hemisphere cap") &&
         CheckFloatClose(
             hpl::RayConeSurfaceSpreadAngle(0.0f, false), 1.5707963268f, 0.0f,
             "diffuse surface spread is independent of zero alpha") &&
         CheckFloatClose(
             hpl::RayConeSurfaceSpreadAngle(1.0f, false), 1.5707963268f, 0.0f,
             "diffuse surface spread is independent of unit alpha") &&
         Check(hpl::RayConeSurfaceSpreadAngle(0.0f, false) ==
                   hpl::kRayConeMaxSpreadAngle,
               "diffuse surface spread returns the named cap") &&
         Check(hpl::RayConeSurfaceSpreadAngle(1.0f, true) ==
                   hpl::RayConeSurfaceSpreadAngle(1.0f, false),
               "specular and diffuse surface spreads meet at alpha one") &&
         CheckFloatClose(
             hpl::RayConeSurfaceSpreadAngle(2.0f, true), 1.5707963268f, 0.0f,
             "GGX alpha above one clamps to the hemisphere cap") &&
         CheckFloatClose(
             hpl::RayConeSurfaceSpreadAngle(-1.0f, true), 0.0f, 0.0f,
             "negative GGX alpha clamps to zero spread");
}

bool CheckSurfaceSpreadMonotonic(int *utest_result) {
  const float alphas[] = {0.0f, 0.04f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f};
  float previousSpread = -1.0f;
  for (const float alpha : alphas) {
    const float spread = hpl::RayConeSurfaceSpreadAngle(alpha, true);
    if (!Check(spread > previousSpread,
               "specular surface spread strictly increases with alpha") ||
        !Check(spread <= hpl::kRayConeMaxSpreadAngle,
               "specular surface spread never exceeds the cap"))
      return false;
    previousSpread = spread;
  }
  return true;
}

bool CheckWaterGuideNormalSpreadAlpha(int *utest_result) {
  const float flatNormalSum[3] = {0.0f, 0.0f, 4.0f};
  const float nonFiniteNormalSum[3] = {
      0.0f, std::numeric_limits<float>::infinity(), 0.0f};
  const float thirtyDegreePairSum[3] = {0.0f, 0.0f, 1.7320508f};
  const float fullySpreadNormalSum[3] = {0.0f, 0.0f, 0.0f};

  const float flatAlpha =
      hpl::WaterGuideNormalSpreadAlpha(flatNormalSum, 4);
  const float thirtyDegreeAlpha =
      hpl::WaterGuideNormalSpreadAlpha(thirtyDegreePairSum, 2);
  const float cappedAlpha =
      hpl::WaterGuideNormalSpreadAlpha(fullySpreadNormalSum, 4);

  // For two normals at +/-30 degrees, R = cos(30 degrees) = 0.8660254.
  // Evaluating the specified sqrt and tangent gives this float literal.
  if (!CheckFloatClose(flatAlpha, 0.0f, 0.0f,
                       "identical guide normals have zero alpha") ||
      !Check(hpl::RayConeSurfaceSpreadAngle(flatAlpha, true) == 0.0f,
             "zero guide alpha adds no mirror spread") ||
      !CheckFloatClose(
          hpl::WaterGuideNormalSpreadAlpha(flatNormalSum, 0), 0.0f, 0.0f,
          "zero guide samples have zero alpha") ||
      !CheckFloatClose(
          hpl::WaterGuideNormalSpreadAlpha(flatNormalSum, 1), 0.0f, 0.0f,
          "one guide sample has zero alpha") ||
      !CheckFloatClose(
          hpl::WaterGuideNormalSpreadAlpha(nonFiniteNormalSum, 4), 0.0f,
          0.0f, "non-finite guide normal sums have zero alpha") ||
      !CheckFloatClose(thirtyDegreeAlpha, 0.569429934f, 1.0e-7f,
                       "thirty-degree normal spread has the literal alpha") ||
      !Check(cappedAlpha == 1.0f,
             "fully spread guide normals reach the alpha clamp") ||
      !Check(hpl::RayConeSurfaceSpreadAngle(cappedAlpha, true) ==
                 hpl::kRayConeMaxSpreadAngle,
             "clamped guide alpha reaches the hemisphere spread cap"))
    return false;

  const float resultantLengths[] = {4.0f, 3.6f, 3.2f, 2.8f, 0.0f};
  float previousAlpha = -1.0f;
  for (const float resultantLength : resultantLengths) {
    const float normalSum[3] = {0.0f, 0.0f, resultantLength};
    const float alpha = hpl::WaterGuideNormalSpreadAlpha(normalSum, 4);
    if (!Check(alpha > previousAlpha,
               "guide alpha strictly increases as resultant length decreases"))
      return false;
    previousAlpha = alpha;
  }
  return true;
}

bool CheckWaterNrdLinearRoughness(int *utest_result) {
  // This is the load-bearing mirror case: a flat guide must preserve the
  // previously hardcoded NRD mirror packing bit for bit.
  if (!CheckFloatClose(hpl::WaterNrdLinearRoughness(0.0f), 0.0f, 0.0f,
                       "zero guide alpha packs exact mirror roughness") ||
      !CheckFloatClose(hpl::WaterNrdLinearRoughness(1.0f), 1.0f, 0.0f,
                       "unit guide alpha packs unit linear roughness") ||
      !CheckFloatClose(hpl::WaterNrdLinearRoughness(0.25f), 0.5f, 0.0f,
                       "quarter guide alpha packs half linear roughness") ||
      !CheckFloatClose(
          hpl::WaterNrdLinearRoughness(0.5625f), 0.75f, 0.0f,
          "nine-sixteenths guide alpha packs three-quarter linear roughness") ||
      // This is sqrt of the pinned 30-degree alpha 0.569429934f.
      !CheckFloatClose(hpl::WaterNrdLinearRoughness(0.569429934f),
                       0.754605814f, 1.0e-7f,
                       "pinned thirty-degree alpha packs its square root") ||
      !CheckFloatClose(hpl::WaterNrdLinearRoughness(-1.0f), 0.0f, 0.0f,
                       "negative guide alpha clamps to mirror roughness") ||
      !CheckFloatClose(hpl::WaterNrdLinearRoughness(2.0f), 1.0f, 0.0f,
                       "guide alpha above one clamps to unit roughness") ||
      !CheckFloatClose(
          hpl::WaterNrdLinearRoughness(std::numeric_limits<float>::infinity()),
          0.0f, 0.0f, "infinite guide alpha packs mirror roughness") ||
      !CheckFloatClose(
          hpl::WaterNrdLinearRoughness(std::numeric_limits<float>::quiet_NaN()),
          0.0f, 0.0f, "NaN guide alpha packs mirror roughness"))
    return false;

  const float alphas[] = {0.01f, 0.1f, 0.25f, 0.5f, 0.75f, 0.99f};
  float previousRoughness = -1.0f;
  for (const float alpha : alphas) {
    const float roughness = hpl::WaterNrdLinearRoughness(alpha);
    if (!Check(roughness > previousRoughness,
               "NRD linear roughness strictly increases with guide alpha"))
      return false;
    previousRoughness = roughness;
  }
  return true;
}

bool CheckRayConeWidenSpreadAngle(int *utest_result) {
  const float nearMirrorSpread =
      hpl::RayConeSurfaceSpreadAngle(0.04f, true);
  return CheckFloatClose(
             hpl::RayConeWidenSpreadAngle(0.001f, nearMirrorSpread),
             0.0809573755f, 1.0e-7f,
             "near-mirror spread widens the camera cone by a literal amount") &&
         CheckFloatClose(
             hpl::RayConeWidenSpreadAngle(0.001f,
                                          hpl::kRayConeMaxSpreadAngle),
             1.5707963268f, 0.0f,
             "diffuse spread saturates at the hemisphere cap") &&
         Check(hpl::RayConeWidenSpreadAngle(
                   hpl::kRayConeMaxSpreadAngle,
                   hpl::kRayConeMaxSpreadAngle) ==
                   hpl::kRayConeMaxSpreadAngle,
               "widening a saturated cone is idempotent") &&
         CheckFloatClose(hpl::RayConeWidenSpreadAngle(0.37f, -0.2f), 0.37f,
                         0.0f, "negative surface spread does not shrink a cone");
}

bool CheckWidenedConeWidensFootprint(int *utest_result) {
  const float primarySpread =
      hpl::RayConePrimarySpreadAngle(1.04719755f, 1080);
  const float firstWidth = hpl::RayConeWidthAt(primarySpread, 0.0f, 10.0f);
  const float oldSecondWidth =
      hpl::RayConeWidthAt(primarySpread, firstWidth, 5.0f);
  const float diffuseSpread = hpl::RayConeWidenSpreadAngle(
      primarySpread, hpl::RayConeSurfaceSpreadAngle(0.0f, false));
  const float widenedSecondWidth =
      hpl::RayConeWidthAt(diffuseSpread, firstWidth, 5.0f);
  const float mirrorSpread = hpl::RayConeWidenSpreadAngle(
      primarySpread, hpl::RayConeSurfaceSpreadAngle(0.0f, true));
  const float mirrorSecondWidth =
      hpl::RayConeWidthAt(mirrorSpread, firstWidth, 5.0f);

  // The unchanged path is the old shipped behaviour: the first-hit width is
  // propagated another five world units with the original camera spread.
  // A diffuse bounce widens that spread before the second propagation, while
  // a perfect mirror contributes exactly zero and must reproduce the old
  // width bit-for-bit.
  return CheckFloatClose(firstWidth, 0.010691668f, 1.0e-7f,
                         "first-hit width is the literal primary footprint") &&
         CheckFloatClose(oldSecondWidth, 0.0160375014f, 1.0e-7f,
                         "old second-hit width is the literal baseline") &&
         CheckFloatClose(widenedSecondWidth, 7.8646736f, 1.0e-5f,
                         "diffuse second-hit width is the literal widened footprint") &&
         Check(widenedSecondWidth > oldSecondWidth * 100.0f,
               "diffuse bounce makes the second footprint much larger") &&
         Check(mirrorSecondWidth == oldSecondWidth,
               "mirror bounce reproduces the old second-hit width exactly") &&
         CheckFloatClose(mirrorSecondWidth, 0.0160375014f, 1.0e-7f,
                         "mirror second-hit width matches the literal baseline");
}

bool CheckWidthAndDistanceLod(int *utest_result) {
  const float nearWidth = hpl::RayConeWidthAt(0.001f, 0.0f, 8.0f);
  const float farWidth = hpl::RayConeWidthAt(0.001f, 0.0f, 16.0f);
  const float nearLod =
      hpl::RayConeTextureLod(0.0f, nearWidth, 1.0f, 1.0f, 1.0f, 0.0f);
  const float farLod =
      hpl::RayConeTextureLod(0.0f, farWidth, 1.0f, 1.0f, 1.0f, 0.0f);

  return CheckFloatClose(nearWidth, 0.008f, 1.0e-7f,
                         "ray-cone width at the near hit is literal") &&
         CheckFloatClose(farWidth, 0.016f, 1.0e-7f,
                         "ray-cone width at the far hit is literal") &&
         // Doubling the hit distance doubles the width here, costing exactly
         // one mip in the logarithmic texture LOD.
         CheckFloatClose(farLod - nearLod, 1.0f, 1.0e-6f,
                         "doubling hit distance costs one mip");
}

bool CheckTriangleLodConstant(int *utest_result) {
  const float ratioOne = hpl::RayConeTriangleLodConstant(1.0f, 1.0f);
  const float ratioFour = hpl::RayConeTriangleLodConstant(4.0f, 1.0f);
  return CheckFloatClose(ratioOne, 0.0f, 0.0f,
                         "unit UV-to-world area has no triangle bias") &&
         CheckFloatClose(ratioFour, 1.0f, 0.0f,
                         "quadrupled UV-to-world area adds one mip") &&
         CheckFloatClose(ratioFour - ratioOne, 1.0f, 0.0f,
                         "quadrupling the area ratio costs one mip");
}

bool CheckTextureLod(int *utest_result) {
  const float noBias =
      hpl::RayConeTextureLod(0.0f, 1.0f, 1.0f, 1024.0f, 1024.0f, 0.0f);
  const float negativeBias =
      hpl::RayConeTextureLod(0.0f, 1.0f, 1.0f, 1024.0f, 1024.0f, -1.0f);
  return CheckFloatClose(noBias, 10.0f, 0.0f,
                         "1024 by 1024 contributes ten mips") &&
         CheckFloatClose(negativeBias, 9.0f, 0.0f,
                         "minus one material bias lowers the literal LOD") &&
         CheckFloatClose(negativeBias - noBias, -1.0f, 0.0f,
                         "material mip bias adds through exactly");
}

bool CheckGrazingCosineFloor(int *utest_result) {
  const float zeroCosine =
      hpl::RayConeTextureLod(0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f);
  const float floorCosine =
      hpl::RayConeTextureLod(0.0f, 1.0f, 0.05f, 1.0f, 1.0f, 0.0f);
  return CheckFloatClose(zeroCosine, floorCosine, 0.0f,
                         "zero and floor cosine produce the same LOD") &&
         CheckFloatClose(zeroCosine, 4.321928f, 1.0e-6f,
                         "grazing cosine boost is bounded at about 4.3 mips") &&
         Check(zeroCosine < 4.322f, "grazing cosine boost is bounded");
}

bool CheckSurfaceFootprint(int *utest_result) {
  // These literal diameters are hand-worked from the cone width and cosine,
  // not a restatement of the implementation.
  const float headOn = hpl::RayConeSurfaceFootprint(2.0f, 1.0f);
  const float halfCosine = hpl::RayConeSurfaceFootprint(2.0f, 0.5f);
  const float zeroCosine = hpl::RayConeSurfaceFootprint(2.0f, 0.0f);
  const float floorCosine = hpl::RayConeSurfaceFootprint(2.0f, 0.05f);

  return CheckFloatClose(headOn, 2.0f, 0.0f,
                         "head-on surface footprint keeps cone width") &&
         CheckFloatClose(halfCosine, 4.0f, 0.0f,
                         "half cosine doubles surface footprint") &&
         CheckFloatClose(zeroCosine, 40.0f, 1.0e-6f,
                         "zero cosine is bounded by the floor") &&
         CheckFloatClose(floorCosine, 40.0f, 1.0e-6f,
                         "floor cosine has the bounded footprint") &&
         CheckFloatClose(zeroCosine, floorCosine, 0.0f,
                         "zero and floor cosine have the same footprint");
}

static void DistortWaterWave(float u, float v, float amplitude,
                             float frequency, float phase, float outUv[2]) {
  outUv[0] = u + amplitude * std::sin(phase + v * frequency);
  outUv[1] = v + amplitude * std::sin(phase + outUv[0] * frequency);
}

static float WaterWaveGradientPairMagnitude(const float gradientX[2],
                                            const float gradientY[2]) {
  return std::sqrt(gradientX[0] * gradientX[0] +
                   gradientX[1] * gradientX[1] +
                   gradientY[0] * gradientY[0] +
                   gradientY[1] * gradientY[1]);
}

static bool CheckWaterWaveUvJacobianIdentity(int *utest_result) {
  const float baseGradient[2] = {0.37f, -0.23f};
  float amplitudeZeroJacobian[4] = {};
  float frequencyZeroJacobian[4] = {};
  float amplitudeZeroGradient[2] = {};
  float frequencyZeroGradient[2] = {};
  hpl::WaterWaveUvJacobian(0.0f, 3.0f, 0.8f, -0.4f, 0.25f,
                           amplitudeZeroJacobian);
  hpl::WaterWaveUvJacobian(0.2f, 0.0f, 0.8f, -0.4f, 0.25f,
                           frequencyZeroJacobian);
  hpl::WaterWaveUvGradient(amplitudeZeroJacobian, baseGradient,
                           amplitudeZeroGradient);
  hpl::WaterWaveUvGradient(frequencyZeroJacobian, baseGradient,
                           frequencyZeroGradient);

  return CheckFloatClose(amplitudeZeroJacobian[0], 1.0f, 0.0f,
                         "zero water-wave amplitude has unit J00") &&
         CheckFloatClose(amplitudeZeroJacobian[1], 0.0f, 0.0f,
                         "zero water-wave amplitude has zero J01") &&
         CheckFloatClose(amplitudeZeroJacobian[2], 0.0f, 0.0f,
                         "zero water-wave amplitude has zero J10") &&
         CheckFloatClose(amplitudeZeroJacobian[3], 1.0f, 0.0f,
                         "zero water-wave amplitude has unit J11") &&
         CheckFloatClose(amplitudeZeroGradient[0], baseGradient[0], 0.0f,
                         "zero water-wave amplitude preserves gradient x") &&
         CheckFloatClose(amplitudeZeroGradient[1], baseGradient[1], 0.0f,
                         "zero water-wave amplitude preserves gradient y") &&
         CheckFloatClose(frequencyZeroJacobian[0], 1.0f, 0.0f,
                         "zero water-wave frequency has unit J00") &&
         CheckFloatClose(frequencyZeroJacobian[1], 0.0f, 0.0f,
                         "zero water-wave frequency has zero J01") &&
         CheckFloatClose(frequencyZeroJacobian[2], 0.0f, 0.0f,
                         "zero water-wave frequency has zero J10") &&
         CheckFloatClose(frequencyZeroJacobian[3], 1.0f, 0.0f,
                         "zero water-wave frequency has unit J11") &&
         CheckFloatClose(frequencyZeroGradient[0], baseGradient[0], 0.0f,
                         "zero water-wave frequency preserves gradient x") &&
         CheckFloatClose(frequencyZeroGradient[1], baseGradient[1], 0.0f,
                         "zero water-wave frequency preserves gradient y");
}

static bool CheckWaterWaveUvJacobianHandWorked(int *utest_result) {
  const float baseGradient[2] = {2.0f, 3.0f};
  float jacobian[4] = {};
  float sampleGradient[2] = {};

  hpl::WaterWaveUvJacobian(0.25f, 2.0f, 0.0f, 0.0f, 0.0f, jacobian);
  hpl::WaterWaveUvGradient(jacobian, baseGradient, sampleGradient);

  // A*F*cos(0 + 0*F) = 0.25*2*1 = 0.5 for both entries, so
  // J = [1, 0.5; 0.5, 1 + 0.5*0.5 = 1.25]. Applying it to [2, 3]
  // gives [1*2 + 0.5*3 = 3.5, 0.5*2 + 1.25*3 = 4.75].
  return CheckFloatClose(jacobian[1], 0.5f, 0.0f,
                         "hand-worked water-wave J01 is literal") &&
         CheckFloatClose(jacobian[2], 0.5f, 0.0f,
                         "hand-worked water-wave J10 is literal") &&
         CheckFloatClose(jacobian[3], 1.25f, 0.0f,
                         "hand-worked water-wave J11 is literal") &&
         CheckFloatClose(sampleGradient[0], 3.5f, 0.0f,
                         "hand-worked water-wave gradient x is literal") &&
         CheckFloatClose(sampleGradient[1], 4.75f, 0.0f,
                         "hand-worked water-wave gradient y is literal");
}

static bool CheckWaterWaveUvJacobianFiniteDifference(int *utest_result) {
  const float u = 0.37f;
  const float v = -0.22f;
  const float amplitude = 0.17f;
  const float frequency = 2.3f;
  const float phase = 0.63f;
  const float step = 1.0e-3f;
  const float baseGradient[2] = {0.31f, -0.27f};
  float centreUv[2] = {};
  float analyticJacobian[4] = {};
  float analyticGradient[2] = {};
  float plusU[2] = {};
  float minusU[2] = {};
  float plusV[2] = {};
  float minusV[2] = {};

  DistortWaterWave(u, v, amplitude, frequency, phase, centreUv);
  hpl::WaterWaveUvJacobian(amplitude, frequency, phase, v, centreUv[0],
                           analyticJacobian);
  hpl::WaterWaveUvGradient(analyticJacobian, baseGradient, analyticGradient);
  DistortWaterWave(u + step, v, amplitude, frequency, phase, plusU);
  DistortWaterWave(u - step, v, amplitude, frequency, phase, minusU);
  DistortWaterWave(u, v + step, amplitude, frequency, phase, plusV);
  DistortWaterWave(u, v - step, amplitude, frequency, phase, minusV);

  const float finiteDifference[4] = {
      (plusU[0] - minusU[0]) / (2.0f * step),
      (plusV[0] - minusV[0]) / (2.0f * step),
      (plusU[1] - minusU[1]) / (2.0f * step),
      (plusV[1] - minusV[1]) / (2.0f * step)};
  const float finiteDifferenceGradient[2] = {
      finiteDifference[0] * baseGradient[0] +
          finiteDifference[1] * baseGradient[1],
      finiteDifference[2] * baseGradient[0] +
          finiteDifference[3] * baseGradient[1]};
  const float tolerance = 3.0e-4f;

  return CheckFloatClose(analyticJacobian[0], finiteDifference[0], tolerance,
                         "water-wave J00 agrees with finite difference") &&
         CheckFloatClose(analyticJacobian[1], finiteDifference[1], tolerance,
                         "water-wave J01 agrees with finite difference") &&
         CheckFloatClose(analyticJacobian[2], finiteDifference[2], tolerance,
                         "water-wave J10 agrees with finite difference") &&
         CheckFloatClose(analyticJacobian[3], finiteDifference[3], tolerance,
                         "water-wave J11 agrees with finite difference") &&
         CheckFloatClose(analyticGradient[0], finiteDifferenceGradient[0],
                         tolerance,
                         "water-wave gradient x agrees with finite difference") &&
         CheckFloatClose(analyticGradient[1], finiteDifferenceGradient[1],
                         tolerance,
                         "water-wave gradient y agrees with finite difference");
}

static bool CheckWaterWaveSampleGradients(int *utest_result) {
  // These are the shader's two water samples at waveT = 0.91, including their
  // distinct scrolls and phases. The material values are scaled exactly as in
  // WaterCommon.slang: A = 0.2 * 0.04 and F = 1 * 10.
  const float baseUv[2] = {0.37f, 0.41f};
  const float waveT = 0.91f;
  const float amplitude = 0.2f * 0.04f;
  const float frequency = 1.0f * 10.0f;
  const float baseGradientX[2] = {1.0f / 1920.0f, 0.0f};
  const float baseGradientY[2] = {0.0f, 1.0f / 1080.0f};
  const float baseMagnitude = WaterWaveGradientPairMagnitude(
      baseGradientX, baseGradientY);
  const float sample1U = baseUv[0] + waveT * 0.01f;
  const float sample1V = baseUv[1] + waveT * 0.01f;
  const float sample2U = baseUv[0] - waveT * 0.012f;
  const float sample2V = baseUv[1] - waveT * 0.012f;
  float sample1Uv[2] = {};
  float sample2Uv[2] = {};
  float sample1Jacobian[4] = {};
  float sample2Jacobian[4] = {};
  float sample1GradientX[2] = {};
  float sample1GradientY[2] = {};
  float sample2GradientX[2] = {};
  float sample2GradientY[2] = {};

  DistortWaterWave(sample1U, sample1V, amplitude, frequency, waveT * 0.8f,
                   sample1Uv);
  DistortWaterWave(sample2U, sample2V, amplitude * 0.75f, frequency * 1.2f,
                   waveT * -2.6f, sample2Uv);
  hpl::WaterWaveUvJacobian(amplitude, frequency, waveT * 0.8f, sample1V,
                           sample1Uv[0], sample1Jacobian);
  hpl::WaterWaveUvJacobian(amplitude * 0.75f, frequency * 1.2f,
                           waveT * -2.6f, sample2V, sample2Uv[0],
                           sample2Jacobian);
  hpl::WaterWaveUvGradient(sample1Jacobian, baseGradientX, sample1GradientX);
  hpl::WaterWaveUvGradient(sample1Jacobian, baseGradientY, sample1GradientY);
  hpl::WaterWaveUvGradient(sample2Jacobian, baseGradientX, sample2GradientX);
  hpl::WaterWaveUvGradient(sample2Jacobian, baseGradientY, sample2GradientY);

  const float sample1Magnitude = WaterWaveGradientPairMagnitude(
      sample1GradientX, sample1GradientY);
  const float sample2Magnitude = WaterWaveGradientPairMagnitude(
      sample2GradientX, sample2GradientY);
  const float sample1LodShift = std::log2(sample1Magnitude / baseMagnitude);
  const float sample2LodShift = std::log2(sample2Magnitude / baseMagnitude);
  const bool gradientsDiffer =
      std::fabs(sample1GradientX[0] - sample2GradientX[0]) > 1.0e-8f ||
      std::fabs(sample1GradientX[1] - sample2GradientX[1]) > 1.0e-8f ||
      std::fabs(sample1GradientY[0] - sample2GradientY[0]) > 1.0e-8f ||
      std::fabs(sample1GradientY[1] - sample2GradientY[1]) > 1.0e-8f;

  return Check(gradientsDiffer,
               "water samples produce different gradients") &&
         CheckFloatClose(sample1Magnitude / baseMagnitude, 1.0f, 0.03f,
                         "first water gradient stays within three percent") &&
         CheckFloatClose(sample2Magnitude / baseMagnitude, 1.0f, 0.03f,
                         "second water gradient stays within three percent") &&
         CheckFloatClose(sample1LodShift, 0.0f, 0.25f,
                         "first water gradient shifts less than a quarter mip") &&
         CheckFloatClose(sample2LodShift, 0.0f, 0.25f,
                         "second water gradient shifts less than a quarter mip");
}

bool CheckProjectedGoboUvPerWorldUnit(int *utest_result) {
  const float orthographicX[4] = {0.5f, 0.0f, 0.0f, 0.5f};
  const float orthographicY[4] = {0.0f, 0.5f, 0.0f, 0.5f};
  const float orthographicW[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float orthographicPosition[3] = {3.0f, -2.0f, 5.0f};

  const float perspectiveX[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  const float perspectiveY[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  const float perspectiveW[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  const float perspectiveNearPosition[3] = {0.0f, 0.0f, 2.0f};
  const float perspectiveFarPosition[3] = {0.0f, 0.0f, 4.0f};
  const float onLightPlane[3] = {0.0f, 0.0f, 0.0f};
  const float behindLight[3] = {0.0f, 0.0f, -1.0f};

  // These rates are hand-worked from the projective rows, not a restatement of
  // the implementation: orthographic rates are 0.5, while depths 2 and 4
  // give perspective rates 1/2 and 1/4.
  return CheckFloatClose(
             hpl::ProjectedGoboUvPerWorldUnit(
                 orthographicX, orthographicY, orthographicW,
                 orthographicPosition),
             0.5f, 0.0f, "orthographic gobo projection has half UV rate") &&
         CheckFloatClose(
             hpl::ProjectedGoboUvPerWorldUnit(
                 perspectiveX, perspectiveY, perspectiveW,
                 perspectiveNearPosition),
             0.5f, 0.0f, "perspective gobo rate at depth two is literal") &&
         CheckFloatClose(
             hpl::ProjectedGoboUvPerWorldUnit(
                 perspectiveX, perspectiveY, perspectiveW,
                 perspectiveFarPosition),
             0.25f, 0.0f, "doubling perspective depth halves gobo rate") &&
         CheckFloatClose(
             hpl::ProjectedGoboUvPerWorldUnit(
                 perspectiveX, perspectiveY, perspectiveW, onLightPlane),
             0.0f, 0.0f, "gobo point on light plane has no UV rate") &&
         CheckFloatClose(
             hpl::ProjectedGoboUvPerWorldUnit(
                 perspectiveX, perspectiveY, perspectiveW, behindLight),
             0.0f, 0.0f, "gobo point behind light has no UV rate");
}

bool CheckProjectedGoboTexLodBase(int *utest_result) {
  // These literal LODs are hand-worked from the UV diameters, not a
  // restatement of the implementation: 0.5 * 0.5 = 0.25, whose log2 is -2.
  const float base = hpl::ProjectedGoboTexLodBase(0.5f, 0.5f);
  const float doubledFootprint = hpl::ProjectedGoboTexLodBase(1.0f, 0.5f);
  const float zeroFootprint = hpl::ProjectedGoboTexLodBase(0.0f, 0.5f);
  const float negativeFootprint = hpl::ProjectedGoboTexLodBase(-1.0f, 0.5f);
  const float zeroRate = hpl::ProjectedGoboTexLodBase(0.5f, 0.0f);
  const float negativeRate = hpl::ProjectedGoboTexLodBase(0.5f, -1.0f);

  // The named -64 escape hatch means top mip; zero would mean a mid mip after
  // the sampler completes the texture-size-dependent term.
  return CheckFloatClose(base, -2.0f, 0.0f,
                         "quarter UV diameter has base minus two") &&
         CheckFloatClose(doubledFootprint, -1.0f, 0.0f,
                         "doubling gobo footprint adds one mip") &&
         CheckFloatClose(doubledFootprint - base, 1.0f, 0.0f,
                         "doubling gobo footprint changes one mip") &&
         CheckFloatClose(zeroFootprint, -64.0f, 0.0f,
                         "zero gobo footprint uses top-mip escape hatch") &&
         CheckFloatClose(negativeFootprint, -64.0f, 0.0f,
                         "negative gobo footprint uses top-mip escape hatch") &&
         CheckFloatClose(zeroRate, -64.0f, 0.0f,
                         "zero gobo rate uses top-mip escape hatch") &&
         CheckFloatClose(negativeRate, -64.0f, 0.0f,
                         "negative gobo rate uses top-mip escape hatch") &&
         Check(zeroFootprint == -64.0f && zeroFootprint != 0.0f,
               "top-mip escape hatch is not zero");
}

bool CheckCubeFaceUvPerRadian(int *utest_result) {
  const float axisDirection[3] = {0.0f, 0.0f, 1.0f};
  const float scaledAxisDirection[3] = {0.0f, 0.0f, 2.0f};
  const float faceEdgeDirection[3] = {1.0f, 0.0f, 1.0f};
  const float faceCornerDirection[3] = {1.0f, 1.0f, 1.0f};
  const float zeroDirection[3] = {0.0f, 0.0f, 0.0f};

  // These literal rates are hand-worked from the cube-face projection, not a
  // restatement of the implementation: the axis, edge, and corner rates are
  // 0.5, 1.0, and 1.5, and scaling a direction does not change its rate.
  return CheckFloatClose(hpl::CubeFaceUvPerRadian(axisDirection), 0.5f, 0.0f,
                         "cube-face axis has half UV rate") &&
         CheckFloatClose(hpl::CubeFaceUvPerRadian(scaledAxisDirection), 0.5f,
                         0.0f, "cube-face rate is scale invariant") &&
         CheckFloatClose(hpl::CubeFaceUvPerRadian(faceEdgeDirection), 1.0f,
                         0.0f, "cube-face edge has unit UV rate") &&
         CheckFloatClose(hpl::CubeFaceUvPerRadian(faceCornerDirection), 1.5f,
                         0.0f, "cube-face corner has one-and-a-half UV rate") &&
         CheckFloatClose(hpl::CubeFaceUvPerRadian(zeroDirection), 0.0f, 0.0f,
                         "zero cube-face direction has no UV rate");
}

bool CheckCubeGoboTexLodBase(int *utest_result) {
  const float axisDirection[3] = {0.0f, 0.0f, 1.0f};
  const float cornerDirection[3] = {1.0f, 1.0f, 1.0f};
  const float zeroDirection[3] = {0.0f, 0.0f, 0.0f};
  const float axisBase =
      hpl::CubeGoboTexLodBase(1.0f, 4.0f, axisDirection);
  const float cornerBase =
      hpl::CubeGoboTexLodBase(1.0f, 2.0f, cornerDirection);
  const float halfFootprintBase =
      hpl::CubeGoboTexLodBase(0.5f, 4.0f, axisDirection);
  const float zeroDistance =
      hpl::CubeGoboTexLodBase(1.0f, 0.0f, axisDirection);
  const float negativeDistance =
      hpl::CubeGoboTexLodBase(1.0f, -1.0f, axisDirection);
  const float zeroFootprint =
      hpl::CubeGoboTexLodBase(0.0f, 4.0f, axisDirection);
  const float negativeFootprint =
      hpl::CubeGoboTexLodBase(-1.0f, 4.0f, axisDirection);
  const float zeroDirectionBase =
      hpl::CubeGoboTexLodBase(1.0f, 4.0f, zeroDirection);

  // These literal LODs are hand-worked from the subtended UV diameters, not a
  // restatement of the implementation: 0.25*0.5 = 0.125 (-3 mips), while
  // 0.5*1.5 = 0.75 (log2(0.75) = -0.4150375), and halving the diameter costs
  // one mip.
  return CheckFloatClose(axisBase, -3.0f, 0.0f,
                         "axis cube-gobo footprint has base minus three") &&
         CheckFloatClose(cornerBase, -0.4150375f, 1.0e-7f,
                         "corner cube-gobo footprint has the literal base") &&
         CheckFloatClose(halfFootprintBase, -4.0f, 0.0f,
                         "halved cube-gobo footprint has base minus four") &&
         CheckFloatClose(halfFootprintBase - axisBase, -1.0f, 0.0f,
                         "halving cube-gobo footprint costs one mip") &&
         CheckFloatClose(zeroDistance, hpl::kTexLodTopMipBase, 0.0f,
                         "zero cube-gobo distance uses top-mip escape hatch") &&
         CheckFloatClose(negativeDistance, hpl::kTexLodTopMipBase, 0.0f,
                         "negative cube-gobo distance uses top-mip escape hatch") &&
         CheckFloatClose(zeroFootprint, hpl::kTexLodTopMipBase, 0.0f,
                         "zero cube-gobo footprint uses top-mip escape hatch") &&
         CheckFloatClose(negativeFootprint, hpl::kTexLodTopMipBase, 0.0f,
                         "negative cube-gobo footprint uses top-mip escape hatch") &&
         CheckFloatClose(zeroDirectionBase, hpl::kTexLodTopMipBase, 0.0f,
                         "zero cube-gobo direction uses top-mip escape hatch") &&
         Check(zeroDistance == hpl::kTexLodTopMipBase &&
                   zeroFootprint == hpl::kTexLodTopMipBase &&
                   zeroDirectionBase == hpl::kTexLodTopMipBase &&
                   hpl::kTexLodTopMipBase != 0.0f,
               "cube-gobo top-mip escape hatch is not zero");
}

bool CheckDecalUvPerWorldUnit(int *utest_result) {
  const float identityX[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  const float identityZ[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  const float halfScaleX[4] = {0.5f, 0.0f, 0.0f, 0.0f};
  const float halfScaleZ[4] = {0.0f, 0.0f, 0.5f, 0.0f};
  const float zeroZ[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  // These literal rates are hand-worked from the row lengths and atlas cells,
  // not a restatement of the implementation: unit rows give 1, while rows
  // of length 0.5 give half the UV rate.
  return CheckFloatClose(
             hpl::DecalUvPerWorldUnit(identityX, identityZ, 1, 1), 1.0f, 0.0f,
             "identity decal box has unit UV rate") &&
         CheckFloatClose(
             hpl::DecalUvPerWorldUnit(halfScaleX, halfScaleZ, 1, 1), 0.5f,
             0.0f, "scaled-up decal box halves the UV rate") &&
         CheckFloatClose(
             hpl::DecalUvPerWorldUnit(identityX, identityZ, 0, 0), 1.0f, 0.0f,
             "zero decal subdivisions behave as a 1x1 grid") &&
         CheckFloatClose(
             hpl::DecalUvPerWorldUnit(identityX, identityZ, 4, 1), 1.0f,
             0.0f, "decal UV rate takes the larger atlas axis") &&
         CheckFloatClose(
             hpl::DecalUvPerWorldUnit(identityX, zeroZ, 4, 1), 0.25f, 0.0f,
             "four horizontal decal cells quarter the horizontal rate");
}

bool CheckDecalTexLodBase(int *utest_result) {
  // These literal LODs are hand-worked from the UV diameters and atlas cells,
  // not a restatement of the implementation: unit footprint/rate is 0, a
  // half rate is -1, and a four-cell cap is -2 after the raw +2 rate case.
  const float identity = hpl::DecalTexLodBase(1.0f, 1.0f, 1, 1);
  const float scaledBox = hpl::DecalTexLodBase(1.0f, 0.5f, 1, 1);
  const float atlasRate = hpl::DecalTexLodBase(1.0f, 4.0f, 4, 1);
  const float coarseFootprint = hpl::DecalTexLodBase(16.0f, 1.0f, 4, 1);
  const float oneByOneCap = hpl::DecalAtlasMaxTexLodBase(1, 1);
  const float fourByOneCap = hpl::DecalAtlasMaxTexLodBase(4, 1);
  const float zeroSubdivisions = hpl::DecalTexLodBase(1.0f, 1.0f, 0, 0);
  const float zeroFootprint = hpl::DecalTexLodBase(0.0f, 1.0f, 1, 1);
  const float negativeFootprint = hpl::DecalTexLodBase(-1.0f, 1.0f, 1, 1);
  const float zeroRate = hpl::DecalTexLodBase(1.0f, 0.0f, 1, 1);
  const float negativeRate = hpl::DecalTexLodBase(1.0f, -1.0f, 1, 1);

  return CheckFloatClose(identity, 0.0f, 0.0f,
                         "identity decal footprint has zero base") &&
         CheckFloatClose(scaledBox, -1.0f, 0.0f,
                         "halving decal UV rate costs one mip") &&
         CheckFloatClose(atlasRate, -2.0f, 0.0f,
                         "four-cell decal atlas caps the base at minus two") &&
         CheckFloatClose(coarseFootprint, -2.0f, 0.0f,
                         "coarse decal footprint is capped at the atlas cell") &&
         CheckFloatClose(oneByOneCap, 0.0f, 0.0f,
                         "1x1 decal atlas cap is the 1x1 mip") &&
         CheckFloatClose(fourByOneCap, -2.0f, 0.0f,
                         "four-cell decal atlas cap is minus two") &&
         CheckFloatClose(zeroSubdivisions, 0.0f, 0.0f,
                         "zero decal subdivisions leave the 1x1 cap unchanged") &&
         CheckFloatClose(zeroFootprint, -64.0f, 0.0f,
                         "zero decal footprint uses top-mip escape hatch") &&
         CheckFloatClose(negativeFootprint, -64.0f, 0.0f,
                         "negative decal footprint uses top-mip escape hatch") &&
         CheckFloatClose(zeroRate, -64.0f, 0.0f,
                         "zero decal rate uses top-mip escape hatch") &&
         CheckFloatClose(negativeRate, -64.0f, 0.0f,
                         "negative decal rate uses top-mip escape hatch") &&
         Check(zeroFootprint == -64.0f && zeroFootprint != 0.0f,
               "decal top-mip escape hatch is not zero");
}

bool CheckAreaLightSourceTexLodBase(int *utest_result) {
  // These literal LODs are hand-worked, not a restatement of the
  // implementation: distance 2 over the smaller extent 4 is 0.5 (-1 mip),
  // while footprint 8 over 4 is 2 (+1 mip).
  const float distanceDominated =
      hpl::AreaLightSourceTexLodBase(0.1f, 2.0f, 4.0f, 8.0f);
  const float footprintDominated =
      hpl::AreaLightSourceTexLodBase(8.0f, 2.0f, 4.0f, 8.0f);
  const float doubledDistance =
      hpl::AreaLightSourceTexLodBase(0.1f, 4.0f, 4.0f, 8.0f);
  const float degenerateRect =
      hpl::AreaLightSourceTexLodBase(1.0f, 1.0f, 0.0f, 8.0f);

  return CheckFloatClose(distanceDominated, -1.0f, 0.0f,
                         "distance-dominated area-light LOD is literal") &&
         CheckFloatClose(footprintDominated, 1.0f, 0.0f,
                         "footprint-dominated area-light LOD is literal") &&
         CheckFloatClose(doubledDistance - distanceDominated, 1.0f, 0.0f,
                         "doubling light distance adds one mip") &&
         CheckFloatClose(degenerateRect, -64.0f, 0.0f,
                         "degenerate area-light extent uses top-mip escape hatch");
}

bool CheckDegenerateGuards(int *utest_result) {
  return CheckFloatClose(
             hpl::RayConeTriangleLodConstant(0.0f, 1.0f), 0.0f, 0.0f,
             "zero UV area returns zero") &&
         CheckFloatClose(
             hpl::RayConeTriangleLodConstant(-1.0f, 1.0f), 0.0f, 0.0f,
             "negative UV area returns zero") &&
         CheckFloatClose(
             hpl::RayConeTriangleLodConstant(1.0f, 0.0f), 0.0f, 0.0f,
             "zero world area returns zero") &&
         CheckFloatClose(
             hpl::RayConeTriangleLodConstant(1.0f, -1.0f), 0.0f, 0.0f,
             "negative world area returns zero") &&
         CheckFloatClose(
             hpl::RayConeTextureLod(2.0f, 1.0f, 1.0f, 0.0f, 1024.0f, 3.0f),
             0.0f, 0.0f, "zero texture width returns zero") &&
         CheckFloatClose(
             hpl::RayConeTextureLod(2.0f, 1.0f, 1.0f, -1.0f, 1024.0f, 3.0f),
             0.0f, 0.0f, "negative texture width returns zero") &&
         CheckFloatClose(
             hpl::RayConeTextureLod(2.0f, 1.0f, 1.0f, 1024.0f, 0.0f, 3.0f),
             0.0f, 0.0f, "zero texture height returns zero") &&
         CheckFloatClose(
             hpl::RayConeTextureLod(2.0f, 1.0f, 1.0f, 1024.0f, -1.0f, 3.0f),
             0.0f, 0.0f, "negative texture height returns zero");
}

} // namespace

UTEST(RayConeLod, PrimarySpread) { CheckPrimarySpread(utest_result); }
UTEST(RayConeLod, SurfaceSpreadAngle) {
  CheckSurfaceSpreadAngle(utest_result);
}
UTEST(RayConeLod, SurfaceSpreadMonotonic) {
  CheckSurfaceSpreadMonotonic(utest_result);
}
UTEST(RayConeLod, WaterGuideNormalSpreadAlpha) {
  CheckWaterGuideNormalSpreadAlpha(utest_result);
}
UTEST(RayConeLod, WaterNrdLinearRoughness) {
  CheckWaterNrdLinearRoughness(utest_result);
}
UTEST(RayConeLod, RayConeWidenSpreadAngle) {
  CheckRayConeWidenSpreadAngle(utest_result);
}
UTEST(RayConeLod, WidenedConeWidensFootprint) {
  CheckWidenedConeWidensFootprint(utest_result);
}
UTEST(RayConeLod, WidthAndDistanceLod) {
  CheckWidthAndDistanceLod(utest_result);
}
UTEST(RayConeLod, TriangleLodConstant) {
  CheckTriangleLodConstant(utest_result);
}
UTEST(RayConeLod, TextureLod) { CheckTextureLod(utest_result); }
UTEST(RayConeLod, GrazingCosineFloor) {
  CheckGrazingCosineFloor(utest_result);
}
UTEST(RayConeLod, SurfaceFootprint) {
  CheckSurfaceFootprint(utest_result);
}
UTEST(RayConeLod, WaterWaveUvJacobianIdentity) {
  CheckWaterWaveUvJacobianIdentity(utest_result);
}
UTEST(RayConeLod, WaterWaveUvJacobianHandWorked) {
  CheckWaterWaveUvJacobianHandWorked(utest_result);
}
UTEST(RayConeLod, WaterWaveUvJacobianFiniteDifference) {
  CheckWaterWaveUvJacobianFiniteDifference(utest_result);
}
UTEST(RayConeLod, WaterWaveSampleGradients) {
  CheckWaterWaveSampleGradients(utest_result);
}
UTEST(RayConeLod, ProjectedGoboUvPerWorldUnit) {
  CheckProjectedGoboUvPerWorldUnit(utest_result);
}
UTEST(RayConeLod, ProjectedGoboTexLodBase) {
  CheckProjectedGoboTexLodBase(utest_result);
}
UTEST(RayConeLod, CubeFaceUvPerRadian) {
  CheckCubeFaceUvPerRadian(utest_result);
}
UTEST(RayConeLod, CubeGoboTexLodBase) {
  CheckCubeGoboTexLodBase(utest_result);
}
UTEST(RayConeLod, DecalUvPerWorldUnit) {
  CheckDecalUvPerWorldUnit(utest_result);
}
UTEST(RayConeLod, DecalTexLodBase) {
  CheckDecalTexLodBase(utest_result);
}
UTEST(RayConeLod, AreaLightSourceTexLodBase) {
  CheckAreaLightSourceTexLodBase(utest_result);
}
UTEST(RayConeLod, DegenerateGuards) { CheckDegenerateGuards(utest_result); }
