#include "graphics/StandardWaterReflectionSort.h"

#include "math/BoundingVolume.h"
#include "math/Frustum.h"
#include "math/Math.h"

namespace hpl {
float StandardReflectionTranslucentViewZ(cFrustum *frustum,
                                         cBoundingVolume &bounds) {
  if (!frustum)
    return 0.0f;
  // The nearest point of the volume toward the eye, exactly as the main list
  // does. Inside the volume there is no intersection; fall back to the centre.
  cVector3f intersection;
  if (cMath::CheckAABBLineIntersection(bounds.GetMin(), bounds.GetMax(),
                                       frustum->GetOrigin(),
                                       bounds.GetWorldCenter(), &intersection,
                                       NULL) == false) {
    intersection = bounds.GetWorldCenter();
  }
  return cMath::MatrixMul(frustum->GetViewMatrix(), intersection).z;
}
} // namespace hpl
