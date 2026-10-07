// SPDX-License-Identifier: GPL-3.0
// Shared physical point sensor for gameplay and translucent object lighting.
#include "../Constants.h"

import bindless;
import SceneTypes;              // LightProbeRequest / LightProbeResult
import Scene;                   // getCellLights / cellLightAt / getLight
import Material;                // traceShadowRay / traceReflectionHit
import DiffuseShading;
import Brdf;
import RayCone;                 // kTexLodTopMipBase
import PerFrame.resource;       // gPerFrame.allLightsCastShadows

public struct ProbeLightFilter
{
    public uint excludeCount;
    public uint excludeIds[kMaxLightProbeExcludes];
};

bool probeLightExcluded(uint packedId, ProbeLightFilter filter)
{
    for (uint i = 0u; i < filter.excludeCount && i < kMaxLightProbeExcludes; ++i)
    {
        if (filter.excludeIds[i] == packedId)
            return true;
    }
    return false;
}

// Equal-solid-angle Fibonacci sphere. No frame/camera seed: a stationary
// sensor should not flicker between gameplay updates. Midpoints avoid poles.
float3 probeDirection(uint sampleIndex)
{
    float z = 1.0f - 2.0f * (float(sampleIndex) + 0.5f)
                           / float(kLightProbeSampleCount);
    float phi = float(sampleIndex) * 2.399963229728653f;
    float r = sqrt(max(0.0f, 1.0f - z * z));
    return float3(r * cos(phi), r * sin(phi), z);
}

float3 probeDirectLighting(float3 posW, ProbeLightFilter filter)
{
    float3 lighting = float3(0.0f);
    // Empty/out-of-grid cells supply no analytic light, with no full-bright
    // fallback. Surface emission is evaluated independently by the gather.
    CellLights cl = getCellLights(posW);
    const uint nDir = getDirectionalLightCount();

    // Cell lights first, then the directional lights (not in the grid).
    for (uint k = 0u; k < cl.count + nDir; ++k)
    {
        uint packedId = k < cl.count ? cellLightAt(cl, k)
                                     : directionalLightId(k - cl.count);
        if (probeLightExcluded(packedId, filter))
            continue;

        ILight light = getLight(packedId);

        // A probe has no surface, so there is no footprint to size a gobo
        // mip from; 0 makes cubeGoboTexLodBase pick the top mip.
        LightSample ls;
        if (!light.sample(posW, 0.0f, ls))
            continue;   // outside a spot cone / gobo projection, or behind a rect light

        // Same shadow policy as the shading paths (DiffuseShading.slang): by
        // default every light casts, and the authored flag only decides in the
        // legacy opt-out mode. Each contribution uses one visibility ray.
        if ((gPerFrame.allLightsCastShadows != 0u || light.castsShadow()) &&
            !traceShadowRay(posW, ls.dir, ls.distance))
            continue;

        lighting += ls.Li;
    }
    return lighting;
}

float3 probeOneBounceRadiance(float3 posW, float3 direction, ProbeLightFilter filter)
{
    TriangleHit hit;
    // Uses the opaque/GI mask, alpha testing and nearest-hit traversal. Shadow
    // replacement hulls and translucent materials must not be shaded as walls.
    if (!traceReflectionHit(posW, direction, 1.0e30f, hit))
        return float3(0.0f);

    VertexData v = getVertexData(hit);
    uint materialID = gSceneObjects[hit.instanceID].materialID;
    // A point sensor has no camera footprint: sample top mip, as for direct
    // probe gobos. Reuse the path tracer's material and normal-map evaluation.
    DiffuseShading sd = prepareDiffuseShading(v, materialID,
        gSceneObjects[hit.instanceID].illuminationAmount,
        kTexLodTopMipBase, 0.0f, /*decodeRoughness=*/true, /*applyNormalMap=*/true);
    SurfaceBRDF surface = prepareSurface(loadDiffuseMaterial(materialID),
                                         sd.normalW, v.texC, kTexLodTopMipBase);
    float3 radiance = sd.emission;
    float3 origin = sd.posW + sd.normalW * 1.0e-3f;
    CellLights cl = getCellLights(sd.posW);
    const uint nDir = getDirectionalLightCount();
    for (uint k = 0u; k < cl.count + nDir; ++k)
    {
        uint packedId = k < cl.count ? cellLightAt(cl, k)
                                     : directionalLightId(k - cl.count);
        // Filtering must also happen at the bounce: otherwise the darkness
        // assist light would illuminate walls and cancel its own penalty.
        if (probeLightExcluded(packedId, filter))
            continue;
        float3 L, LiNdotL;
        if (shadeAnalyticLightRadiance(sd.posW, sd.normalW,
                                      sd.surfaceFootprintW, packedId,
                                      origin, L, LiNdotL))
            radiance += evalBrdf(surface, -direction, L) * LiNdotL;
    }
    // Prevent a malformed material from poisoning the entire group reduction.
    return float3(isfinite(radiance.x) ? max(radiance.x, 0.0f) : 0.0f,
                  isfinite(radiance.y) ? max(radiance.y, 0.0f) : 0.0f,
                  isfinite(radiance.z) ? max(radiance.z, 0.0f) : 0.0f);
}

groupshared float3 gProbeSamples[kLightProbeSampleCount];

// All 64 lanes must call this with the same position/filter. Only lane zero
// consumes the returned reduction. Fixed directions keep stationary probes stable.
public float3 sampleLightProbe(float3 posW, uint lane, ProbeLightFilter filter)
{
    float3 indirect = float3(0.0f);
    if (kDefaultRenderIndirectLighting != 0u)
        indirect = probeOneBounceRadiance(posW, probeDirection(lane), filter);

    // Integral over the full sphere: no receiving surface normal/cosine.
    // This matches the direct sensor's sum of incoming LightSample.Li.
    gProbeSamples[lane] = indirect * (12.566370614359172f / float(kLightProbeSampleCount));
    if (lane == 0u)
        gProbeSamples[lane] += probeDirectLighting(posW, filter);
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kLightProbeSampleCount / 2u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            gProbeSamples[lane] += gProbeSamples[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    return gProbeSamples[0];
}
