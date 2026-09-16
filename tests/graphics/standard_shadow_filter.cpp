// Headless coverage for the Standard renderer's PCSS shadow-filter math.
//
// The lighting shader calls these same functions out of
// amnesia/slang/StandardShadowFilter.h, so the arithmetic exercised here is the
// arithmetic the GPU runs. Contact hardening is entirely a property of this
// math, and the depth linearization it rests on is easy to get backwards in a
// way that looks plausible on screen, so it is pinned here rather than by eye.

#include "../../amnesia/slang/StandardShadowFilter.h"
#include "utest.h"

#include <cmath>

namespace {

// The NDC depth cMath::MatrixPerspectiveProjection stores for a view-space
// depth, i.e. the inverse of standardShadowLinearDepth.
float NdcFromLinear(float linear, float nearPlane, float farPlane) {
    const float a = farPlane / (farPlane - nearPlane);
    const float b = -(farPlane * nearPlane) / (farPlane - nearPlane);
    return (a * linear + b) / linear;
}

constexpr float kNear = 0.05f;
constexpr float kFar = 20.0f;
// A 90-degree cube face, the point-light case.
constexpr float kTanHalfFov = 1.0f;

} // namespace

UTEST(StandardShadowFilter, LinearDepthInvertsTheProjection) {
    const float depths[5] = {0.06f, 0.5f, 3.0f, 10.0f, 19.5f};
    for (float linear : depths) {
        const float ndc = NdcFromLinear(linear, kNear, kFar);
        const float roundTrip = hpl::standardShadowLinearDepth(ndc, kNear, kFar);
        ASSERT_TRUE(std::fabs(roundTrip - linear) < 1e-3f * linear);
    }
    // The near and far planes land exactly on 0 and 1.
    ASSERT_TRUE(std::fabs(hpl::standardShadowLinearDepth(0.0f, kNear, kFar) - kNear) < 1e-4f);
    ASSERT_TRUE(std::fabs(hpl::standardShadowLinearDepth(1.0f, kNear, kFar) - kFar) < 1e-2f);
}

UTEST(StandardShadowFilter, LinearDepthIsMonotonicAndNonlinear) {
    // Monotonic: a larger NDC depth is always further away. A sign slip in the
    // inversion shows up here before it ever reaches a pixel.
    float previous = 0.0f;
    for (int step = 0; step <= 20; ++step) {
        const float ndc = static_cast<float>(step) / 20.0f;
        const float linear = hpl::standardShadowLinearDepth(ndc, kNear, kFar);
        ASSERT_TRUE(linear > previous);
        previous = linear;
    }
    // Violently nonlinear near the camera: NDC 0.5 is nowhere near half way.
    const float middle = hpl::standardShadowLinearDepth(0.5f, kNear, kFar);
    ASSERT_TRUE(middle < 0.5f * kFar);
}

UTEST(StandardShadowFilter, LinearDepthRejectsDegenerateFrusta) {
    // Every degenerate case reports the far plane, which widens no penumbra.
    ASSERT_EQ(hpl::standardShadowLinearDepth(0.5f, 0.0f, kFar), kFar);
    ASSERT_EQ(hpl::standardShadowLinearDepth(0.5f, kFar, kNear), kNear);
    ASSERT_EQ(hpl::standardShadowLinearDepth(2.0f, kNear, kFar), kFar);
}

UTEST(StandardShadowFilter, PenumbraHardensOnContact) {
    const float lightSize = 0.2f;
    const float receiver = 4.0f;

    // An occluder touching the receiver casts a hard edge.
    ASSERT_EQ(hpl::standardShadowPenumbraUV(receiver, receiver, lightSize, kTanHalfFov), 0.0f);

    // Pulling the occluder toward the light widens the penumbra, monotonically.
    float previous = 0.0f;
    const float blockers[4] = {3.9f, 3.0f, 2.0f, 1.0f};
    for (float blocker : blockers) {
        const float penumbra = hpl::standardShadowPenumbraUV(receiver, blocker,
                                                             lightSize, kTanHalfFov);
        ASSERT_TRUE(penumbra > previous);
        previous = penumbra;
    }
}

UTEST(StandardShadowFilter, PenumbraScalesWithLightSize) {
    const float receiver = 4.0f;
    const float blocker = 2.0f;
    const float small = hpl::standardShadowPenumbraUV(receiver, blocker, 0.1f, kTanHalfFov);
    const float large = hpl::standardShadowPenumbraUV(receiver, blocker, 0.4f, kTanHalfFov);
    // Four times the emitter, four times the penumbra.
    ASSERT_TRUE(std::fabs(large - 4.0f * small) < 1e-5f);
}

UTEST(StandardShadowFilter, PenumbraIgnoresNonOccluders) {
    const float lightSize = 0.2f;
    // A "blocker" behind the receiver is not an occluder.
    ASSERT_EQ(hpl::standardShadowPenumbraUV(4.0f, 6.0f, lightSize, kTanHalfFov), 0.0f);
    // A zero-size emitter is a point light in the literal sense: hard edges.
    ASSERT_EQ(hpl::standardShadowPenumbraUV(4.0f, 2.0f, 0.0f, kTanHalfFov), 0.0f);
    // Degenerate frustum or depth.
    ASSERT_EQ(hpl::standardShadowPenumbraUV(4.0f, 0.0f, lightSize, kTanHalfFov), 0.0f);
    ASSERT_EQ(hpl::standardShadowPenumbraUV(4.0f, 2.0f, lightSize, 0.0f), 0.0f);
}

UTEST(StandardShadowFilter, SearchRegionGrowsWithDistance) {
    const float lightSize = 0.2f;
    float previous = 0.0f;
    const float receivers[4] = {0.5f, 1.0f, 4.0f, 16.0f};
    for (float receiver : receivers) {
        const float search =
            hpl::standardShadowSearchUV(receiver, kNear, lightSize, kTanHalfFov);
        ASSERT_TRUE(search > 0.0f);
        // In UV the search shrinks with distance even as it grows in world
        // space, because the frustum widens faster than the search does.
        if (previous > 0.0f)
            ASSERT_TRUE(search < previous);
        previous = search;
    }
    // A receiver at the near plane has nothing in front of it to search.
    ASSERT_EQ(hpl::standardShadowSearchUV(kNear, kNear, lightSize, kTanHalfFov), 0.0f);
}

UTEST(StandardShadowFilter, FilterRadiusIsClampedToItsBand) {
    const float texel = 1.0f / 512.0f;
    const float maxTexels = 16.0f;

    // Never harder than the bilinear compare PCSS replaced.
    ASSERT_EQ(hpl::standardShadowFilterRadiusUV(0.0f, texel, maxTexels), texel);
    ASSERT_EQ(hpl::standardShadowFilterRadiusUV(texel * 0.25f, texel, maxTexels), texel);
    // Never wider than the band, however far the occluder is.
    ASSERT_EQ(hpl::standardShadowFilterRadiusUV(1.0f, texel, maxTexels), texel * maxTexels);
    // In between it is the penumbra itself.
    const float middle = texel * 4.0f;
    ASSERT_EQ(hpl::standardShadowFilterRadiusUV(middle, texel, maxTexels), middle);
}

UTEST(StandardShadowFilter, ContactHardeningEndToEnd) {
    // A lamp 1m from a wall, an occluder walked from the wall to the lamp. The
    // filter radius must start at the one-texel floor and open up as the
    // occluder separates -- the whole point of the change.
    const float lightSize = 0.15f;
    const float texel = 1.0f / 256.0f;
    const float receiverNdc = NdcFromLinear(4.0f, kNear, kFar);
    const float receiver = hpl::standardShadowLinearDepth(receiverNdc, kNear, kFar);

    const float touching = hpl::standardShadowFilterRadiusUV(
        hpl::standardShadowPenumbraUV(receiver, receiver, lightSize, kTanHalfFov),
        texel, 16.0f);
    ASSERT_EQ(touching, texel);

    const float separated = hpl::standardShadowFilterRadiusUV(
        hpl::standardShadowPenumbraUV(receiver, 1.0f, lightSize, kTanHalfFov),
        texel, 16.0f);
    ASSERT_TRUE(separated > touching);
}
