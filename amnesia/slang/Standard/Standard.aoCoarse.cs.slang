// Coarse ambient occlusion pass: quarter-resolution horizon-based AO, ported
// from enhanced's hbao_courseAO.comp.fsl.
//
// Dispatched with z = 16: slice s covers full-resolution pixel
// did.xy * 4 + (s % 4, s / 4), matching the prepare and reinterleave passes.

#include "../HostDefinitions.h"

import StandardAmbientOcclusion;

[vk::binding(0, 2)]
ConstantBuffer<AOConstants> gAOConstants;

// Positive linear view depth, one slice per full-res offset in the 4x4 block.
// Read as a storage image: it stays in GENERAL from the prepare pass.
[vk::binding(1, 2)]
RWTexture2DArray<float> aoPreparedDepthTexture;

// Standard's geometric view-space normal (right-handed, -Z forward).
[vk::binding(2, 2)]
Texture2D<float3> normalTexture;

// Output: quarter-resolution AO, 16 slices.
[vk::binding(3, 2)]
RWTexture2DArray<float> aoQuarterTexture;

void AccumulateAO(inout float ao, inout float rayPixels, uint sliceIndex,
                  uint2 fullResPos, float stepSizePixels, float2 direction,
                  float3 viewPosKernelCenter, float3 viewNormalKernelCenter,
                  float negInvR2)
{
    const AOConstants c = gAOConstants;
    int2 samplePos = int2(round(rayPixels * direction)) + int2(fullResPos);
    uint2 clampedPos = uint2(clamp(samplePos, int2(0, 0), int2(c.viewportDim) - int2(1, 1)));
    float2 uv = (float2(clampedPos) + 0.5f) * c.viewportTexel;

    float viewDepth = aoPreparedDepthTexture[uint3(clampedPos / 4u, sliceIndex)];
    float3 viewPos = ComputeViewspacePosition(c, uv, viewDepth);
    rayPixels += stepSizePixels;
    ao += ComputeAO(viewPosKernelCenter, viewNormalKernelCenter, viewPos, negInvR2);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 did : SV_DispatchThreadID)
{
    const AOConstants c = gAOConstants;
    if (did.x >= c.quarterDim.x || did.y >= c.quarterDim.y || did.z >= 16u)
        return;

    const uint sliceIndex = did.z;
    const uint2 fullResPos = did.xy * 4u + uint2(sliceIndex % 4u, sliceIndex / 4u);
    if (fullResPos.x >= c.viewportDim.x || fullResPos.y >= c.viewportDim.y)
        return;

    const float2 uv = (float2(fullResPos) + 0.5f) * c.viewportTexel;
    float viewDepth = aoPreparedDepthTexture[uint3(did.xy, sliceIndex)];
    float3 viewNormal = normalTexture[fullResPos];
    // Standard normals are -Z forward; the AO math runs in +Z-forward space.
    viewNormal.z = -viewNormal.z;
    float3 viewPos = ComputeViewspacePosition(c, uv, viewDepth);

    const float radiusToScreen = RADIUS * (0.5f / c.tanHalfFovY) * float(c.viewportDim.y);
    float radiusPixels = radiusToScreen / max(viewDepth, 1e-4f);

    // Too small to matter (distant or background): unoccluded. Enhanced wrote 0
    // (black) here, which darkened the far field.
    if (radiusPixels <= 1.0f)
    {
        aoQuarterTexture[uint3(did.xy, sliceIndex)] = 1.0f;
        return;
    }

    const float negInvR2 = NEG_INV_RADIUS_SQUARED;
    const float aoScaleFactor = 1.0f / (1.0f - VIEW_COS_BIAS);
    const float smallScaleAOFactor = SMALL_SCALE_AO_AMOUNT * aoScaleFactor * 2.0f;
    const float largeScaleAOFactor = LARGE_SCALE_AO_AMOUNT * aoScaleFactor;

    const float stepSizePixels = (radiusPixels / 4.0f) / float(NUM_STEPS + 1);
    const float alpha = (2.0f * 3.14159265359f) / float(NUM_DIRECTIONS);
    const float3 jitter = JITTER[uint(frac(sin(dot(float2(did.xy), float2(12.9898f, 78.233f))) * 43758.5453f) * 32.0f) % 32u];

    float smallScaleAO = 0.0f;
    float largeScaleAO = 0.0f;
    for (uint directionIndex = 0; directionIndex < NUM_DIRECTIONS; ++directionIndex)
    {
        const float angle = alpha * float(directionIndex);
        const float2 direction = RotateDirection(float2(cos(angle), sin(angle)), jitter.xy);
        // Jitter the first sample within the first step.
        float rayPixels = jitter.z * stepSizePixels + 1.0f;

        AccumulateAO(smallScaleAO, rayPixels, sliceIndex, fullResPos, stepSizePixels,
                     direction, viewPos, viewNormal, negInvR2);
        for (uint stepIndex = 1; stepIndex < NUM_STEPS; ++stepIndex)
        {
            AccumulateAO(largeScaleAO, rayPixels, sliceIndex, fullResPos, stepSizePixels,
                         direction, viewPos, viewNormal, negInvR2);
        }
    }

    float ao = smallScaleAO * smallScaleAOFactor + largeScaleAO * largeScaleAOFactor;
    ao /= float(NUM_DIRECTIONS * NUM_STEPS);
    aoQuarterTexture[uint3(did.xy, sliceIndex)] = saturate(1.0f - ao * 2.0f);
}
