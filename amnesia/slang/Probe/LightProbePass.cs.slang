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
// inline shadow ray per light against the scene TLAS for occlusion. One thread
// per probe, one workgroup total — the request count is tiny (the player sensor
// asks for five body points) and the results are copied to a host-readback
// buffer that the CPU picks up a few frames later.
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
import Material;                // traceShadowRay
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
LightProbePC gPushConstants;

bool isExcluded(uint packedId)
{
    for (uint i = 0u; i < gPushConstants.excludeCount && i < kMaxLightProbeExcludes; ++i)
    {
        if (gPushConstants.excludeIds[i] == packedId)
            return true;
    }
    return false;
}

[shader("compute")]
[numthreads(kMaxLightProbes, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint probe = tid.x;
    if (probe >= gPushConstants.probeCount || probe >= kMaxLightProbes)
        return;

    LightProbeRequest req = gProbeRequests[probe];

    LightProbeResult res;
    res.irradiance  = float3(0.0f);
    res.padding = 0.0f;

    // Out-of-grid probes report darkness rather than lightLevelAt's
    // "don't darken" full-bright fallback: that fallback exists so distant
    // GEOMETRY doesn't fade to black, which is the opposite of what a sensor
    // wants. The grid is camera-centered and 62.5 units across, so anything
    // gameplay probes (the player, props near him) is always inside it; a probe
    // that falls out is a bug or a teleport mid-frame, and reading dark is the
    // safe answer for a darkness-driven system.
    CellLights cl = getCellLights(req.posW);

    for (uint k = 0u; k < cl.count; ++k)
    {
        uint packedId = cellLightAt(cl, k);
        if (isExcluded(packedId))
            continue;

        ILight light = getLight(packedId);

        // A probe has no surface, so there is no footprint to size a gobo
        // mip from; 0 makes cubeGoboTexLodBase pick the top mip.
        LightSample ls;
        if (!light.sample(req.posW, 0.0f, ls))
            continue;   // outside a spot cone / gobo projection, or behind a rect light

        // Same shadow policy as the shading paths (DiffuseShading.slang): by
        // default every light casts, and the authored flag only decides in the
        // legacy opt-out mode. Each contribution uses one visibility ray.
        if ((gPerFrame.allLightsCastShadows != 0u || light.castsShadow()) &&
            !traceShadowRay(req.posW, ls.dir, ls.distance))
            continue;

        res.irradiance  += ls.Li;
    }

    gProbeResults[probe] = res;
}
