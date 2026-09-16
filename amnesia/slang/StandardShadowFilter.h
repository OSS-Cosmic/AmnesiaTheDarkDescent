#pragma once

#include "HostDefinitions.h"

// Shared host/device shadow-filter math for the Standard renderer.
//
// The PCSS arithmetic lives here rather than in the lighting shader so the
// headless tests exercise the same code the GPU runs -- the same arrangement as
// StandardCull.h. Everything is scalar: the C++ vector types carry no
// arithmetic operators, and keeping the shader side scalar too means the two
// paths cannot drift through an operator that exists on only one side.
//
// The renderer rasterizes shadows with standard Z (depth cleared to 1, near at
// 0), so a stored depth is an NDC z that is violently nonlinear -- with a near
// plane of 0.05 a stored 0.5 is a fraction of a world unit out. Every ratio
// below therefore runs in LINEAR view depth; comparing NDC depths directly
// would make the penumbra estimate meaningless.

// Ceiling on the filter radius, in texels of the tile. Bounds the cost and the
// light leaking a very wide kernel causes across depth discontinuities.
#define kStandardShadowMaxFilterTexels 12.0f

HOST_NAMESPACE_BEGIN

// View-space depth from a standard-Z NDC depth.
//
// Inverts z_ndc = (a*z + b) / z with a = f/(f-n) and b = -f*n/(f-n), which is
// what cMath::MatrixPerspectiveProjection builds. Degenerate inputs return the
// far plane: that makes a bad sample look maximally distant, which widens no
// penumbra and darkens nothing.
SLANG_PUBLIC inline float standardShadowLinearDepth(float ndcDepth,
                                                    float nearPlane,
                                                    float farPlane)
{
    float span = farPlane - nearPlane;
    float denominator = farPlane - ndcDepth * span;
    if (!(denominator > 0.0f) || !(nearPlane > 0.0f) || !(farPlane > nearPlane))
        return farPlane;
    return (nearPlane * farPlane) / denominator;
}

// Half-width, in shadow-map UV, of the region to search for blockers.
//
// Similar triangles from the emitter: an occluder anywhere between the near
// plane and the receiver can shadow it, and the widest such region is the
// light's own size scaled by how far the receiver is past the near plane.
SLANG_PUBLIC inline float standardShadowSearchUV(float receiverLinear,
                                                 float nearPlane,
                                                 float lightSize,
                                                 float tanHalfFov)
{
    if (!(lightSize > 0.0f) || !(receiverLinear > nearPlane) ||
        !(tanHalfFov > 0.0f))
        return 0.0f;
    float searchWorld = lightSize * (receiverLinear - nearPlane) / receiverLinear;
    // The tile's frustum spans this much world at the receiver's depth, and UV
    // spans [0,1] across it.
    float frustumWidth = 2.0f * receiverLinear * tanHalfFov;
    return searchWorld / frustumWidth;
}

// Half-width of the penumbra at the receiver, in shadow-map UV.
//
// The contact-hardening term: an occluder touching the receiver
// (blocker == receiver) gives zero penumbra and a hard edge, and the shadow
// softens as the two separate. A blocker at or behind the receiver is not an
// occluder at all and yields zero.
SLANG_PUBLIC inline float standardShadowPenumbraUV(float receiverLinear,
                                                   float blockerLinear,
                                                   float lightSize,
                                                   float tanHalfFov)
{
    if (!(lightSize > 0.0f) || !(blockerLinear > 0.0f) ||
        !(receiverLinear > blockerLinear) || !(tanHalfFov > 0.0f))
        return 0.0f;
    float penumbraWorld =
        lightSize * (receiverLinear - blockerLinear) / blockerLinear;
    float frustumWidth = 2.0f * receiverLinear * tanHalfFov;
    return penumbraWorld / frustumWidth;
}

// Filter radius in UV, clamped to a sane band.
//
// The floor is one texel: PCSS must never produce a HARDER edge than the plain
// bilinear compare this replaced. The ceiling bounds both the cost and the
// light leaking a very wide kernel causes across depth discontinuities.
SLANG_PUBLIC inline float standardShadowFilterRadiusUV(float penumbraUV,
                                                       float texelSize,
                                                       float maxTexels)
{
    float minimum = texelSize;
    float maximum = texelSize * maxTexels;
    if (!(penumbraUV > minimum))
        return minimum;
    if (penumbraUV > maximum)
        return maximum;
    return penumbraUV;
}

HOST_NAMESPACE_END
