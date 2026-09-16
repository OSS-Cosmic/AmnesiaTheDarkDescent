#include "graphics/StandardWaterReflectionSort.h"
#include "math/BoundingVolume.h"
#include "math/Frustum.h"
#include "math/Math.h"
#include "utest.h"

#include <algorithm>
#include <vector>

using namespace hpl;

namespace {
cFrustum MakeCamera(const cVector3f &origin) {
  const cMatrixf projection = cMath::MatrixPerspectiveProjection(
      0.1f, 200.0f, 1.0f, 16.0f / 9.0f, false);
  const cMatrixf view = cMath::MatrixTranslate(origin * -1);
  cFrustum frustum;
  frustum.SetupPerspectiveProj(projection, view, 200.0f, 0.1f, 1.0f,
                               16.0f / 9.0f, origin);
  return frustum;
}

cBoundingVolume MakeBounds(const cVector3f &centre, float halfSize) {
  cBoundingVolume bv;
  bv.SetLocalMinMax(cVector3f(-halfSize), cVector3f(halfSize));
  bv.SetPosition(centre);
  return bv;
}
} // namespace

UTEST(StandardReflectionSort, NearerSurfaceSortsLast) {
  cFrustum camera = MakeCamera(cVector3f(0, 0, 0));
  cBoundingVolume near = MakeBounds(cVector3f(0, 0, -5), 1.0f);
  cBoundingVolume far = MakeBounds(cVector3f(0, 0, -30), 1.0f);
  const float nearZ = StandardReflectionTranslucentViewZ(&camera, near);
  const float farZ = StandardReflectionTranslucentViewZ(&camera, far);
  EXPECT_LT_MSG(farZ, nearZ,
                "view space is negative forward, so the far surface has the "
                "smaller z and sorts first");
}

UTEST(StandardReflectionSort, AscendingOrderIsBackToFront) {
  cFrustum camera = MakeCamera(cVector3f(0, 0, 0));
  std::vector<cBoundingVolume> volumes = {MakeBounds(cVector3f(0, 0, -5), 1.0f),
                                          MakeBounds(cVector3f(0, 0, -40), 1.0f),
                                          MakeBounds(cVector3f(0, 0, -20), 1.0f)};
  std::vector<size_t> order = {0, 1, 2};
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return StandardReflectionTranslucentViewZ(&camera, volumes[a]) <
           StandardReflectionTranslucentViewZ(&camera, volumes[b]);
  });
  EXPECT_EQ_MSG(order[0], size_t(1), "furthest first");
  EXPECT_EQ_MSG(order[1], size_t(2), "middle second");
  EXPECT_EQ_MSG(order[2], size_t(0), "nearest last");
}

UTEST(StandardReflectionSort, UsesSurfacePointNotCentre) {
  // The key is the nearest point on the volume toward the eye, so a large
  // volume whose centre is far still sorts by how close its face is.
  cFrustum camera = MakeCamera(cVector3f(0, 0, 0));
  cBoundingVolume large = MakeBounds(cVector3f(0, 0, -30), 20.0f);
  const float key = StandardReflectionTranslucentViewZ(&camera, large);
  EXPECT_GT_MSG(key, -30.0f,
                "the intersection with the near face is used, not the centre");
  EXPECT_NEAR_MSG(key, -10.0f, 1.0e-3f, "which is the volume's near face");
}

UTEST(StandardReflectionSort, InsideTheVolumeFallsBackToTheCentre) {
  // No line intersection when the eye is inside, so the world centre is used,
  // mirroring cRenderList2::AddObject.
  cFrustum camera = MakeCamera(cVector3f(0, 0, 0));
  cBoundingVolume around = MakeBounds(cVector3f(0, 0, 0), 5.0f);
  EXPECT_NEAR_MSG(StandardReflectionTranslucentViewZ(&camera, around), 0.0f,
                  1.0e-4f, "the centre is the key when the eye is inside");
}

UTEST(StandardReflectionSort, NullFrustumIsSafe) {
  cBoundingVolume bounds = MakeBounds(cVector3f(0, 0, -5), 1.0f);
  EXPECT_EQ_MSG(StandardReflectionTranslucentViewZ(nullptr, bounds), 0.0f,
                "a null frustum yields a neutral key instead of dereferencing");
}
