#pragma once
// Shared surface shading + ReSTIR DI helpers.
//
// Extracted from MainCompositePass.cs.slang so every pass that shades a
// resolved TriangleHit does it the same way: albedo + emission prep and
// NEE-over-all-lights direct lighting (standard diffuse lobe, kDiffuseBrdf --
// demodulated, pi-compensated knobs), plus the reservoir build / temporal /
// spatial reuse core shared by the direct-lighting passes. Keeping one copy
// avoids the consumers drifting on the lighting convention.

#include "HostDefinitions.h"
#include "Constants.h"

import SceneTypes;
import Scene;
import bindless;
import Brdf;                    // decodeMaterialRoughness (diffuse-lobe roughness)
import ILight;
import PerFrame.resource;
import Grid;                    // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid cull)
import Random;                  // RNG for soft-shadow disk sampling
import Reservoir;               // ReSTIR DI light-selection reservoir
// DiffuseShading + prepareDiffuseShading now live in the shared module; re-export
// so this module's consumers (composite, water, both direct passes) still see them.
__exported import DiffuseShading;

static const float kShadeRayEps = 1.0e-3f;

// The per-light shade (sample -> NdotL-cull -> ray-traced shadow ->
// Li*NdotL*f_d/albedo with the standard diffuse lobe) is shadeAnalyticLight in
// the shared DiffuseShading module (re-exported above). Demodulated: returns
// albedo-free radiance -- the caller applies albedo where outgoing radiance is
// needed. V here is toward the camera (these wrappers shade camera-visible
// points: composite, water, direct pass).
static const float3 kReservoirLum = float3(0.2126f, 0.7152f, 0.0722f);

// --- ReSTIR DI helpers ----------------------------------------------------
// Deterministic primary diffuse term composited after NRD. Keep this partition
// out of secondary-bounce lighting, which still needs every analytic light.
bool isDirectGoboLight(uint lightIndex)
{
    if (lightIdType(lightIndex) != kLightTypeSpot)
        return false;
    SpotLight light = gSpotLights[lightIdSlot(lightIndex)];
    return light.goboTextureIndex != kInvalidTextureIndex && light.sourceRadius <= 0.f;
}

float3 evaluateDirectGoboLighting(DiffuseShading sd)
{
    float3 result = float3(0.f);
    float3 V = normalize(gPerFrame.posW - sd.posW);
    float3 origin = sd.posW + sd.normalW * kShadeRayEps;
    CellLights cl = getCellLights(sd.posW);
    for (uint k = 0u; k < cl.count; ++k)
    {
        uint li = cellLightAt(cl, k);
        if (isDirectGoboLight(li))
            result += shadeAnalyticLight(sd.posW, sd.normalW, V,
                sd.surfaceFootprintW, sd.rough, li, origin);
    }
    return result;
}

// Discard excluded history before merge: a zero target alone would still add
// its confidence M, suppressing the remaining lights during normalization.
void mergeDirectReservoir(inout Reservoir result, Reservoir candidate, float target,
                          inout RNG rng)
{
    if (candidate.isEmpty() || isDirectGoboLight(candidate.y))
        return;
    result.merge(candidate, target, rng);
}

// The direct pass is split into a reservoir build + temporal reuse (the
// reworked DirectLightingPass) and a spatial reuse + resolve
// (DirectSpatialReusePass). These three helpers are the shared shading core.

// Target function (luminance of the unshadowed demodulated contribution) of one
// light at a shading point — the RIS / reuse weight. Re-evaluated at the
// receiving pixel whenever a neighbour's or previous frame's sample is reused.
float directTargetPdf(DiffuseShading sd, uint lightIndex)
{
    if (isDirectGoboLight(lightIndex))
        return 0.f;
    float3 V = normalize(gPerFrame.posW - sd.posW);
    return dot(unshadowedAnalyticLight(sd.posW, sd.normalW, V,
                                              sd.surfaceFootprintW, sd.rough,
                                              lightIndex),
               kReservoirLum);
}

// Build a light-selection reservoir: streaming RIS over the cell's lights with
// the unshadowed-luminance target. No shadow ray — visibility is resolved later
// with a single ray. Empty cells return an empty reservoir.
Reservoir buildDirectReservoir(DiffuseShading sd, inout RNG rng)
{
    Reservoir r; r.init();

    CellLights cl = getCellLights(sd.posW);
    float3 V  = normalize(gPerFrame.posW - sd.posW);
    for (uint k = 0u; k < cl.count; ++k)
    {
        uint li = cellLightAt(cl, k);
        if (isDirectGoboLight(li))
            continue;
        float pHat = dot(unshadowedAnalyticLight(sd.posW, sd.normalW, V,
                                                        sd.surfaceFootprintW,
                                                        sd.rough, li),
                         kReservoirLum);
        r.streamCandidate(li, pHat, rng);
    }
    r.finalizeBuild();
    return r;
}

// Resolve a reservoir to demodulated irradiance: one soft (blue-noise disk)
// shadow ray for the selected light, weighted by the UCW.
float3 resolveDirectReservoir(DiffuseShading sd, Reservoir r, inout RNG rng)
{
    if (r.isEmpty() || isDirectGoboLight(r.y))
        return float3(0.f);
    float3 V = normalize(gPerFrame.posW - sd.posW);
    float3 origin = sd.posW + sd.normalW * kShadeRayEps;
    float3 shaded = shadeAnalyticLightSoft(sd.posW, sd.normalW, V,
                                                  sd.surfaceFootprintW, sd.rough,
                                                  r.y, origin, rng);
    return shaded * r.W;
}
