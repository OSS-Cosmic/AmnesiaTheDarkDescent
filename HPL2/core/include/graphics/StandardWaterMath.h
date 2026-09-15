#pragma once

#include "math/MathTypes.h"

namespace hpl {

// Plane convention used by the Standard planar capture: n is unit length and
// the signed half-space is dot(n, p) + d >= 0.
struct cStandardWaterPlane {
  cVector3f normal = cVector3f(0, 1, 0);
  float distance = 0.0f;
};

bool IsValidStandardWaterPlane(const cStandardWaterPlane &plane);
cVector3f ReflectStandardWaterPoint(const cVector3f &point,
                                    const cStandardWaterPlane &plane);
cVector3f ReflectStandardWaterVector(const cVector3f &vector,
                                     const cStandardWaterPlane &plane);
cMatrixf MakeStandardWaterReflectionMatrix(
    const cStandardWaterPlane &plane);
cMatrixf MakeStandardWaterReflectedView(const cMatrixf &mainView,
                                        const cStandardWaterPlane &plane);
cMatrixf MakeStandardWaterReflectedViewProjection(
    const cMatrixf &mainProjection, const cMatrixf &mainView,
    const cStandardWaterPlane &plane);

} // namespace hpl
