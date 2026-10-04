// Reference per-pixel path tracer — two-channel output (diffuse + specular).
//
// This pass outputs INDIRECT diffuse only (albedo-demodulated). The ReSTIR DI
// chain runs before this pass and is filtered independently. The composite
// adds direct lighting to REBLUR's indirect output after denoising.
// Primary-hit emission is still the composite's job (see rayGen).
//
// History: derived from the since-deleted SurfelRayTrace.rt.slang when the
// surfel GI cache was retired. The path is rooted at screen pixels instead of
// surfels, and nothing of the cache survives — no gather / finalize, no
// lifecycle, no gSurfel* buffers.
//
// BSDF: every scatter (primary and bounce) importance-samples the full
// Cook-Torrance GGX BSDF of Brdf.slang via sampleBrdfLobe() — a diffuse lobe
// plus a GGX specular lobe. The PRIMARY lobe is picked by a screen-space Bayer
// dither with a clamped probability; bounces keep the 0.05/0.95 white-noise
// pick through sampleBrdf(). The returned weight is already
// f·N.L/pdf with the lobe-selection probability folded in, so nothing else
// (cosine, albedo, 1/pSpec) is applied on top. NEE weights the light's raw
// Li·NdotL with evalBrdf() for the same reason.
//
// TWO-CHANNEL SPLIT: the lobe picked at the PRIMARY hit routes the whole path
// into one of two outputs.
//   * gIndirectDiffuse is DEMODULATED: the primary throughput is seeded with
//     weight / albedo, so what lands there is indirect irradiance divided by
//     the primary surface's albedo — INDIRECT only. MainCompositePass fetches
//     the ReSTIR DI chain's demodulated direct irradiance separately and
//     re-modulates the two together with a single `sd.albedo *`.
//   * gIndirectSpecular is NOT demodulated: there is no single albedo to divide
//     out (F0 and roughness vary per texel), so it carries radiance directly.
// Bounce albedo (every vertex AFTER the primary hit) is inside the BSDF weight
// as usual — only the primary diffuse factor is divided out.
//
// There is no primary ray: the V-buffer already holds the primary hit — from
// the raster pass, POM-corrected — so rayGen recovers it the way
// MainCompositePass / DirectLightingPass do and traces only the scattered rays.
//
// The payload, the trace submission and the hit shading / continuation live in
// PathTracer/PathTraceCommon.slang so a reduced-resolution producer can reuse
// the same estimator. What stays here is stage-specific: the four entry points,
// the output descriptors, and rayGen's V-buffer reconstruction, Bayer lobe
// pick, demodulation and channel routing.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import RayCone;
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
import bindless;             // gPackedHitInfo, gSceneObjects, gRtAccel, lights, grid
import Brdf;                 // SurfaceBRDF / prepareSurface / evalBrdf / sampleBrdf
import HitInfo;
import ILight;
import PerFrame.resource;
import Grid;                 // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid lookup)
import Random;
import DiffuseShading;       // shared DiffuseShading + prepareDiffuseShading
import PathTracer.PathTraceCommon; // shared path continuation and hit shading

// Pass-private (set 2). Names are resolved by reflection in
// RIProgram::bindDescriptors, so they must match the host side exactly.
[vk::binding(0, 2)] RWTexture2D<float4> gIndirectDiffuse;      // DEMODULATED indirect diffuse (.a = history count)
[vk::binding(1, 2)] RWTexture2D<float4> gIndirectKeyOut;       // surface key (viewZ, normal.xyz)
// Slot 2 is a gap; the remaining bindings keep their numbers so the host-side
// layout stays stable.
[vk::binding(3, 2)] RWTexture2D<float4> gIndirectSpecular;     // indirect specular, NOT demodulated
[vk::binding(4, 2)] RWTexture2D<float4> gIndirectKeyExtra;     // .x = primary GGX alpha; .y = diffuse primary hit distance (m); .z = specular primary hit distance (m); .w reserved
// Slot 5 is now free: the denoised ReSTIR DI result is consumed by MainCompositePass, not here.

// AREA_3X3 requires the primary lobe probability to be clamped to [1/4; 3/4].
// The dither takes 4 distinct values per 2x2 tile, so this guarantees that any
// 2x2 block — and therefore any 3x3 window — contains at least one pixel that
// picked specular and at least one that picked diffuse, exactly what NRD's
// hit-distance reconstruction needs. The +frameIndex rotation is the "global
// temporal jitter" requested by NRD's README; it is a uniform permutation of
// the tile, so it preserves the guarantee. This replaces white noise for the
// primary lobe pick ONLY.
static const float kPrimaryLobeProbMin = 0.25f;

// Ordered 2x2 Bayer rank in [0;3] with a global temporal rotation.
uint bayer2x2ui(uint2 samplePos, uint frameIndex)
{
    uint2 p = samplePos & 1u;
    uint  b = (p.y << 1u) | (p.y ^ p.x);
    return (b + frameIndex) & 0x3u;
}

float bayer2x2(uint2 samplePos, uint frameIndex)
{
    return (float(bayer2x2ui(samplePos, frameIndex)) + 0.5f) * 0.25f;
}

// Miss / hit shaders.

[shader("miss")]
void ptMiss(inout ScatterPayload scatterPayload)
{
    // Environment map evaluator isn't ported — treat the miss as black
    // radiance and terminate. When an env map binding gets added, this is
    // where the lookup goes.
    scatterPayload.status |= kPathTraceStatusTerminated;
}

[shader("anyhit")]
void ptAnyHit(inout ScatterPayload scatterPayload, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test (+ CoverageAmount dissolve) against the diffuse material —
    // the single shared gate, so fading objects also fade out of the GI.
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
void ptCloseHit(inout ScatterPayload scatterPayload, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID     = InstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics   = attribs.barycentrics;
    pathTraceHandleHit(triangleHit, scatterPayload, kPathTraceMaxBounces, true);
}

[shader("raygeneration")]
void rayGen()
{
    uint outW, outH;
    gIndirectDiffuse.GetDimensions(outW, outH);
    // Camera-jitter audit (a): ray generation is dispatched at the full render
    // extent, which is also the extent of gPackedHitInfo and all four outputs.
    // `pixel` therefore addresses the primary raster sample directly; there is
    // no camera ray reconstruction or half-resolution remap that could accept
    // an additional jitter term here.
    uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= outW || pixel.y >= outH)
        return;

    // The primary hit comes from the full-resolution V-buffer (rasterized
    // primary visibility with POM-corrected barycentrics), not from `pixel`
    // being converted into a camera ray. Its world position below is therefore
    // already the raster sample's position and needs no second jitter term.
    // No primary ray is traced here; a miss in gPackedHitInfo is a real miss.
    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixel]);
    if (!hitInfo.isValid())
    {
        gIndirectDiffuse[pixel]  = float4(0.f, 0.f, 0.f, 0.f);
        gIndirectSpecular[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        gIndirectKeyOut[pixel]   = float4(0.f, 0.f, 0.f, 0.f); // zero normal → rejected
        // NRD reserves a raw hit distance of 0 for no hit / sky.
        gIndirectKeyExtra[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    TriangleHit th = hitInfo.getTriangleHit();
    float triLod;
    VertexData v = getVertexDataAndLodConstant(th, triLod);
    UniformObject obj = gSceneObjects[th.instanceID];
    // The primary cone uses the geometric interpolated normal. It must be
    // captured before prepareDiffuseShading applies the normal map, because
    // cone-to-surface projection is a geometry term.
    float hitT = length(v.posW - gPerFrame.posW);
    RayCone cone = rayConePropagate(rayConePrimary(), hitT);
    float texLodBase = rayConeTexLodBase(cone, triLod, v.normalW,
                                         normalize(v.posW - gPerFrame.posW));
    float surfaceFootprintW = rayConeSurfaceFootprint(cone, v.normalW,
                                                      normalize(v.posW - gPerFrame.posW));
    DiffuseShading sd = prepareDiffuseShading(v, obj.materialID, obj.illuminationAmount,
                                              texLodBase, surfaceFootprintW,
                                              /*decodeRoughness=*/true,
                                              /*applyNormalMap=*/true);
    // Use the bumped normal for both the BSDF and the NRD guide below, so
    // specular filtering follows the same surface orientation as the traced lobe.
    SurfaceBRDF s = prepareSurface(loadDiffuseMaterial(obj.materialID), sd.normalW, v.texC,
                                   texLodBase);

    RNG rng;
    rng.init(pixel, gPerFrame.totalFrames);

    // Category (b): this view vector is camera position minus the V-buffer hit
    // position, not a view vector reconstructed from a pixel index. It is a
    // world-space geometric direction and must remain free of projection jitter.
    float3 V = normalize(gPerFrame.posW - sd.posW);
    float pSpecPrimary = brdfSpecularProbability(s, V, kPrimaryLobeProbMin);

    float3 diffuseSum  = float3(0.f);
    float3 specularSum = float3(0.f);
    float  diffuseHitDistance  = 0.f;
    float  specularHitDistance = 0.f;
    for (uint sampleIdx = 0u; sampleIdx < kPathTraceSamplesPerPixel; ++sampleIdx)
    {
        // Pick the primary lobe with screen-space Bayer dithering. Adding
        // sampleIdx keeps the strata distinct if kPathTraceSamplesPerPixel is
        // ever raised above 1.
        float lobeU = bayer2x2(pixel, gPerFrame.totalFrames + sampleIdx);

        // Importance-sample the primary BSDF; the lobe that comes back decides
        // which channel this whole path lands in.
        float3 L;
        float3 weight;
        bool   sampledSpecular;
        if (!sampleBrdfLobe(s, V, pSpecPrimary, lobeU, rng, L, weight, sampledSpecular))
            continue;   // below the horizon / degenerate — nothing to trace

        ScatterPayload payload = ScatterPayload(rng.s);
        // Start every traced path at the primary V-buffer footprint; secondary
        // hits add their travel distance to this cone in pathTraceHandleHit.
        payload.coneWidth       = cone.width;
        payload.coneSpreadAngle = cone.spreadAngle;
        payload.origin    = sd.posW + sd.normalW * kRayEps;
        payload.direction = L;
        // Diffuse is demodulated: dividing the primary albedo out of the seed
        // throughput is what upholds the contract the composite's `sd.albedo *`
        // restores. Specular keeps its weight as-is — no single albedo exists to
        // divide by, and F0/roughness vary per texel.
        payload.thp = sampledSpecular ? weight
                                      : weight / max(sd.albedo, float3(1e-4f));

        while (!((payload.status & kPathTraceStatusTerminated) != 0u) && payload.currStep <= kPathTraceMaxBounces)
            traceScatterRay(payload, kPathTraceOpaqueRayFlags,
                            kRayMaskOpaque | kRayMaskTranslucent);

        // The traced ray IS the sampled lobe's ray, so its first-bounce distance
        // belongs to that lobe. The other lobe stays at zero this frame because
        // it was not sampled: this is NRD's "skipped lobe" encoding, which is
        // why the host enables HitDistanceReconstructionMode.
        if (sampledSpecular)
            specularHitDistance = payload.firstRayLength;
        else
            diffuseHitDistance = payload.firstRayLength;

        // The primary lobe probability floor raises the worst-case selection
        // boost from at most 20x (old 0.05 floor) to at most 4x (0.25 floor),
        // so fewer samples cross this threshold and the clamp fires less often;
        // this clamp never touches hit distance.
        // Firefly clamp: cap this sample's luminance at gPerFrame.fireflyClampThreshold
        // by scaling the whole float3, so hue survives where a per-channel clamp would
        // shift it. Note what is capped is DEMODULATED irradiance (diffuse has the
        // primary albedo divided out above), not final radiance. Threshold <= 0 disables.
        float3 sampleRadiance = payload.radiance;
        float  sampleLum      = brdfLuminance(sampleRadiance);
        if (gPerFrame.fireflyClampThreshold > 0.f && sampleLum > gPerFrame.fireflyClampThreshold)
            sampleRadiance *= gPerFrame.fireflyClampThreshold / sampleLum;

        if (sampledSpecular) specularSum += sampleRadiance;
        else                 diffuseSum  += sampleRadiance;

        // Carry the path's RNG state back so the next sample is decorrelated.
        rng.s = uint2(payload.rngHi, payload.rngLo);
    }

    // .a is unused downstream — NrdPack reads rgb only — and is written as 1
    // to keep the RGBA16F layout meaningful.
    //
    // With kPathTraceSamplesPerPixel == 1 the primary lobe is picked by a
    // screen-space Bayer dither with a clamped probability, so exactly ONE of
    // these two sums is non-zero for a given pixel/frame and the other writes
    // black. That is expected, not a bug: REBLUR's hit-distance reconstruction
    // and temporal accumulation resolve both channels over frames. Bounce lobe choices remain the stochastic white-noise
    // picks made by sampleBrdf().
    //
    // NOTE: primary-hit emission deliberately stays in MainCompositePass. This
    // output is denoised downstream by REBLUR; an illumination-map's sharp
    // emissive texels would be blurred across the surface if they went through
    // it.
    gIndirectDiffuse[pixel]  = float4(diffuseSum  / float(kPathTraceSamplesPerPixel), 1.0f);
    gIndirectSpecular[pixel] = float4(specularSum / float(kPathTraceSamplesPerPixel), 1.0f);

    // NRD surface guide (viewZ, world-space shading normal). Unlike the direct
    // reservoir's geometric key, this must include the normal map: NrdPack
    // uses it for IN_NORMAL_ROUGHNESS and specular reprojection/filtering.
    // Category (b): the reprojection key's viewZ is derived from the
    // V-buffer world position with the unjittered view matrix; projection
    // jitter must not enter this guide value.
    float  curZ = -mul(gPerFrame.viewMat, float4(sd.posW, 1.0f)).z;
    float3 curN = normalize(sd.normalW);
    gIndirectKeyOut[pixel] = float4(curZ, curN);

    // Side-channel consumed by NrdPack.cs.slang: the primary hit's GGX
    // alpha plus per-lobe primary-bounce hit distances (.y diffuse, .z
    // specular). The traced ray belongs to whichever lobe was sampled, so the
    // other channel remains 0 for this frame — NRD's "skipped lobe" encoding.
    // The host enables HitDistanceReconstructionMode for this probabilistic
    // lobe-selection convention. Kept OUT of gIndirectKeyOut so that texture
    // keeps the same (viewZ, normal.xyz) layout the direct chain's key uses.
    gIndirectKeyExtra[pixel] =
        float4(s.rough, diffuseHitDistance, specularHitDistance, 0.f);
}
