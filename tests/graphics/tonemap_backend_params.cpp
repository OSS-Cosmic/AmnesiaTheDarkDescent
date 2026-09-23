#include "graphics/ToneMapBackendParams.h"
#include "Constants.h"
#include "utest.h"

// The tonemap pass is the one place in post-processing where the two renderer
// backends diverge. Standard must stay an exact identity -- it emits
// sRGBToLinear(display) / kSceneExposure and relies on the tonemap being its
// inverse, so the base game's look returns bit-for-bit -- while the ray-traced
// path takes a gamma bias, a highlight shoulder and a chroma pull-down. The
// selection lives in ResolveToneMapBackendParams so it is testable without a
// device.

using namespace hpl;

UTEST(ToneMapBackendParams, StandardIsAnExactIdentity) {
  const float userGamma = 1.15f;
  const auto params = ResolveToneMapBackendParams(eRendererBackend_Standard, userGamma);
  // Exact equality on purpose: anything else breaks the inverse pair.
  ASSERT_EQ(userGamma, params.mfGamma);
  ASSERT_EQ(0.0f, params.mfShoulder);
  ASSERT_EQ(1.0f, params.mfSaturation);
}

UTEST(ToneMapBackendParams, StandardIdentityHoldsAcrossTheGammaSliderRange) {
  // The options slider runs 0.3 .. 2.0 (LuxMainMenu_Options).
  const float gammas[] = {0.3f, 1.0f, 1.45f, 2.0f};
  for (float gamma : gammas) {
    const auto params = ResolveToneMapBackendParams(eRendererBackend_Standard, gamma);
    ASSERT_EQ(gamma, params.mfGamma);
    ASSERT_EQ(1.0f, params.mfSaturation);
  }
}

UTEST(ToneMapBackendParams, RayTracedLiftsGammaAndRollsOffHighlights) {
  const float userGamma = 1.0f;
  const auto params = ResolveToneMapBackendParams(eRendererBackend_RayTraced, userGamma);
  ASSERT_NEAR(userGamma + kRayTracedGammaBias, params.mfGamma, 1e-6f);
  ASSERT_EQ(1.0f, params.mfShoulder);
}

UTEST(ToneMapBackendParams, RayTracedPullsChromaDown) {
  const auto params = ResolveToneMapBackendParams(eRendererBackend_RayTraced, 1.0f);
  ASSERT_NEAR(kRayTracedSaturation, params.mfSaturation, 1e-6f);
  // Below 1 or the pass skips it entirely; above 0 or the frame goes greyscale.
  ASSERT_LT(params.mfSaturation, 1.0f);
  ASSERT_GT(params.mfSaturation, 0.0f);
}

UTEST(ToneMapBackendParams, TheGammaBiasIsAdditiveNotAScale) {
  // The player's slider must keep moving the result by the amount they chose --
  // the ray-traced frame just starts higher.
  const auto low = ResolveToneMapBackendParams(eRendererBackend_RayTraced, 0.8f);
  const auto high = ResolveToneMapBackendParams(eRendererBackend_RayTraced, 1.6f);
  ASSERT_NEAR(0.8f, high.mfGamma - low.mfGamma, 1e-5f);
}

UTEST(ToneMapBackendParams, SaturationDoesNotDependOnTheGammaSetting) {
  const auto low = ResolveToneMapBackendParams(eRendererBackend_RayTraced, 0.3f);
  const auto high = ResolveToneMapBackendParams(eRendererBackend_RayTraced, 2.0f);
  ASSERT_EQ(low.mfSaturation, high.mfSaturation);
}
