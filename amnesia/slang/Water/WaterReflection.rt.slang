// Reduced-resolution water reflection ray tracer.
//
// gWaterReflectionRadianceHitDist holds RADIANCE, not diffuse-demodulated
// irradiance (unlike gIndirectDiffuse). Full GGX reflected-hit shading and
// one-light NEE come from the shared path-tracing backend; that is an
// intentional behaviour change from the old all-light diffuse evaluator.
// No screen-space opaque irradiance is sampled at reflected hits. The world
// TLAS, bindless materials and light grid are shared with the opaque tracer.
//
// One .spv, four entry points, same single-miss / single-hit-group SBT shape as
// PathTracePass.rt.slang: waterReflRayGen / waterReflMiss / waterReflCloseHit /
// waterReflAnyHit. They are deliberately NOT named rayGen / ptMiss / ptCloseHit
// / ptAnyHit — the host registers each RT pipeline's stages by entry-point name
// and two modules answering to the same names is an easy mistake to make.
// Dispatch extent is half width x half height.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import RayCone;
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
import bindless;             // gSceneObjects, gRtAccel, lights, grid
import Brdf;                 // SurfaceBRDF / prepareSurface / evalBrdf / sampleBrdf
import HitInfo;
import ILight;
import PerFrame.resource;
import Grid;                 // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid lookup)
import Random;
import DiffuseShading;       // shared DiffuseShading + prepareDiffuseShading
import PathTracer.PathTraceCommon; // shared path continuation and hit shading
import Utils.Color.ColorHelpers;   // sRGBToLinear (miss-shader sky term)

// Pass-private resources (set 2). Names are resolved by host reflection.
[vk::binding(0, 2)] Texture2D<float4>   gWaterGuideHalfPosViewZ;
[vk::binding(1, 2)] Texture2D<float4>   gWaterGuideHalfNormalWeight;
[vk::binding(2, 2)] RWTexture2D<float4> gWaterReflectionRadianceHitDist;
[vk::binding(3, 2)] Texture2D<float>   gWaterGuideHalfNormalSpread;

static const uint2 kWaterReflectionRngSalt = uint2(0x9E37u, 0x79B9u);

// Water reflections retain the legacy reflection visibility policy: alpha-tested
// geometry is traversable, and there is no FORCE_OPAQUE bit in these flags.
static const uint kWaterReflectionRayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
                                           | RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

// The reflection hit is always shaded. A second vertex is allowed only when
// indirect lighting is enabled.
static const uint kWaterReflectionMaxPathVertices =
    (kDefaultRenderIndirectLighting != 0u) ? 2u : 1u;

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

// Miss / hit shaders.

[shader("miss")]
void waterReflMiss(inout ScatterPayload payload)
{
    // An environment sample is valid radiance at infinity, not NRD's zero
    // distance sentinel for an untraced sample. Keep later misses from
    // replacing the distance to the first reflected surface.
    if (payload.currStep == 1u)
        payload.firstRayLength = 65504.0f;
    // Match the Standard water capture: a ray that leaves the world sees the
    // sky tint (or the clear colour when no sky box is enabled), authored in
    // display space, so open space reflects instead of reading black. The
    // opaque tracer keeps its black environment; only water reflections gain
    // this term.
    float3 display = (gPerFrame.skyBoxEnabled != 0u)
                   ? gPerFrame.skyBoxColor.rgb
                   : gPerFrame.clearColor.rgb;
    payload.radiance += payload.thp
                      * (sRGBToLinear(saturate(display)) / kSceneExposure);
    payload.status |= kPathTraceStatusTerminated;
}

[shader("anyhit")]
void waterReflAnyHit(inout ScatterPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    // Match the shared alpha test and CoverageAmount dissolve gate used by the
    // opaque path and the legacy water reflection query.
    TriangleHit hit;
    hit.instanceID     = InstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics   = attribs.barycentrics;
    VertexData v = getVertexData(hit);
    uint materialID = gSceneObjects[hit.instanceID].materialID;
    if (materialAlphaTest(v, materialID,
                          gSceneObjects[hit.instanceID].dissolveAmount))
        IgnoreHit();
}

[shader("closesthit")]
void waterReflCloseHit(inout ScatterPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID     = InstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics   = attribs.barycentrics;

    // Read currStep before pathTraceHandleHit mutates it. Direct lighting is
    // gated at the first reflected hit only; NEE at the indirect vertex stays
    // on because reaching it already means indirect lighting is enabled, and
    // emission is visible at both vertices independently of either toggle.
    bool enableAnalyticLights = (payload.currStep == 1u)
                              ? (kDefaultRenderDirectLighting != 0u)
                              : true;
    pathTraceHandleHit(triangleHit, payload, kWaterReflectionMaxPathVertices,
                       enableAnalyticLights);
}

[shader("raygeneration")]
void waterReflRayGen()
{
    uint outW, outH;
    gWaterReflectionRadianceHitDist.GetDimensions(outW, outH);
    // Category (b): halfPixel is only a direct address in the half-resolution
    // guide/output grid. No camera ray or normalized UV is reconstructed from
    // it, so there is no half-grid jitter term to apply.
    uint2 halfPixel = DispatchRaysIndex().xy;
    if (halfPixel.x >= outW || halfPixel.y >= outH)
        return;

    uint guidePosW, guidePosH;
    uint guideNormalW, guideNormalH;
    uint guideSpreadW, guideSpreadH;
    gWaterGuideHalfPosViewZ.GetDimensions(guidePosW, guidePosH);
    gWaterGuideHalfNormalWeight.GetDimensions(guideNormalW, guideNormalH);
    gWaterGuideHalfNormalSpread.GetDimensions(guideSpreadW, guideSpreadH);
    if (halfPixel.x >= guidePosW || halfPixel.y >= guidePosH
        || halfPixel.x >= guideNormalW || halfPixel.y >= guideNormalH
        || halfPixel.x >= guideSpreadW || halfPixel.y >= guideSpreadH)
    {
        gWaterReflectionRadianceHitDist[halfPixel] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    float4 guidePos = gWaterGuideHalfPosViewZ.Load(int3(halfPixel, 0));
    float4 guideNormal = gWaterGuideHalfNormalWeight.Load(int3(halfPixel, 0));
    float normalSpread = gWaterGuideHalfNormalSpread.Load(int3(halfPixel, 0));
    normalSpread = isfinite(normalSpread)
                       ? clamp(normalSpread, 0.f, 1.f)
                       : 0.f;
    if (!isValidWaterGuide(guidePos, guideNormal))
    {
        gWaterReflectionRadianceHitDist[halfPixel] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    // This water-stream salt decorrelates the producer from the opaque tracer
    // when both use the same coordinate.
    RNG rng;
    rng.init(halfPixel + kWaterReflectionRngSalt, gPerFrame.totalFrames);

    // Exactly one deterministic mirror root is traced for each contributing
    // guide texel. The guide's contribution weight is applied later by the
    // water fragment pass, so this texture remains raw incident radiance.
    // Category (b): the ray root comes from the buffered world position chosen
    // by the guide reducer, not from halfPixel. This world-position-buffer
    // construction is jitter-independent by nature; only the raster capture
    // that populated the guide has already selected the surface sample.
    float3 position = guidePos.xyz;
    float3 reflectionNormal = normalize(guideNormal.xyz);
    float3 direction = normalize(reflect(normalize(position - gPerFrame.posW), reflectionNormal));

    ScatterPayload payload = ScatterPayload(rng.s);
    payload.origin = position + reflectionNormal * kRayEps;
    payload.direction = direction;

    // Seed the reflection path with the primary camera cone at the water
    // surface. pathTraceHandleHit propagates this same cone once more from the
    // water surface to each reflected hit before sampling its material.
    // The reflection is a mirror trace off ONE reflection normal per
    // half-resolution guide texel. Wave detail finer than that texel is
    // unresolved and behaves as roughness, so widen the seeded cone by that
    // surface spread before storing it in the payload.
    // This widening is the measured spread of the reflection normals inside
    // the guide texel, produced by the reducer. It only resolves variation
    // present across full-resolution raster samples, so it is a lower bound
    // on the true subpixel normal variation.
    RayCone primaryCone = rayConePropagate(
        rayConePrimary(), length(position - gPerFrame.posW));
    primaryCone = rayConeWiden(
        primaryCone,
        rayConeSurfaceSpreadAngle(normalSpread, /*sampledSpecular=*/true));
    payload.coneWidth = primaryCone.width;
    payload.coneSpreadAngle = primaryCone.spreadAngle;

    traceScatterRay(payload, kWaterReflectionRayFlags);   // first reflected vertex
    float3 firstVertexRadiance = payload.radiance;        // emission + NEE at the reflection hit
    float  hitDistance = payload.firstRayLength;          // first hit, or distant environment

    // With a one-vertex budget, pathTraceHandleHit increments currStep from 1
    // to 2 after shading the first hit; the loop's `currStep <= 1` test then
    // prevents a continuation. With the two-vertex budget, the same test
    // permits exactly one indirect hit. RR or a miss can terminate earlier.
    while (((payload.status & kPathTraceStatusTerminated) == 0u)
           && payload.currStep <= kWaterReflectionMaxPathVertices)
        traceScatterRay(payload, kWaterReflectionRayFlags);

    float3 continuation = payload.radiance - firstVertexRadiance;
    float3 radiance = firstVertexRadiance + continuation * kWaterReflectionIndirectIntensity;

    // Replace non-finite radiance components before measuring luminance.
    radiance.x = isfinite(radiance.x) ? radiance.x : 0.f;
    radiance.y = isfinite(radiance.y) ? radiance.y : 0.f;
    radiance.z = isfinite(radiance.z) ? radiance.z : 0.f;

    // Hue-preserving firefly clamp, matching PathTracePass.rt.slang. A
    // non-positive threshold disables the clamp.
    float radianceLum = brdfLuminance(radiance);
    if (gPerFrame.fireflyClampThreshold > 0.f
        && radianceLum > gPerFrame.fireflyClampThreshold)
        radiance *= gPerFrame.fireflyClampThreshold / radianceLum;

    if (!isfinite(hitDistance) || hitDistance < 0.f)
        hitDistance = 0.f;

    gWaterReflectionRadianceHitDist[halfPixel] = float4(radiance, hitDistance);
}
