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

import SceneTypes;
import Probe.LightProbeSampling;

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

[shader("compute")]
[numthreads(kLightProbeSampleCount, 1, 1)]
void csMain(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint probe = group.x;
    // Uniform across the group, so no lane can skip a reduction barrier.
    if (probe >= gPushConstants.probeCount || probe >= kMaxLightProbes)
        return;

    float3 posW = gProbeRequests[probe].posW;
    ProbeLightFilter filter;
    filter.excludeCount = gPushConstants.excludeCount;
    for (uint i = 0u; i < kMaxLightProbeExcludes; ++i)
        filter.excludeIds[i] = gPushConstants.excludeIds[i];
    float3 irradiance = sampleLightProbe(posW, lane, filter);

    if (lane == 0u)
    {
        LightProbeResult res;
        res.irradiance = irradiance;
        res.padding = 0.0f;
        gProbeResults[probe] = res;
    }
}
