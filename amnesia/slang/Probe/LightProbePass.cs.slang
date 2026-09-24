// LightProbePass — an off-screen illumination sensor for gameplay.
//
// Gameplay wants to know how brightly lit a world point is (the Amnesia
// darkness/sanity system, enemy "is the player hiding in the dark" checks). The
// base game answered that on the CPU by walking every light in the world and
// applying a distance falloff, with no occlusion for point lights at all
// (cLuxMapHelper::GetLightLevelAtPos) — so standing behind a crate inside a
// lamp's radius still read as fully lit, and area lights read as nothing.
//
// This pass answers the same question with the renderer's own data: the
// world-space light grid for which lights reach the point, ILight.sample for
// their radiance (spot cones, gobos and rect one-sidedness included), and an
// inline shadow ray per light against the scene TLAS for occlusion. Each probe
// also gathers one-bounce lighting and emission from surrounding surfaces.
// One workgroup per probe traces a fixed sphere of directions, then reduces
// their contributions. Results reach the CPU through asynchronous readback.
//
// Returns physical RGB lighting. Gameplay converts it to normalized linear
// luminance; this pass does not calculate an authored-radius light score.
//
// Dispatched in cHybridRenderer::Draw right after the light-grid build barrier,
// so the grid is populated and cWorld::PrepareFrame has already built the TLAS.

#include "../Constants.h"

import bindless;
import SceneTypes;              // LightProbeRequest / LightProbeResult
import Scene;                   // getCellLights / cellLightAt / getLight
import Material;                // traceShadowRay / traceReflectionHit
import DiffuseShading;
import Brdf;
import RayCone;                 // kTexLodTopMipBase
import PerFrame.resource;       // gPerFrame.allLightsCastShadows

[vk::binding(0, 2)]
StructuredBuffer<LightProbeRequest> gProbeRequests;

[vk::binding(1, 2)]
RWStructuredBuffer<LightProbeResult> gProbeResults;

struct LightProbePC
{
    uint probeCount;
    uint excludeCount;
    // Packed light ids (packLightId(type, slot)) the probe must ignore. The
    // player carries its own lights — the darkness "eye adaptation" ambient
    // light follows the camera and brightens *because* the player is in the
    // dark, so sensing it would cancel the very darkness that turned it on.
    uint excludeIds[kMaxLightProbeExcludes];
};

[vk::push_constant]
LightProbePC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

bool isExcluded(uint packedId)
{
    for (uint i = 0u; i < gPushConstants.excludeCount && i < kMaxLightProbeExcludes; ++i)
    {
        if (gPushConstants.excludeIds[i] == packedId)
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

float3 directLighting(float3 posW)
{
    float3 lighting = float3(0.0f);
    // Empty/out-of-grid cells supply no analytic light, with no full-bright
    // fallback. Surface emission is evaluated independently by the gather.
    CellLights cl = getCellLights(posW);

    for (uint k = 0u; k < cl.count; ++k)
    {
        uint packedId = cellLightAt(cl, k);
        if (isExcluded(packedId))
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

float3 oneBounceRadiance(float3 posW, float3 direction)
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
    for (uint k = 0u; k < cl.count; ++k)
    {
        uint packedId = cellLightAt(cl, k);
        // Filtering must also happen at the bounce: otherwise the darkness
        // assist light would illuminate walls and cancel its own penalty.
        if (isExcluded(packedId))
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

[shader("compute")]
[numthreads(kLightProbeSampleCount, 1, 1)]
void csMain(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint probe = group.x;
    // Uniform across the group, so no lane can skip a reduction barrier.
    if (probe >= gPushConstants.probeCount || probe >= kMaxLightProbes)
        return;

    float3 posW = gProbeRequests[probe].posW;
    float3 indirect = float3(0.0f);
    if (kDefaultRenderIndirectLighting != 0u)
        indirect = oneBounceRadiance(posW, probeDirection(lane));

    // Integral over the full sphere: no receiving surface normal/cosine.
    // This matches the direct sensor's sum of incoming LightSample.Li.
    gProbeSamples[lane] = indirect * (12.566370614359172f / float(kLightProbeSampleCount));
    if (lane == 0u)
        gProbeSamples[lane] += directLighting(posW);
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kLightProbeSampleCount / 2u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            gProbeSamples[lane] += gProbeSamples[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
    {
        LightProbeResult res;
        res.irradiance = gProbeSamples[0];
        res.padding = 0.0f;
        gProbeResults[probe] = res;
    }
}
