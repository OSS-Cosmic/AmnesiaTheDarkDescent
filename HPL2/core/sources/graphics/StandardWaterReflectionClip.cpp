#include "graphics/StandardWaterReflectionClip.h"

#include "math/BoundingVolume.h"
#include "math/Frustum.h"
#include "math/Math.h"

#include <cmath>

namespace hpl {
namespace {
void AddPlane(StandardWaterReflectionClip &clip, const cPlanef &plane) {
  if (clip.planeCount < clip.planes.size())
    clip.planes[clip.planeCount++] = plane;
}
} // namespace

StandardWaterReflectionClip BuildStandardWaterReflectionClip(
    cFrustum *mainFrustum, cFrustum *reflectedFrustum,
    cBoundingVolume &surfaceBounds, const cPlanef &surfacePlane,
    float maxReflectionDistance, bool clipScreenRect,
    const cVector2l &reflectionExtent) {
  StandardWaterReflectionClip clip;
  if (!mainFrustum || !reflectedFrustum)
    return clip;

  const cVector3f surfaceNormal(surfacePlane.a, surfacePlane.b, surfacePlane.c);

  ///////////////////////////
  // End-of-reflection clip plane. Beyond it the surface's reflection has
  // already faded out, so nothing behind it is worth capturing.
  if (maxReflectionDistance > 0) {
    const cVector3f vForward = mainFrustum->GetForward() * -1;
    const float fMaxReflDist = maxReflectionDistance;
    cPlanef maxReflectionDistPlane;

    ///////////////////////////////
    // Forward and normal are aligned: the plane normal is inverse forward.
    const float fFDotN = cMath::Vector3Dot(vForward, surfaceNormal);
    if (fFDotN < -0.99999f) {
      const cVector3f vClipNormal = vForward * -1;
      const cVector3f vClipPoint =
          mainFrustum->GetOrigin() + vForward * fMaxReflDist;
      maxReflectionDistPlane.FromNormalPoint(vClipNormal, vClipPoint);
    }
    ///////////////////////////////
    // Otherwise take the surface plane into camera space and pick two points
    // on it at z = -maxReflectionDistance. The test above guarantees that a
    // and b are not both zero.
    else {
      const cPlanef cameraSpacePlane =
          cMath::TransformPlane(mainFrustum->GetViewMatrix(), surfacePlane);

      cVector3f vPoint1 = cVector3f(0, 0, -fMaxReflDist);
      cVector3f vPoint2 = cVector3f(0, 0, -fMaxReflDist);

      // Vertical row (x always the same)
      if (std::fabs(cameraSpacePlane.b) < 0.0001f) {
        vPoint1.x = (-cameraSpacePlane.c * -fMaxReflDist - cameraSpacePlane.d) /
                    cameraSpacePlane.a;
        vPoint2 = vPoint1;
        vPoint2.y += 1;
      }
      // Horizontal row (y always the same)
      else if (std::fabs(cameraSpacePlane.a) < 0.0001f) {
        vPoint1.y = (-cameraSpacePlane.c * -fMaxReflDist - cameraSpacePlane.d) /
                    cameraSpacePlane.b;
        vPoint2 = vPoint1;
        vPoint2.x += 1;
      }
      // Oblique row (x and y both change)
      else {
        vPoint1.x = (-cameraSpacePlane.c * -fMaxReflDist - cameraSpacePlane.d) /
                    cameraSpacePlane.a;
        vPoint2.y = (-cameraSpacePlane.c * -fMaxReflDist - cameraSpacePlane.d) /
                    cameraSpacePlane.b;
      }

      const cMatrixf mtxInvCamera =
          cMath::MatrixInverse(mainFrustum->GetViewMatrix());
      vPoint1 = cMath::MatrixMul(mtxInvCamera, vPoint1);
      vPoint2 = cMath::MatrixMul(mtxInvCamera, vPoint2);

      const cVector3f reflectedOrigin = reflectedFrustum->GetOrigin();
      cVector3f vNormal = cMath::Vector3Cross(vPoint1 - reflectedOrigin,
                                              vPoint2 - reflectedOrigin);
      vNormal.Normalize();
      // Make sure the normal has the correct sign.
      if (cMath::Vector3Dot(surfaceNormal, vNormal) < 0)
        vNormal = vNormal * -1;

      maxReflectionDistPlane.FromNormalPoint(vNormal, vPoint1);
    }

    if (cMath::CheckPlaneBVCollision(maxReflectionDistPlane, surfaceBounds) ==
        eCollision_Outside) {
      clip.surfaceOutOfRange = true;
      return clip;
    }
    AddPlane(clip, maxReflectionDistPlane);
  }

  //////////////////////////
  // Screen-rect planes: nothing outside the water surface's own screen
  // footprint can ever be sampled from the reflection texture.
  if (!clipScreenRect)
    return clip;

  const cVector3f vUp = reflectedFrustum->GetViewMatrix().GetUp();
  const cVector3f vRight = reflectedFrustum->GetViewMatrix().GetRight();
  const cVector3f vForward = reflectedFrustum->GetViewMatrix().GetForward();
  const cVector3f vOrigin = reflectedFrustum->GetOrigin();

  const float fNearPlane = reflectedFrustum->GetNearPlane();
  const float fHalfFovTan = std::tan(reflectedFrustum->GetFOV() * 0.5f);
  const float fNearTop = fHalfFovTan * fNearPlane;
  const float fNearRight = reflectedFrustum->GetAspect() * fNearTop;

  cVector3f vMin, vMax;
  if (!cMath::GetNormalizedClipRectFromBV(vMin, vMax, surfaceBounds,
                                          reflectedFrustum, fHalfFovTan))
    return clip;

  bool bNeedsClipRect = false;

  // Right
  if (vMax.x < 1) {
    const cVector3f vNearPlanePos =
        vOrigin + vRight * (vMax.x * fNearRight) + vForward * -fNearPlane;
    cPlanef rightPlane;
    rightPlane.FromPoints(vOrigin, vNearPlanePos, vNearPlanePos + vUp);
    AddPlane(clip, rightPlane);
    bNeedsClipRect = true;
  }
  // Left
  if (vMin.x > -1) {
    const cVector3f vNearPlanePos =
        vOrigin + vRight * (vMin.x * fNearRight) + vForward * -fNearPlane;
    cPlanef leftPlane;
    leftPlane.FromPoints(vOrigin, vNearPlanePos + vUp, vNearPlanePos);
    AddPlane(clip, leftPlane);
    bNeedsClipRect = true;
  }
  // Top
  if (vMax.y < 1) {
    const cVector3f vNearPlanePos =
        vOrigin + vUp * (vMax.y * fNearTop) + vForward * -fNearPlane;
    cPlanef topPlane;
    topPlane.FromPoints(vOrigin, vNearPlanePos + vRight, vNearPlanePos);
    AddPlane(clip, topPlane);
    bNeedsClipRect = true;
  }
  // Bottom
  if (vMin.y > -1) {
    const cVector3f vNearPlanePos =
        vOrigin + vUp * (vMin.y * fNearTop) + vForward * -fNearPlane;
    cPlanef bottomPlane;
    bottomPlane.FromPoints(vOrigin, vNearPlanePos, vNearPlanePos + vRight);
    AddPlane(clip, bottomPlane);
    bNeedsClipRect = true;
  }

  //////////////////////////
  // Scissor. Taken against the MAIN frustum, as the legacy pass did: the
  // surface projects to the same screen rect under both cameras, and the
  // reflection is sampled with the main camera's projection.
  //
  // GetClipRectFromNormalizedMinMax emits a top-left-origin, y-down rect,
  // which is already the Vulkan scissor convention.
  if (bNeedsClipRect && reflectionExtent.x > 0 && reflectionExtent.y > 0) {
    cRect2l clipRect;
    cMath::GetClipRectFromBV(clipRect, surfaceBounds, mainFrustum,
                             reflectionExtent, fHalfFovTan);
    clipRect.x = cMath::Max(clipRect.x, 0);
    clipRect.y = cMath::Max(clipRect.y, 0);
    clipRect.w = cMath::Min(clipRect.w, reflectionExtent.x - clipRect.x);
    clipRect.h = cMath::Min(clipRect.h, reflectionExtent.y - clipRect.y);
    if (clipRect.w > 0 && clipRect.h > 0) {
      clip.scissor = clipRect;
      clip.hasScissor = true;
    }
  }

  return clip;
}
} // namespace hpl
