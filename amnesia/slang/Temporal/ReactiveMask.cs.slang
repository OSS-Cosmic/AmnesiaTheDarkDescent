// Same-frame transparency masks for temporal upscaling.
//
// The opaque and final inputs are both linear HDR RGBA16F images at the
// render extent. Behavior mirrors TemporalReactiveMaskEvaluate in
// HPL2/core/include/graphics/TemporalReactiveMask.h.

[vk::binding(0, 2)] Texture2D<float4>  gOpaqueColor;
[vk::binding(1, 2)] Texture2D<float4>  gSceneColor;
[vk::binding(2, 2)] RWTexture2D<float> gReactiveMask;
[vk::binding(3, 2)] RWTexture2D<float> gCompositionMask;
[vk::binding(4, 2)] RWTexture2D<float> gResponsiveMask;

struct ReactiveMaskPC
{
    uint2  gExtent;          // render extent
    float2 gJitterPixels;    // sample offset vs pixel center, render-extent pixels, [-0.5,0.5]
    uint   gWriteResponsive; // 0 => skip the unjittered XeSS variant
    float  gLuminanceFloor;  // 1e-3 default
    float  gReactiveScale;
    float  gCompositionScale;
    float  gCompositionKnee;
    float  gChangeEpsilon;
};

// Scalar-layout offsets: uint2 0, float2 8, uint 16, then five floats at
// 20, 24, 28, 32, and 36; total push-constant size is 40 bytes.
[vk::push_constant]
ReactiveMaskPC gPushConstants;

float sanitizeComponent(float c)
{
    return isfinite(c) ? c : 0.0f;
}

float evaluateRelative(int2 clampedPixel)
{
    float4 opaque = gOpaqueColor.Load(int3(clampedPixel, 0));
    float4 finalColor = gSceneColor.Load(int3(clampedPixel, 0));

    // RGB only. Alpha in the scene image is not part of the composited
    // radiance and is usually a constant 1, which would otherwise inflate the
    // denominator and damp every response by a fixed factor.
    opaque.x = sanitizeComponent(opaque.x);
    opaque.y = sanitizeComponent(opaque.y);
    opaque.z = sanitizeComponent(opaque.z);
    finalColor.x = sanitizeComponent(finalColor.x);
    finalColor.y = sanitizeComponent(finalColor.y);
    finalColor.z = sanitizeComponent(finalColor.z);

    float num = abs(finalColor.x - opaque.x)
              + abs(finalColor.y - opaque.y)
              + abs(finalColor.z - opaque.z);
    float den = max(abs(opaque.x), abs(finalColor.x))
              + max(abs(opaque.y), abs(finalColor.y))
              + max(abs(opaque.z), abs(finalColor.z))
              + gPushConstants.gLuminanceFloor;

    // This is a per-channel L1 ratio, not a luminance difference. An
    // equal-luminance chromatic change such as opaque (1,1,1) versus final
    // (1.2,1.0,0.8) still responds.
    // The ratio compares the same linear HDR pair, so it is independent of
    // the later tone-map exposure and gamma.
    return saturate(num / den);
}

float evaluateDilatedRelative(int2 center, int2 extentMax)
{
    float relative = 0.0f;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 tap = clamp(center + int2(x, y), int2(0, 0), extentMax);
            // Explicit Load taps keep this a point-sampled, bounded 3x3
            // neighborhood and include filtered/anti-aliased transparency
            // edges without sampler filtering.
            relative = max(relative, evaluateRelative(tap));
        }
    }
    return relative;
}

float evaluateReactive(float relative)
{
    return saturate(relative * gPushConstants.gReactiveScale);
}

float evaluateComposition(float relative)
{
    // Composition strength is deliberately independent and conservative; it
    // does not force every changed pixel to full history rejection.
    return (relative > gPushConstants.gChangeEpsilon)
        ? saturate(gPushConstants.gCompositionScale
                 * saturate(relative / gPushConstants.gCompositionKnee))
        : 0.0f;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    // Check before any write so odd and boundary extents are safe.
    if (p.x >= gPushConstants.gExtent.x || p.y >= gPushConstants.gExtent.y)
        return;

    int2 extentMax = int2(int(gPushConstants.gExtent.x) - 1,
                         int(gPushConstants.gExtent.y) - 1);
    float relative = evaluateDilatedRelative(int2(p), extentMax);

    // FSR consumes these masks on the original jittered input grid, so both
    // outputs are written at the original jittered input-grid coordinates.
    gReactiveMask[p] = evaluateReactive(relative);
    gCompositionMask[p] = evaluateComposition(relative);

    if (gPushConstants.gWriteResponsive != 0)
    {
        // Subpixel/sign conversion (see notes/temporal_upscaling.md):
        // jitterPixels is the offset relative to the pixel center in
        // render-extent pixels, with each component in [-0.5, 0.5]. Geometry
        // in the jittered raster image is displaced by -jitter, so recovering
        // the unjittered grid SUBTRACTS jitterPixels/extent:
        // sampleUV = unjitteredUV - jitterPixels / extent
        //          = ((float2(p) + 0.5) - jitterPixels) / extent.
        float2 extentF = float2(float(gPushConstants.gExtent.x), float(gPushConstants.gExtent.y));
        float2 unjitteredPixel = float2(float(p.x), float(p.y));
        float2 sampleUV = (unjitteredPixel + 0.5f - gPushConstants.gJitterPixels)
                        / extentF;
        int2 responsivePixel = clamp(
            int2(floor(sampleUV * extentF)), int2(0, 0), extentMax);
        float responsiveRelative =
            evaluateDilatedRelative(responsivePixel, extentMax);

        // XeSS requires this responsive variant in unjittered image
        // coordinates. When disabled, the host binds the reactive mask view
        // to gResponsiveMask so the descriptor stays valid; this binding
        // remains untouched in that mode.
        gResponsiveMask[p] = evaluateReactive(responsiveRelative);
    }
}
