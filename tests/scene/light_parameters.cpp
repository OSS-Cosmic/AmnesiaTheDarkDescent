#include "scene/LightParameters.h"
#include "utest.h"

#include <cmath>
#include <limits>

// The contract under test: ONE light element carries both backends' tuning.
// The retail attributes drive Standard; the ray-traced backend reads the same
// attribute spelled "Re_<Name>" when the element authors one, and falls back to
// promoting the retail Radius when it does not.
//
// These tests pin the half of that rule that lives in LightParameters: the
// element table, the class choice, and the three parameter resolvers. The
// attribute-name resolution itself lives in cLightElementAttributes
// (EngineFileLoading.cpp), which cannot be linked without the engine; the
// schema of the shipped data is covered by tests/light_override_schema_test.py.

using namespace hpl;

namespace {
bool NearlyEqual(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::fmax(1.0f, std::fabs(b)); }
} // namespace

UTEST(LightElementInfo, LegacyShapesAreNotRayTracedOnly) {
  const cLightElementInfo point = GetLightElementInfo("PointLight");
  ASSERT_TRUE(point.mbValid);
  ASSERT_EQ(eLightElementShape_Point, point.mShape);
  ASSERT_FALSE(point.mbRayTracedOnly);

  const cLightElementInfo box = GetLightElementInfo("BoxLight");
  ASSERT_TRUE(box.mbValid);
  ASSERT_EQ(eLightElementShape_Box, box.mShape);
  ASSERT_FALSE(box.mbRayTracedOnly);
}

UTEST(LightElementInfo, AreaLightIsRayTracedOnly) {
  const cLightElementInfo area = GetLightElementInfo("AreaLight");
  ASSERT_TRUE(area.mbValid);
  ASSERT_EQ(eLightElementShape_Area, area.mShape);
  // No legacy class, so its unprefixed attributes already are the ray-traced
  // schema and it never loads on Standard.
  ASSERT_TRUE(area.mbRayTracedOnly);
  ASSERT_EQ(kRendererMaskRayTraced, GetDefaultLightRendererMask(area));
}

UTEST(LightElementInfo, LegacyShapeDefaultsToBothBackends) {
  // This is what lets a merged light drop RendererMask entirely: retail values
  // on Standard, Re_* overrides on ray-traced, from one element.
  ASSERT_EQ(kRendererMaskAll, GetDefaultLightRendererMask(GetLightElementInfo("PointLight")));
  ASSERT_EQ(kRendererMaskAll, GetDefaultLightRendererMask(GetLightElementInfo("SpotLight")));
}

UTEST(LightElementInfo, UnknownTagIsInvalid) {
  ASSERT_FALSE(GetLightElementInfo("Sound").mbValid);
  ASSERT_FALSE(GetLightElementInfo(nullptr).mbValid);
}

UTEST(RayTracedLightClass, BoxLightNeverUsesIt) {
  // A BoxLight has no ray-traced class, so its Re_* attributes are ignored.
  const cLightElementInfo box = GetLightElementInfo("BoxLight");
  ASSERT_FALSE(ShouldUseRayTracedLightClass(box, kRendererMaskAll, true));
  ASSERT_FALSE(ShouldUseRayTracedLightClass(box, kRendererMaskAll, false));
}

UTEST(RayTracedLightClass, LegacyShapeFollowsTheBackendAndMask) {
  const cLightElementInfo point = GetLightElementInfo("PointLight");
  ASSERT_FALSE(ShouldUseRayTracedLightClass(point, kRendererMaskAll, false));
  ASSERT_TRUE(ShouldUseRayTracedLightClass(point, kRendererMaskAll, true));
  // Standard-only light previewed under the ray-traced backend stays legacy.
  ASSERT_FALSE(ShouldUseRayTracedLightClass(point, kRendererMaskStandard, true));
}

UTEST(RayTracedLightClass, AreaLightAlwaysUsesIt) {
  const cLightElementInfo area = GetLightElementInfo("AreaLight");
  ASSERT_TRUE(ShouldUseRayTracedLightClass(area, kRendererMaskRayTraced, true));
  ASSERT_TRUE(ShouldUseRayTracedLightClass(area, kRendererMaskRayTraced, false));
}

UTEST(LegacyLightParameters, AuthoredRadiusIsVerbatim) {
  cLegacyLightInput input;
  input.mbHasRadius = true;
  input.mfRadius = 4.0f;
  input.mbHasFlickerOffRadius = true;
  input.mfFlickerOffRadius = 2.5f;

  const cLegacyLightParameters params = ResolveLegacyLightParameters(input);
  ASSERT_TRUE(NearlyEqual(4.0f, params.mfRadius));
  ASSERT_TRUE(NearlyEqual(2.5f, params.mfFlickerOffRadius));
}

UTEST(LegacyLightParameters, AbsentRadiusTakesTheDefault) {
  const cLegacyLightParameters params = ResolveLegacyLightParameters(cLegacyLightInput(), 1.0f);
  ASSERT_TRUE(NearlyEqual(1.0f, params.mfRadius));
  ASSERT_TRUE(NearlyEqual(0.0f, params.mfFlickerOffRadius));
}

UTEST(RayTracedLightParameters, IntensityAloneDerivesTheReach) {
  // Re_Intensity with no Re_Radius: reach comes from intensity and lit colour,
  // and the light keeps following its intensity as scripts fade it.
  cRayTracedLightInput input;
  input.mbHasIntensity = true;
  input.mfIntensity = 2.0f;

  const cRayTracedLightParameters params = ResolveRayTracedLightParameters(input);
  ASSERT_TRUE(NearlyEqual(2.0f, params.mfIntensity));
  ASSERT_TRUE(NearlyEqual(DeriveLightReach(2.0f, 1.0f, 1.0f, 1.0f), params.mfRadius));
  ASSERT_GT(params.mfRadius, 0.0f);
}

UTEST(RayTracedLightParameters, AuthoredReachWins) {
  cRayTracedLightInput input;
  input.mbHasIntensity = true;
  input.mfIntensity = 2.0f;
  input.mbHasRadius = true;
  input.mfRadius = 7.0f;
  input.mbHasSourceRadius = true;
  input.mfSourceRadius = 0.035f;
  input.mbHasFlickerOffIntensity = true;
  input.mfFlickerOffIntensity = 1.2f;

  const cRayTracedLightParameters params = ResolveRayTracedLightParameters(input);
  ASSERT_TRUE(NearlyEqual(7.0f, params.mfRadius));
  ASSERT_TRUE(NearlyEqual(0.035f, params.mfSourceRadius));
  ASSERT_TRUE(NearlyEqual(1.2f, params.mfFlickerOffIntensity));
}

UTEST(RayTracedLightParameters, DarkColourReachesFurtherThanABrightOne) {
  // The reach derivation is colour-dependent, which is why DiffuseColor (and
  // therefore Re_DiffuseColor) has to be read before the photometric fork.
  const float bright = DeriveLightReach(1.0f, 1.0f, 1.0f, 1.0f);
  const float dim = DeriveLightReach(1.0f, 0.2f, 0.2f, 0.2f);
  ASSERT_GT(bright, dim);
}

UTEST(PromoteLegacyLightParameters, MirrorsTheRetailRadius) {
  // A light with no authored Re_ photometry must load on the ray-traced backend
  // exactly as it did before the elements merged: intensity IS the retail
  // radius, reach derives from it, source radius is zero.
  cLegacyLightParameters legacy;
  legacy.mfRadius = 3.0f;
  legacy.mfFlickerOffRadius = 5.0f;

  const cRayTracedLightParameters promoted =
      PromoteLegacyLightParameters(legacy, 0.69f, 0.569f, 0.31f);
  ASSERT_TRUE(NearlyEqual(3.0f, promoted.mfIntensity));
  ASSERT_TRUE(NearlyEqual(DeriveLightReach(3.0f, 0.69f, 0.569f, 0.31f), promoted.mfRadius));
  ASSERT_TRUE(NearlyEqual(0.0f, promoted.mfSourceRadius));
  ASSERT_TRUE(NearlyEqual(5.0f, promoted.mfFlickerOffIntensity));
}

UTEST(DeriveLightReach, RoundTripsThroughItsInverse) {
  const float reach = DeriveLightReach(2.5f, 0.8f, 0.75f, 0.5f);
  const float intensity = DeriveLightIntensityForReach(reach, 0.8f, 0.75f, 0.5f);
  ASSERT_TRUE(NearlyEqual(2.5f, intensity));
}

//-----------------------------------------------------------------------
// The shared level: fades, flicker and scripts drive ONE normalized value that
// both backends read, so a light dimmed to half stays half of whatever each
// backend authored instead of snapping to one backend's numbers.
//-----------------------------------------------------------------------

UTEST(LightLevel, LevelOneIsTheAuthoredOnValue) {
  ASSERT_TRUE(NearlyEqual(3.0f, ResolveLightLevelValue(3.0f, 0.0f, 1.0f)));
  ASSERT_TRUE(NearlyEqual(1.3f, ResolveLightLevelValue(1.3f, 0.2f, 1.0f)));
}

UTEST(LightLevel, LevelZeroIsTheAuthoredFlickerOffValue) {
  ASSERT_TRUE(NearlyEqual(0.0f, ResolveLightLevelValue(3.0f, 0.0f, 0.0f)));
  ASSERT_TRUE(NearlyEqual(0.2f, ResolveLightLevelValue(1.3f, 0.2f, 0.0f)));
}

UTEST(LightLevel, OneLevelDrivesBothBackendsAtTheirOwnDepth) {
  // The invariant the whole scheme rests on: a Standard tuning of (on 3, off 0)
  // and a ray-traced tuning of (on 1.3, off 0.2) at the same level land on each
  // one's own midpoint, and either one recovers the shared level.
  const float fStandard = ResolveLightLevelValue(3.0f, 0.0f, 0.5f);
  const float fRayTraced = ResolveLightLevelValue(1.3f, 0.2f, 0.5f);
  ASSERT_TRUE(NearlyEqual(1.5f, fStandard));
  ASSERT_TRUE(NearlyEqual(0.75f, fRayTraced));

  float fFromStandard = 0.0f, fFromRayTraced = 0.0f;
  ASSERT_TRUE(DeriveLightLevel(3.0f, 0.0f, fStandard, &fFromStandard));
  ASSERT_TRUE(DeriveLightLevel(1.3f, 0.2f, fRayTraced, &fFromRayTraced));
  ASSERT_TRUE(NearlyEqual(0.5f, fFromStandard));
  ASSERT_TRUE(NearlyEqual(0.5f, fFromRayTraced));
}

UTEST(LightLevel, AFadeDestinationMustBeSnappedInItsOwnBackendsUnits) {
  // What iLight::SnapFadeToDestination is for. A fade issued on Standard puts
  // its destination in metres of reach (on 3, off 0); reading that number
  // against the ray-traced range (on 1.3, off 0.2) is the bug -- 1.5 becomes a
  // level well above 1 and the light balloons.
  float fWrong = 0.0f;
  ASSERT_TRUE(DeriveLightLevel(1.3f, 0.2f, 1.5f, &fWrong));
  ASSERT_TRUE(fWrong > 1.0f);
  ASSERT_TRUE(ResolveLightLevelValue(1.3f, 0.2f, fWrong) > 1.3f);

  // Snapped against the tuning that issued it, the destination becomes a level,
  // and the ray-traced tuning then sits at its OWN value for that level.
  float fLevel = 0.0f;
  ASSERT_TRUE(DeriveLightLevel(3.0f, 0.0f, 1.5f, &fLevel));
  ASSERT_TRUE(NearlyEqual(0.5f, fLevel));
  ASSERT_TRUE(NearlyEqual(0.75f, ResolveLightLevelValue(1.3f, 0.2f, fLevel)));
}

UTEST(LightLevel, RoundTripsThroughItsInverse) {
  float fLevel = 0.0f;
  ASSERT_TRUE(DeriveLightLevel(4.0f, 0.5f, 2.25f, &fLevel));
  ASSERT_TRUE(NearlyEqual(2.25f, ResolveLightLevelValue(4.0f, 0.5f, fLevel)));
}

UTEST(LightLevel, ScriptsMayOverdriveAboveOne) {
  // FadeLightTo can ask for more than the authored value; the level is not
  // clamped, only the resolved value's lower bound is.
  ASSERT_TRUE(NearlyEqual(6.0f, ResolveLightLevelValue(3.0f, 0.0f, 2.0f)));
}

UTEST(LightLevel, ResolvedValueNeverGoesNegative) {
  // A fade passing through zero reads as invisible and then comes back,
  // rather than driving the light to a negative radius.
  ASSERT_TRUE(NearlyEqual(0.0f, ResolveLightLevelValue(3.0f, 0.0f, -1.0f)));
}

UTEST(LightLevel, NoRangeHasNoLevel) {
  // A light authored dark, or one whose flicker goes nowhere, has no ratio to
  // preserve -- the caller re-authors the value instead of scaling to it.
  float fLevel = 123.0f;
  ASSERT_FALSE(DeriveLightLevel(0.0f, 0.0f, 6.0f, &fLevel));
  ASSERT_FALSE(DeriveLightLevel(2.0f, 2.0f, 6.0f, &fLevel));
  ASSERT_TRUE(NearlyEqual(123.0f, fLevel));
}

UTEST(LightLevel, NonFiniteInputsDoNotEscape) {
  const float fInfinity = std::numeric_limits<float>::infinity();
  ASSERT_TRUE(std::isfinite(ResolveLightLevelValue(fInfinity, 0.0f, 1.0f)));
  ASSERT_TRUE(std::isfinite(ResolveLightLevelValue(3.0f, 0.0f, fInfinity)));
}

//-----------------------------------------------------------------------
// Colour is a DRIVE, not an authoring write. A script that dims a lamp means
// "it looks like this now", so it is stored as a scale over the authored
// colour and read by both tunings -- otherwise a lamp turned off on one
// backend lights back up the moment the renderer changes.
//-----------------------------------------------------------------------

UTEST(LightColorScale, DimmingIsARatio) {
  float scale = 0.0f;
  ASSERT_TRUE(DeriveLightColorScale(0.8f, 0.4f, &scale));
  ASSERT_TRUE(NearlyEqual(0.5f, scale));
}

UTEST(LightColorScale, TurningOffIsZeroOnEveryTuning) {
  // The regression this guards: a lamp blacked out while Standard was drawing
  // must still be black when the ray-traced tuning takes over, even though the
  // two authored different colours.
  float standard = 1.0f, rayTraced = 1.0f;
  ASSERT_TRUE(DeriveLightColorScale(0.8f, 0.0f, &standard));
  ASSERT_TRUE(NearlyEqual(0.0f, standard));

  // The same scale read against the other tuning's authored colour.
  const float otherAuthored = 0.45f;
  rayTraced = otherAuthored * standard;
  ASSERT_TRUE(NearlyEqual(0.0f, rayTraced));
}

UTEST(LightColorScale, AuthoredDarkAndAskedDarkIsExpressible) {
  float scale = 123.0f;
  ASSERT_TRUE(DeriveLightColorScale(0.0f, 0.0f, &scale));
  ASSERT_TRUE(NearlyEqual(0.0f, scale));
}

UTEST(LightColorScale, AuthoredDarkAskedLitNeedsAnAbsoluteColour) {
  // No ratio exists: the caller applies the colour outright to both tunings.
  float scale = 123.0f;
  ASSERT_FALSE(DeriveLightColorScale(0.0f, 0.7f, &scale));
  ASSERT_TRUE(NearlyEqual(123.0f, scale));
}

UTEST(LightColorScale, BrighteningAboveTheAuthoredColourIsAllowed) {
  float scale = 0.0f;
  ASSERT_TRUE(DeriveLightColorScale(0.4f, 0.8f, &scale));
  ASSERT_TRUE(NearlyEqual(2.0f, scale));
}

UTEST_MAIN()
