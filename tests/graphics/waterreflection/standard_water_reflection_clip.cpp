#include "graphics/StandardWaterReflectionClip.h"
#include "graphics/StandardWaterMath.h"
#include "math/BoundingVolume.h"
#include "math/Frustum.h"
#include "math/Math.h"
#include "utest.h"

#include <cmath>

using namespace hpl;

namespace {
constexpr float kFov = 1.0f;
constexpr float kAspect = 16.0f / 9.0f;
constexpr float kNear = 0.1f;
constexpr float kFar = 200.0f;
const cVector2l kExtent(640, 360);

cPlanef MakePlane(const cVector3f &normal, const cVector3f &point) {
  cPlanef plane;
  plane.FromNormalPoint(normal, point);
  return plane;
}

cBoundingVolume MakeBounds(const cVector3f &centre, const cVector3f &halfSize) {
  cBoundingVolume bv;
  bv.SetLocalMinMax(halfSize * -1, halfSize);
  bv.SetPosition(centre);
  return bv;
}

struct Scene {
  cFrustum main;
  cFrustum reflected;
  cBoundingVolume surface;
  cPlanef plane;
};

// The reflected view is a genuine mirror of the main one, which matters: the
// mirror flips the basis handedness, and the screen-rect planes are built from
// cross products of that basis. Feeding this helper an unmirrored second
// camera would invert every plane it emits.
void SetupReflectedCamera(Scene &s, const cVector3f &origin,
                          const cStandardWaterPlane &waterPlane) {
  const cMatrixf projection =
      cMath::MatrixPerspectiveProjection(kNear, kFar, kFov, kAspect, false);
  const cMatrixf view = cMath::MatrixTranslate(origin * -1);
  s.main.SetupPerspectiveProj(projection, view, kFar, kNear, kFov, kAspect,
                              origin);
  s.reflected.SetupPerspectiveProj(
      projection, MakeStandardWaterReflectedView(view, waterPlane), kFar, kNear,
      kFov, kAspect, ReflectStandardWaterPoint(origin, waterPlane));
}

// A vertical pane 20 units ahead of the camera, facing back toward it. The
// distance-plane branch only reads the main frustum and the surface plane.
Scene MakeForwardFacingScene() {
  Scene s;
  const cStandardWaterPlane waterPlane{cVector3f(0, 0, 1), 20.0f};
  SetupReflectedCamera(s, cVector3f(0, 0, 0), waterPlane);
  s.plane = MakePlane(cVector3f(0, 0, 1), cVector3f(0, 0, -20));
  s.surface = MakeBounds(cVector3f(0, 0, -20), cVector3f(40, 40, 0.1f));
  return s;
}

// A horizontal pane of water at y = 0 with the camera above it, i.e. the shape
// every real capture has.
Scene MakeWaterScene(const cVector3f &surfaceCentre,
                     const cVector3f &surfaceHalfSize) {
  Scene s;
  const cStandardWaterPlane waterPlane{cVector3f(0, 1, 0), 0.0f};
  SetupReflectedCamera(s, cVector3f(0, 4, 0), waterPlane);
  s.plane = MakePlane(cVector3f(0, 1, 0), cVector3f(0, 0, 0));
  s.surface = MakeBounds(surfaceCentre, surfaceHalfSize);
  return s;
}

bool CulledBy(StandardWaterReflectionClip &clip, cBoundingVolume &bounds) {
  for (uint32_t i = 0; i < clip.planeCount; ++i)
    if (cMath::CheckPlaneBVCollision(clip.planes[i], bounds) ==
        eCollision_Outside)
      return true;
  return false;
}
} // namespace

UTEST(StandardWaterReflectionClip, NoDistanceNoScreenRectYieldsNothing) {
  Scene s = MakeForwardFacingScene();
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 0.0f, false, kExtent);
  EXPECT_EQ_MSG(clip.planeCount, 0u,
                "a non-positive distance emits no distance plane");
  EXPECT_FALSE_MSG(clip.hasScissor, "screen-rect clipping off emits no scissor");
  EXPECT_FALSE_MSG(clip.surfaceOutOfRange, "the surface is well within range");
}

UTEST(StandardWaterReflectionClip, ForwardAlignedDistancePlane) {
  // fFDotN < -0.99999f: forward and the surface normal are exactly opposed, so
  // the plane normal is the inverse forward and no camera-space solve happens.
  Scene s = MakeForwardFacingScene();
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 50.0f, false, kExtent);
  ASSERT_EQ(clip.planeCount, 1u);
  const cPlanef &plane = clip.planes[0];
  EXPECT_NEAR_MSG(plane.a, 0.0f, 1.0e-4f, "distance plane faces the camera");
  EXPECT_NEAR_MSG(plane.b, 0.0f, 1.0e-4f, "distance plane faces the camera");
  EXPECT_NEAR_MSG(std::fabs(plane.c), 1.0f, 1.0e-4f,
                  "distance plane normal is the view axis");
  // A point just inside the limit is kept, one well beyond it is not.
  cBoundingVolume near = MakeBounds(cVector3f(0, 0, -10), cVector3f(1, 1, 1));
  cBoundingVolume far = MakeBounds(cVector3f(0, 0, -120), cVector3f(1, 1, 1));
  EXPECT_NE_MSG(cMath::CheckPlaneBVCollision(plane, near), eCollision_Outside,
                "geometry inside the fade distance survives");
  EXPECT_EQ_MSG(cMath::CheckPlaneBVCollision(plane, far), eCollision_Outside,
                "geometry past the fade distance is culled");
}

UTEST(StandardWaterReflectionClip, DistancePlaneKeepsTheSurface) {
  Scene s = MakeForwardFacingScene();
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 50.0f, true, kExtent);
  for (uint32_t i = 0; i < clip.planeCount; ++i)
    EXPECT_NE_MSG(cMath::CheckPlaneBVCollision(clip.planes[i], s.surface),
                  eCollision_Outside,
                  "no emitted plane may cull the water surface itself");
}

UTEST(StandardWaterReflectionClip, SurfaceBeyondFadeSkipsCapture) {
  Scene s = MakeForwardFacingScene();
  // A small pane, entirely past the fade distance. The large pane in the other
  // cases straddles the plane and is therefore still partly in range.
  s.surface = MakeBounds(cVector3f(0, 0, -20), cVector3f(1, 0.1f, 1));
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 2.0f, true, kExtent);
  EXPECT_TRUE_MSG(clip.surfaceOutOfRange,
                  "a surface past the fade distance skips the whole capture");
  EXPECT_EQ_MSG(clip.planeCount, 0u, "a skipped capture emits no planes");
  EXPECT_FALSE_MSG(clip.hasScissor, "a skipped capture emits no scissor");
}

UTEST(StandardWaterReflectionClip, ObliqueDistancePlane) {
  // A tilted surface takes the camera-space solve instead of the aligned
  // branch. The resulting plane must still keep the surface.
  Scene s;
  const cVector3f tilted =
      cMath::Vector3Normalize(cVector3f(0.3f, 1.0f, 0.2f));
  const cStandardWaterPlane waterPlane{tilted, 0.0f};
  SetupReflectedCamera(s, cVector3f(0, 4, 0), waterPlane);
  s.plane = MakePlane(tilted, cVector3f(0, 0, -20));
  s.surface = MakeBounds(cVector3f(0, 0, -20), cVector3f(40, 0.1f, 40));
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 60.0f, false, kExtent);
  EXPECT_FALSE_MSG(clip.surfaceOutOfRange, "the surface is within range");
  ASSERT_EQ(clip.planeCount, 1u);
  const float length = std::sqrt(clip.planes[0].a * clip.planes[0].a +
                                 clip.planes[0].b * clip.planes[0].b +
                                 clip.planes[0].c * clip.planes[0].c);
  EXPECT_NEAR_MSG(length, 1.0f, 1.0e-3f, "the emitted plane is normalized");
}

UTEST(StandardWaterReflectionClip, ScissorStaysInsideTheCaptureExtent) {
  // A small pane off to one side produces screen-rect planes and a scissor
  // bounded by the half-resolution capture extent.
  Scene s = MakeWaterScene(cVector3f(6, 0, -20), cVector3f(2, 0.1f, 2));
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 0.0f, true, kExtent);
  EXPECT_GT_MSG(clip.planeCount, 0u,
                "a bounded surface emits screen-rect planes");
  ASSERT_TRUE(clip.hasScissor);
  EXPECT_GE_MSG(clip.scissor.x, 0, "scissor starts inside the target");
  EXPECT_GE_MSG(clip.scissor.y, 0, "scissor starts inside the target");
  EXPECT_LE_MSG(clip.scissor.x + clip.scissor.w, kExtent.x,
                "scissor ends inside the target");
  EXPECT_LE_MSG(clip.scissor.y + clip.scissor.h, kExtent.y,
                "scissor ends inside the target");
}

UTEST(StandardWaterReflectionClip, ScreenRectPlanesFaceInward) {
  // The orientation check that matters: geometry whose reflection lands inside
  // the pane survives, geometry far off to the side does not. Inverted normals
  // would cull everything and leave the reflection black.
  Scene s = MakeWaterScene(cVector3f(6, 0, -20), cVector3f(2, 0.1f, 2));
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 0.0f, true, kExtent);
  ASSERT_GT(clip.planeCount, 0u);
  // On the ray from the reflected eye (0, -4, 0) through the pane's centre,
  // so its reflection lands in the middle of the pane's screen rect.
  cBoundingVolume overThePane =
      MakeBounds(cVector3f(12, 4, -40), cVector3f(0.5f, 0.5f, 0.5f));
  cBoundingVolume farToTheSide =
      MakeBounds(cVector3f(-60, 2, -20), cVector3f(0.5f, 0.5f, 0.5f));
  EXPECT_FALSE_MSG(CulledBy(clip, overThePane),
                   "geometry reflected inside the pane is kept");
  EXPECT_TRUE_MSG(CulledBy(clip, farToTheSide),
                  "geometry far outside the pane's screen rect is culled");
}

UTEST(StandardWaterReflectionClip, ScreenRectPlanesKeepTheSurface) {
  Scene s = MakeWaterScene(cVector3f(6, 0, -20), cVector3f(2, 0.1f, 2));
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 0.0f, true, kExtent);
  EXPECT_FALSE_MSG(CulledBy(clip, s.surface),
                   "the bounds the rect was derived from are never culled");
}

UTEST(StandardWaterReflectionClip, SurfaceBehindTheReflectedViewEmitsNoRect) {
  // Entirely behind the camera.
  Scene s = MakeWaterScene(cVector3f(0, 0, 40), cVector3f(1, 0.1f, 1));
  auto clip = BuildStandardWaterReflectionClip(&s.main, &s.reflected, s.surface,
                                               s.plane, 0.0f, true, kExtent);
  EXPECT_EQ_MSG(clip.planeCount, 0u,
                "a surface behind the frustum emits no screen-rect planes");
  EXPECT_FALSE_MSG(clip.hasScissor, "and no scissor");
}

UTEST(StandardWaterReflectionClip, NullFrustaAreRejected) {
  Scene s = MakeForwardFacingScene();
  auto clip = BuildStandardWaterReflectionClip(nullptr, &s.reflected, s.surface,
                                               s.plane, 50.0f, true, kExtent);
  EXPECT_EQ_MSG(clip.planeCount, 0u, "a null main frustum emits nothing");
  clip = BuildStandardWaterReflectionClip(&s.main, nullptr, s.surface, s.plane,
                                          50.0f, true, kExtent);
  EXPECT_EQ_MSG(clip.planeCount, 0u, "a null reflected frustum emits nothing");
}

UTEST_MAIN();
