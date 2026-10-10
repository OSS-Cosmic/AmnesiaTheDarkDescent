// Half-resolution water reflection front-end packing for NRD.
//
// This is a specular-only signal: each water ray is deterministic, but a
// half-resolution guide texel can aggregate full-resolution reflection normals
// with a measured spread. The normal, position, viewZ, and motion guides all
// come from the water surface, never from the underwater background behind it.

#include "../HostDefinitions.h"
#include "../Constants.h"
#include "../../../HPL2/extern/NRD/Shaders/NRD.hlsli"

import SceneTypes;
import PerFrame.resource;
import Water.WaterNormalSpread;

// Pass-private resources (set 2). Resource names are resolved by host reflection.
[vk::binding(0, 2)] Texture2D<float4>   gWaterGuideHalfPosViewZ;
[vk::binding(1, 2)] Texture2D<float4>   gWaterGuideHalfNormalWeight;
[vk::binding(2, 2)] Texture2D<float2>   gWaterGuideHalfVelocity;
[vk::binding(3, 2)] Texture2D<float4>   gWaterReflectionRadianceHitDist;
[vk::binding(4, 2)] RWTexture2D<float4> gWaterNrdNormalRoughness;
[vk::binding(5, 2)] RWTexture2D<float>  gWaterNrdViewZ;
[vk::binding(6, 2)] RWTexture2D<float4> gWaterNrdSpecularRadianceHitDist;
// This is a private writable copy because REBLUR's stabilization pass does
// PushOutput(IN_MV), so NRD writes the motion texture it is handed. Sharing
// the engine's velocity texture would corrupt velocities read by other passes.
[vk::binding(7, 2)] RWTexture2D<float2> gWaterNrdMotionVectors;
[vk::binding(8, 2)] Texture2D<float>    gWaterGuideHalfNormalSpread;

bool isValidWaterGuide(float4 posViewZ, float4 normalWeight)
{
    bool finitePosition = isfinite(posViewZ.x) && isfinite(posViewZ.y)
                       && isfinite(posViewZ.z);
    float normalLengthSquared = dot(normalWeight.xyz, normalWeight.xyz);
    return finitePosition
        && posViewZ.w > 0.0f
        && normalWeight.w > 0.0f
        && normalLengthSquared > 1.0e-8f;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    gWaterNrdViewZ.GetDimensions(w, h);
    // Category (b): p directly addresses the half-resolution guide and NRD
    // resources. It is not converted to a camera UV, so no jitter offset is
    // added while packing this grid.
    uint2 p = tid.xy;
    if (p.x >= w || p.y >= h)
        return;

    // Category (b): the engine stores currentUV - previousUV in normalized
    // full-image UV units. NRD applies motion as pixelUvPrev = pixelUv + mv,
    // both normalized, so the same values stay correct at NRD's
    // half-resolution resource size and need no extent factor here; only
    // NrdIntegration supplies the sign conversion via motionVectorScale, and
    // this shader does not flip or add jitter to it.
    gWaterNrdMotionVectors[p] = gWaterGuideHalfVelocity.Load(int3(p, 0));

    float4 posViewZ = gWaterGuideHalfPosViewZ.Load(int3(p, 0));
    float4 normalWeight = gWaterGuideHalfNormalWeight.Load(int3(p, 0));
    bool validWaterGuide = isValidWaterGuide(posViewZ, normalWeight);

    if (validWaterGuide)
    {
        // The ray is one deterministic mirror ray, but the half-resolution
        // texel aggregates full-resolution guide samples whose reflection
        // normals disagree, so the signal REBLUR sees within a texel is a
        // lobe of the measured width, not a mirror. A texel whose normals all
        // agree still measures alpha 0 and therefore still packs linear
        // roughness 0, exactly reproducing the previous behaviour. This is
        // the WATER surface's normal, not the reflected geometry's normal.
        float spreadAlpha = gWaterGuideHalfNormalSpread.Load(int3(p, 0));
        float linearRoughness = waterNrdLinearRoughness(spreadAlpha);
        gWaterNrdNormalRoughness[p] =
            NRD_FrontEnd_PackNormalAndRoughness(
                normalize(normalWeight.xyz), linearRoughness, 0.0f);
        gWaterNrdViewZ[p] = posViewZ.w;

        float4 radiance =
            gWaterReflectionRadianceHitDist.Load(int3(p, 0));
        float rawDist = radiance.w;
        float specularNormHitDist = 0.0f;
        // A raw distance of zero means no data and must stay zero.
        // Environment misses carry a distant positive value from the tracer.
        // Do not pass zero to GetNormHitDist: NRD reserves zero for no data,
        // while the helper deliberately replaces zero with epsilon.
        if (rawDist > 0.0f)
            specularNormHitDist = REBLUR_FrontEnd_GetNormHitDist(
                rawDist, posViewZ.w,
                kNrdHitDistanceParameters, linearRoughness);

        gWaterNrdSpecularRadianceHitDist[p] =
            REBLUR_FrontEnd_PackRadianceAndNormHitDist(
                radiance.rgb, specularNormHitDist, true);
    }
    else
    {
        gWaterNrdNormalRoughness[p] = float4(0.0f);
        gWaterNrdViewZ[p] = NRD_INF;
        gWaterNrdSpecularRadianceHitDist[p] =
            REBLUR_FrontEnd_PackRadianceAndNormHitDist(
                float3(0.0f), 0.0f, true);
    }
}
