#include "graphics/StandardWaterMath.h"
#include <cmath>

namespace hpl {
static float dot(const cVector3f &a, const cVector3f &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
static cMatrixf multiply(const cMatrixf &a, const cMatrixf &b) {
  cMatrixf result;
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col) {
      result.m[row][col] = 0.0f;
      for (int k = 0; k < 4; ++k)
        result.m[row][col] += a.m[row][k] * b.m[k][col];
    }
  return result;
}

bool IsValidStandardWaterPlane(const cStandardWaterPlane &p) {
  const float n2 = p.normal.x * p.normal.x + p.normal.y * p.normal.y +
                   p.normal.z * p.normal.z;
  return std::isfinite(p.distance) && std::isfinite(n2) && n2 > 1.0e-8f;
}

cVector3f ReflectStandardWaterPoint(const cVector3f &p,
                                    const cStandardWaterPlane &plane) {
  if (!IsValidStandardWaterPlane(plane))
    return p;
  const float invN2 = 1.0f / (plane.normal.x * plane.normal.x +
                              plane.normal.y * plane.normal.y +
                              plane.normal.z * plane.normal.z);
  const float twiceDistance =
      2.0f * (dot(plane.normal, p) + plane.distance) * invN2;
  return p - plane.normal * twiceDistance;
}

cVector3f ReflectStandardWaterVector(const cVector3f &v,
                                     const cStandardWaterPlane &plane) {
  if (!IsValidStandardWaterPlane(plane))
    return v;
  const float invN2 = 1.0f / (plane.normal.x * plane.normal.x +
                              plane.normal.y * plane.normal.y +
                              plane.normal.z * plane.normal.z);
  return v - plane.normal * (2.0f * dot(plane.normal, v) * invN2);
}

cMatrixf MakeStandardWaterReflectionMatrix(const cStandardWaterPlane &p) {
  if (!IsValidStandardWaterPlane(p))
    return cMatrixf::Identity;
  const float invN2 =
      1.0f / (p.normal.x * p.normal.x + p.normal.y * p.normal.y +
              p.normal.z * p.normal.z);
  const float x = 2.0f * p.normal.x * invN2;
  const float y = 2.0f * p.normal.y * invN2;
  const float z = 2.0f * p.normal.z * invN2;
  return cMatrixf(1 - x * p.normal.x, -x * p.normal.y, -x * p.normal.z,
                  -x * p.distance, -y * p.normal.x, 1 - y * p.normal.y,
                  -y * p.normal.z, -y * p.distance, -z * p.normal.x,
                  -z * p.normal.y, 1 - z * p.normal.z, -z * p.distance, 0, 0, 0,
                  1);
}

cMatrixf MakeStandardWaterReflectedView(const cMatrixf &view,
                                        const cStandardWaterPlane &plane) {
  return multiply(view, MakeStandardWaterReflectionMatrix(plane));
}

cMatrixf
MakeStandardWaterReflectedViewProjection(const cMatrixf &projection,
                                         const cMatrixf &view,
                                         const cStandardWaterPlane &plane) {
  return multiply(projection, MakeStandardWaterReflectedView(view, plane));
}
} // namespace hpl
