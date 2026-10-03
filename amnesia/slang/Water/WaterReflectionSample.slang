// Stage-free water reflection sampling module: no entry point, no descriptor
// bindings, and no ray-tracing operations. The result is unweighted linear
// reflection radiance, not diffuse-demodulated irradiance; this helper applies
// no Fresnel, fog, exposure, water tint, or guide contribution weight. Every
// half-resolution tap is decoded from the REBLUR back end before use.

#include "../Constants.h"
#include "../../../HPL2/extern/NRD/Shaders/NRD.hlsli"

// The reducer's normal floor rejects uncovered or degenerate guide normals.
static const float kWaterReflectionGuideNormalMinLength2 = 1.0e-8f;
// A small world-space allowance covers guide quantization without crossing edges.
static const float kWaterReflectionPlaneTolerance = 0.05f;
// Wave normals animate, so this accepts useful neighbourhood taps without requiring equality.
static const float kWaterReflectionNormalCosThreshold = 0.65f;
// Avoid treating a numerically empty accumulated weight as a valid result.
static const float kWaterReflectionWeightEpsilon = 1.0e-8f;

bool isValidWaterReflectionGuide(float4 posViewZ, float4 normalWeight)
{
    bool finitePosition = isfinite(posViewZ.x) && isfinite(posViewZ.y)
                       && isfinite(posViewZ.z);
    float normalLengthSquared = dot(normalWeight.xyz, normalWeight.xyz);
    return finitePosition
        && posViewZ.w > 0.0f
        && normalWeight.w > 0.0f
        && normalLengthSquared > kWaterReflectionGuideNormalMinLength2;
}

bool isFiniteWaterReflectionVector(float3 value)
{
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

// Classify a guide after its reducer validity check. normalAgreement is only
// written as a finite value on the compatible path.
bool isCompatibleWaterReflectionGuide(
    float4 guidePosViewZ,
    float4 guideNormalWeight,
    float3 fullPosW,
    float3 fullGeometricNormalW,
    float3 fullReflectionNormalW,
    out float normalAgreement)
{
    normalAgreement = 0.0f;

    if (!isValidWaterReflectionGuide(guidePosViewZ, guideNormalWeight))
        return false;

    float planeDistance = dot(guidePosViewZ.xyz - fullPosW, fullGeometricNormalW);
    // Compare distance to the water plane rather than viewZ. A tilted or
    // steeply viewed water plane has a legitimate depth gradient across this
    // 3x3 half-resolution window, so a viewZ rejection would discard valid taps.
    if (!isfinite(planeDistance) || abs(planeDistance) > kWaterReflectionPlaneTolerance)
        return false;

    float guideNormalLengthSquared = dot(guideNormalWeight.xyz, guideNormalWeight.xyz);
    float reflectionNormalLengthSquared = dot(fullReflectionNormalW, fullReflectionNormalW);
    if (!isfinite(guideNormalLengthSquared)
        || !isfinite(reflectionNormalLengthSquared)
        || reflectionNormalLengthSquared <= kWaterReflectionGuideNormalMinLength2)
        return false;

    float3 guideNormal = guideNormalWeight.xyz / sqrt(guideNormalLengthSquared);
    float3 reflectionNormal = fullReflectionNormalW / sqrt(reflectionNormalLengthSquared);
    normalAgreement = dot(guideNormal, reflectionNormal);
    if (!isfinite(normalAgreement)
        || normalAgreement < kWaterReflectionNormalCosThreshold)
        return false;

    return true;
}

public struct WaterReflectionSample
{
    public float3 radiance;   // unweighted linear reflection radiance
    public bool   valid;      // false when no compatible half-resolution sample exists
}

public WaterReflectionSample sampleWaterReflectionHalfRes(
    Texture2D<float4> denoisedSpecularRadianceHitDist,
    Texture2D<float4> guideHalfPosViewZ,
    Texture2D<float4> guideHalfNormalWeight,
    uint2  fullPixel,
    float3 fullPosW,
    float3 fullGeometricNormalW,
    float3 fullReflectionNormalW)
{
    WaterReflectionSample result;
    result.radiance = float3(0.0f);
    result.valid = false;

    uint halfWidth;
    uint halfHeight;
    denoisedSpecularRadianceHitDist.GetDimensions(halfWidth, halfHeight);
    if (halfWidth == 0u || halfHeight == 0u)
        return result;

    // Category (b): this is the exact inverse of the reducer's integer
    // blockOrigin = p * 2 mapping. The full pixel becomes a half-grid owner
    // address, not a normalized-UV sample, so no jitter conversion is needed.
    uint2 ownerHalfPixel = fullPixel >> 1u;
    // Pixel centres in half-grid coordinates. Preserve the full-pixel phase
    // so a flat normal does not give all four pixels identical filter weights.
    float2 halfSamplePosition = (float2(fullPixel) + 0.5f) * 0.5f - 0.5f;
    uint2 halfDim = uint2(halfWidth, halfHeight);
    int2 maxHalfPixel = int2(halfDim) - int2(1, 1);

    float3 weightedRadiance = float3(0.0f);
    float weightSum = 0.0f;
    float3 ownerRadiance = float3(0.0f);
    bool ownerCompatible = false;

    // Fixed 3x3 window; every coordinate is clamped before any texture load.
    for (uint tapIndex = 0u; tapIndex < 9u; ++tapIndex)
    {
        int2 offset = int2(int(tapIndex % 3u) - 1, int(tapIndex / 3u) - 1);
        int2 unclampedTap = int2(ownerHalfPixel) + offset;
        uint2 tapPixel = uint2(clamp(unclampedTap, int2(0, 0), maxHalfPixel));

        // Category (b): tapPixel remains an address in the half-resolution
        // denoised/guide grid; these loads do not reconstruct a camera sample
        // and must not receive a full-to-half jitter offset.
        float4 packedRadianceHitDist =
            denoisedSpecularRadianceHitDist.Load(int3(tapPixel, 0));
        float3 decodedRadiance =
            REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packedRadianceHitDist).xyz;
        float4 guidePosViewZ = guideHalfPosViewZ.Load(int3(tapPixel, 0));
        float4 guideNormalWeight = guideHalfNormalWeight.Load(int3(tapPixel, 0));

        float normalAgreement = 0.0f;
        bool compatible = isCompatibleWaterReflectionGuide(
            guidePosViewZ,
            guideNormalWeight,
            fullPosW,
            fullGeometricNormalW,
            fullReflectionNormalW,
            normalAgreement);
        if (!compatible)
            continue;

        if (!isFiniteWaterReflectionVector(decodedRadiance))
            continue;

        bool isOwner = tapPixel.x == ownerHalfPixel.x
                    && tapPixel.y == ownerHalfPixel.y;
        if (isOwner)
        {
            ownerRadiance = decodedRadiance;
            ownerCompatible = true;
        }

        float2 spatialWeight = saturate(1.0f
            - abs(float2(unclampedTap) - halfSamplePosition) / 1.5f);
        float imageDistanceWeight = spatialWeight.x * spatialWeight.y;
        float normalAgreementWeight = saturate(
            (normalAgreement - kWaterReflectionNormalCosThreshold)
            / (1.0f - kWaterReflectionNormalCosThreshold));
        float tapWeight = imageDistanceWeight * normalAgreementWeight;
        if (tapWeight <= 0.0f)
            continue;

        weightedRadiance += decodedRadiance * tapWeight;
        weightSum += tapWeight;
    }

    if (weightSum > kWaterReflectionWeightEpsilon)
    {
        result.radiance = weightedRadiance / weightSum;
        result.valid = true;
        return result;
    }

    // Thin edges can leave no positive weighted neighbourhood contribution even
    // though the owning representative is the correct compatible sample.
    if (ownerCompatible)
    {
        result.radiance = ownerRadiance;
        result.valid = true;
    }
    return result;
}
