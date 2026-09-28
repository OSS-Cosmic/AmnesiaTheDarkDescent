/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AMFP's posteffect_color_grading_frag.glsl: a 3D colour table
// applied to the display image, optionally cross-faded between two tables.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler (bilinear, clamp)
//   binding 1 : Texture2D<float4> sourceInput
//   binding 2 : Texture2D<float4> gradingMap
//   binding 3 : Texture2D<float4> gradingMap2
//
// Tables are the AMFP flattened 3D strips: N x N*N, N slices of N x N
// stacked vertically. AMFP sampled them as a clamped trilinear sampler3D at
// color.yxz, so green runs along x, red along the rows of a slice and blue
// selects the slice. Here the strip is a 2D texture: bilinear covers x and
// the in-slice rows (rows clamped to texel centres so slices never bleed),
// and the two nearest slices are blended by hand, which is the same result.
//
// AMFP graded the final LDR image, so this pass runs after the tonemap, on
// its display-encoded [0,1] output. Grading before it only worked for the
// Standard backend, whose tonemap is an exact linearToSRGB(x * exposure): the
// ray-traced shoulder and gamma bias re-bent the graded image, and decoding
// HDR to display clipped every highlight before the shoulder could roll it.

#include "../HostDefinitions.h"

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

[vk::binding(2, 0)]
Texture2D<float4> gradingMap;

[vk::binding(3, 0)]
Texture2D<float4> gradingMap2;

struct ColorGradingPC
{
    float crossFadeAlpha; // 0 = gradingMap only
    float lutSize;        // N; 0 = pass through
    float2 padding;
};

[vk::push_constant]
ColorGradingPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

float3 sampleGradingStrip(Texture2D<float4> strip, float3 display, float n)
{
    // sampler3D(s, t, r) = (green, red, blue); clamp-to-edge on all three.
    const float u = display.g;
    const float row = clamp(display.r * n, 0.5f, n - 0.5f);
    const float slice = clamp(display.b * n - 0.5f, 0.0f, n - 1.0f);
    const float slice0 = floor(slice);
    const float slice1 = min(slice0 + 1.0f, n - 1.0f);
    const float invHeight = 1.0f / (n * n);
    const float3 a = strip.SampleLevel(inputSampler, float2(u, (slice0 * n + row) * invHeight), 0.0f).rgb;
    const float3 b = strip.SampleLevel(inputSampler, float2(u, (slice1 * n + row) * invHeight), 0.0f).rgb;
    return lerp(a, b, slice - slice0);
}

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    const float3 source = sourceInput.Sample(inputSampler, pin.v_uv).rgb;
    const float n = gPushConstants.lutSize;
    if (n <= 0.0f)
        return float4(source, 1.0f);

    const float3 display = saturate(source);

    float3 graded = sampleGradingStrip(gradingMap, display, n);
    if (gPushConstants.crossFadeAlpha > 0.0f)
    {
        const float3 graded2 = sampleGradingStrip(gradingMap2, display, n);
        graded = (1.0f - gPushConstants.crossFadeAlpha) * graded
                 + gPushConstants.crossFadeAlpha * graded2;
    }

    return float4(saturate(graded), 1.0f);
}
