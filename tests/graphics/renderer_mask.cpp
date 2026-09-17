// A billboard a light adopts is the Standard backend's fake glow: the
// ray-traced backend lights the source for real, so iLight::AttachBillboard
// takes the ray-traced bit out of the billboard's renderer mask and
// rendering::IsObjectIsVisible drops it from the ray-traced gather. Attaching
// cannot be exercised without the engine, so what is pinned here is the mask
// arithmetic it runs.

#include "graphics/RendererMask.h"
#include "utest.h"

using namespace hpl;

UTEST(RendererMask, MaskWithoutRayTracedLeavesStandardOnly) {
  ASSERT_EQ(kRendererMaskStandard, MaskWithoutRayTraced(kRendererMaskAll));
}

UTEST(RendererMask, MaskWithoutRayTracedIsIdempotent) {
  const unsigned once = MaskWithoutRayTraced(kRendererMaskAll);
  ASSERT_EQ(once, MaskWithoutRayTraced(once));
  ASSERT_EQ(kRendererMaskStandard, MaskWithoutRayTraced(kRendererMaskStandard));
}

UTEST(RendererMask, RayTracedOnlyBillboardDrawsNowhere) {
  // Authored for the backend that now refuses it, so it draws on neither. That
  // is the authored mask winning, not a fallback.
  ASSERT_EQ(0u, MaskWithoutRayTraced(kRendererMaskRayTraced));
}

UTEST(RendererMask, StrippedMaskFailsTheRayTracedGate) {
  const unsigned mask = MaskWithoutRayTraced(kRendererMaskAll);
  ASSERT_TRUE(IsRendererMaskEnabled(mask, kRendererMaskStandard));
  ASSERT_FALSE(IsRendererMaskEnabled(mask, kRendererMaskRayTraced));
}
