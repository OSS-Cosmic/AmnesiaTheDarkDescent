#pragma once

// Free functions over the engine's bindless set (set 0) + per-frame UBO (set 1):
// triangle-hit vertex fetch, the world-space light-grid walkers, and the pinhole
// camera ray. There is no Scene object — callers read gPerFrame / gSceneObjects
// directly for trivial values (camera position, object transform) and call these
// functions for the composite operations.
//
// Design choices:
//   * Lights use a single dispatcher: getLight returns the ILight interface
//     for a flat index, and callers invoke .sample() on it. There are no
//     typed getPointLight() / etc. accessors — iterate 0..getLightCount().
//   * Materials live in Material.slang: read loadMaterialHeader(id).type to
//     branch, then loadDiffuseMaterial / loadTranslucentMaterial /
//     loadWaterMaterial for the typed payload (field-wise BDA reads).
//   * Vertex fetch delegates to BindlessTriangle.slang; barycentric
//     interpolation + object-to-world transform happen here.
//
// Known stubs / non-goals (see implementation plan):
//   * RT acceleration structure — exposed as the bindless gRtAccel global.
//   * Env map binding not declared yet.
//   * Per-light gobo + LUT attenuation: PointLight.sample / SpotLight.sample
//     in ILight.slang do analytic fallback only today; the
//     unified dispatcher inherits that.

#include "HostDefinitions.h"
#include "Constants.h"          // kLightGrid* / kLightsPerCellMax (lightLevelAt)

import SceneTypes;
import bindless;
__exported import ILight;
import PerFrame.resource;
import BindlessTriangle;
import RayCone;
import Brdf;                    // evalDiffuseDemodulated (standard diffuse lobe)
import Random;
import Grid;                    // gridCellPos / gridFlattenCellIndex / gridCellValid (lightLevelAt)
import Utils.Color.ColorHelpers; // linearToSRGB (lightLevelAt's authored-space max channel)

// Falcor-compatible Ray struct (used by computeRayPinhole). Local to this
// module to avoid pulling in a Falcor `Scene` dependency.
public struct Ray
{
    public float3 origin;
    public float  tMin;
    public float3 dir;
    public float  tMax;
};

// Packed light id: top 2 bits = type, low 30 bits = the light's STABLE per-type
// GPU slot (host-side IndexPool, see cWorld::PrepareFrame). Self-describing — the
// decode is independent of the per-frame light counts, so a light's id is the
// same value every frame as long as it lives. This is the identity ReSTIR DI
// reservoirs persist in Reservoir.y; a count-relative flat index would alias a
// different light whenever any type's count changed.
public static const uint kLightTypePoint = 0u;
public static const uint kLightTypeSpot  = 1u;
public static const uint kLightTypeArea  = 2u;
static const uint kLightSlotBits   = 30u;
static const uint kLightSlotMask   = (1u << kLightSlotBits) - 1u;

public uint packLightId(uint type, uint slot) { return (type << kLightSlotBits) | (slot & kLightSlotMask); }
public uint lightIdType(uint id)              { return id >> kLightSlotBits; }
public uint lightIdSlot(uint id)              { return id & kLightSlotMask; }

// Packed-id → ILight resolver. The interface return goes through Slang's
// existential boxing; callers sample lights via
// getLight(id).sample(...). No slot bounds check — an out-of-range slot is a
// caller bug, not a guarded case. A destroyed-but-not-reused slot is a zeroed
// hole (intensity/color/radius 0) and decodes to a no-op light.
public ILight getLight(uint ID)
{
    uint slot = lightIdSlot(ID);
    switch (lightIdType(ID))
    {
        case kLightTypePoint: return gPointLights[slot];
        case kLightTypeSpot:  return gSpotLights[slot];
        default:              return gAreaLights[slot];
    }
}

// Non-normalized primary ray direction for a pinhole camera, matching
// Falcor's Scene::Camera::computeNonNormalizedRayDirPinhole. Coordinates
// are top-left (y down): pixelUV is the pixel center in FULL-render-extent
// normalized units, and jitterUV is the normalized sample offset. Apply
// jitter exactly once so V-buffer POM rays pass through the rasterized sample.
// `+ndc.x*cameraU + ndc.y*cameraV + cameraW` walks out from the optical axis
// along the pre-scaled basis vectors filled host-side in cHybridRenderer::Draw().
float3 computeNonNormalizedRayDirPinhole(uint2 pixel, uint2 frameDim, bool applyJitter)
{
    float2 p = (float2(pixel) + float2(0.5f, 0.5f)) / float2(frameDim);
    if (applyJitter) p += float2(gPerFrame.jitterX, gPerFrame.jitterY);
    float2 ndc = float2(2.0f, -2.0f) * p + float2(-1.0f, 1.0f);
    return ndc.x * gPerFrame.cameraU + ndc.y * gPerFrame.cameraV + gPerFrame.cameraW;
}

// World-space pinhole ray, Falcor parity. The jitterUV added when
// applyJitter=true is in FULL-render-extent normalized units (top-left origin,
// y down), so this is correct only when frameDim is the full render extent;
// then the ray samples exactly where the V-buffer raster sampled. A pass on a
// coarser grid must pass applyJitter=false or add that grid's own offset at the
// call site; this function does not rescale jitterUV, and the shared field's
// units must not change. Passes elsewhere label which of the three they are:
// (a) matches the primary raster sample — full extent, applyJitter=true;
// (b) deliberately unjittered — applyJitter=false, no jitter term;
// (c) its own grid's offset, converted at the call site. tMin/tMax scale by 1/cos(theta) so the engine's
// view-space near/far planes map exactly onto ray length — same trick Falcor
// uses to keep ray bounds tight at the frustum edge.
// (Trivial camera values — eye position gPerFrame.posW, view-proj
// mul(gPerFrame.projMat, gPerFrame.viewMat) — are read inline at call sites.)
public Ray cameraComputeRayPinhole(uint2 pixel, uint2 frameDim, bool applyJitter)
{
    Ray r;
    r.origin = gPerFrame.posW;
    r.dir    = normalize(computeNonNormalizedRayDirPinhole(pixel, frameDim, applyJitter));

    float invCos = 1.0f / dot(normalize(gPerFrame.cameraW), r.dir);
    r.tMin = gPerFrame.zNear * invCos;
    r.tMax = (gPerFrame.zFar > 0.0f ? gPerFrame.zFar : 1e6f) * invCos;
    return r;
}

// One world-space light-grid cell's light list, resolved once by
// getCellLights() so the four light-grid walkers (composite eval, reservoir
// build, path-tracer NEE, lightLevelAt) share the cell-lookup boilerplate instead of
// each re-deriving cell → validate → flatten → count.
public struct CellLights
{
    public uint flat;   // flattened cell index into gLightGridCount / gLightGridList
    public uint count;  // lights in the cell, clamped to kLightsPerCellMax
    public bool valid;  // false ⇒ no lights / out-of-grid; count is then 0
};

// --- Lights ---------------------------------------------------------------
// gPerFrame.{point,spot,area}LightCount are now per-type SLOT CAPACITIES
// (host high-water of stable slots), not live light counts — sparse slots
// may include zeroed holes. Used only as a ">0 any lights?" guard here, so
// capacity semantics are fine; per-light enumeration goes through the grid.
public uint getLightCount()
{
    return gPerFrame.pointLightCount
         + gPerFrame.spotLightCount
         + gPerFrame.areaLightCount;
}

// Resolve the world-space light-grid cell at `posW` to its light list once.
// Shared by every light-grid walker so the cell-lookup boilerplate lives in
// one place; callers loop 0..count and fetch indices via cellLightAt.
public CellLights getCellLights(float3 posW)
{
    CellLights c;
    // gPerFrame.lightGridOriginW, not posW: the camera snapped to a whole cell.
    // LightGridBuildPass bins against this same origin, so using the raw camera
    // position here would look up a cell the builder filled for a different grid.
    int3 cell = gridCellPos(posW, gPerFrame.lightGridOriginW, kLightGridUnit);
    c.valid = (getLightCount() > 0u) && gridCellValid(cell, kLightGridDim);
    if (!c.valid)
    {
        c.flat  = 0u;
        c.count = 0u;
        return c;
    }
    c.flat  = gridFlattenCellIndex(cell, kLightGridDim);
    c.count = min(gLightGridCount[c.flat], kLightsPerCellMax);
    return c;
}

// The global (unified point+spot) light index of the k-th light in a cell.
public uint cellLightAt(CellLights c, uint k)
{
    return gLightGridList[c.flat * kLightsPerCellMax + k];
}

// The per-light diffuse shade helpers — shadeAnalyticLight (hard),
// shadeAnalyticLightSoft (jittered disk), unshadowedAnalyticLight (no ray) —
// are lighting-evaluation utilities, not scene-data accessors, so they live
// as free functions in DiffuseShading.slang alongside the eval / reservoir
// helpers that use them. They compose the scene data primitives
// (getLight().sample / sampleSourcePoint, gPerFrame.posW, traceShadowRay).

// Scalar "surrounding light" heuristic for AffectedByLightLevel
// translucents — base-game afLightLevel semantics (the legacy CPU loop in
// MaterialType_BasicTranslucent::SetupObjectSpecificData): sum
// maxChannel(authored color) · saturate(1 − d/authoredRadius) over the
// lights binned to the grid cell at posW, saturated at 1. Normal-free and
// shadow-free: billboards have no normal and the legacy heuristic never
// had occlusion (or a spot-cone term — spots fall off radially like
// points, matching the original).
//   * `intensity` IS the authored engine radius (uploaded verbatim for
//     both light types — Falcor LightData semantics, no scale knobs), so
//     it is used directly. The struct's `radius` field is the grid-bin
//     reach, a different quantity.
//   * GPU light colors are linear (sRGB-decoded at upload) while the
//     legacy heuristic read the authored sRGB values, so the max channel
//     is re-encoded to match the CPU translucent-mesh path's dimming
//     curve.
//   * Out-of-grid points return 1.0 (don't darken) — distant geometry
//     beyond the camera-centered grid shouldn't fade to black.
public float lightLevelAt(float3 posW)
{
    // No lights is darkness, not the out-of-grid full-bright fallback.
    // getCellLights also marks an empty light set invalid, so distinguish it
    // before applying the distant-geometry policy below.
    if (getLightCount() == 0u)
        return 0.0f;

    // Out-of-grid points return 1.0 (don't darken) — note this differs from
    // the shaded walkers, which treat an invalid cell as no contribution.
    CellLights cl = getCellLights(posW);
    if (!cl.valid)
        return 1.0f;

    float sum = 0.0f;
    for (uint k = 0u; k < cl.count; ++k)
    {
        uint li   = cellLightAt(cl, k);   // packed type|slot
        uint slot = lightIdSlot(li);
        float3 lpos;
        float3 lcolor;
        float  lintensity;
        switch (lightIdType(li))
        {
        case kLightTypePoint:
            lpos       = gPointLights[slot].position;
            lcolor     = gPointLights[slot].color;
            lintensity = gPointLights[slot].intensity;
            break;
        case kLightTypeSpot:
            lpos       = gSpotLights[slot].position;
            lcolor     = gSpotLights[slot].color;
            lintensity = gSpotLights[slot].intensity;
            break;
        default:
            lpos       = gAreaLights[slot].position;
            lcolor     = gAreaLights[slot].color;
            lintensity = gAreaLights[slot].intensity;
            break;
        }

        float authoredRadius = lintensity;
        if (authoredRadius <= 0.0f) continue;

        float maxC = linearToSRGB(max(lcolor.x, max(lcolor.y, lcolor.z)));
        float dist = length(posW - lpos);
        sum += maxC * saturate(1.0f - dist / authoredRadius);
        if (sum >= 1.0f) return 1.0f;
    }
    return sum;
}

// --- Mesh / hit data ------------------------------------------------------
// Reconstruct world-space VertexData for a triangle hit. Instance
// transform comes from gSceneObjects[hit.instanceID]; vertex streams
// come from the per-instance BDA handle arrays via BindlessTriangle.
public VertexData getVertexData(TriangleHit hit)
{
    BindlessTriangleVtx vtx;
    return getVertexDataAndTri(hit, vtx);
}

// Same as getVertexData, but also returns the raw per-vertex triangle data
// (positions / normals / tangents / UVs). Parallax mapping needs the
// per-vertex UVs to invert a UV offset back into a barycentric delta — see
// VBuffer/VBufferPomBary.cs.slang. Fetches the bindless triangle once so
// callers that want both don't pay the BDA handle loads twice.
public VertexData getVertexDataAndTri(TriangleHit hit, out BindlessTriangleVtx vtx)
{
    fetchBindlessTriangle(hit.instanceID, hit.primitiveIndex, vtx);

    float b1 = hit.barycentrics.x;
    float b2 = hit.barycentrics.y;
    float b0 = 1.0f - b1 - b2;

    float3 posL    = vtx.posL[0]    * b0 + vtx.posL[1]    * b1 + vtx.posL[2]    * b2;
    float3 normalL = vtx.normalL[0] * b0 + vtx.normalL[1] * b1 + vtx.normalL[2] * b2;
    float3 tanL    = vtx.tangentL[0].xyz * b0
                   + vtx.tangentL[1].xyz * b1
                   + vtx.tangentL[2].xyz * b2;
    float2 uv      = vtx.uv[0]      * b0 + vtx.uv[1]      * b1 + vtx.uv[2]      * b2;

    UniformObject obj = gSceneObjects[hit.instanceID];
    float4x4 modelMat = obj.modelMat;
    // Upper-3x3 carries rotation + uniform scale; for normals we'd
    // ideally want the inverse-transpose, but the engine ships
    // uniform-scaled meshes, so the upper-3x3 of modelMat is
    // equivalent up to a normalize().
    float3x3 nrmMat = float3x3(modelMat[0].xyz,
                               modelMat[1].xyz,
                               modelMat[2].xyz);

    VertexData v;
    v.posW     = mul(modelMat, float4(posL, 1.0f)).xyz;
    v.normalW  = normalize(mul(nrmMat, normalL));
    v.tangentW = normalize(mul(nrmMat, tanL));
    v.texC     = uv;
    return v;
}

// The triangle's texture-footprint constant for an already-fetched triangle.
// Split out so a caller that already holds the BindlessTriangleVtx (the POM
// pass) doesn't pay a second bindless fetch. The local-space positions are
// transformed to world space before the ratio is taken, so instance scale is
// included — a scaled-up instance covers more world area per UV area and must
// select a coarser mip.
public float triangleTexLodConstantForInstance(BindlessTriangleVtx vtx,
                                                uint instanceID)
{
    float4x4 modelMat = gSceneObjects[instanceID].modelMat;
    float3 p0W = mul(modelMat, float4(vtx.posL[0], 1.0f)).xyz;
    float3 p1W = mul(modelMat, float4(vtx.posL[1], 1.0f)).xyz;
    float3 p2W = mul(modelMat, float4(vtx.posL[2], 1.0f)).xyz;
    return triangleTexLodConstant(vtx.uv[0], vtx.uv[1], vtx.uv[2],
                                  p0W, p1W, p2W);
}

// getVertexData plus the triangle's texture-footprint constant, for the
// ray-traced shading paths that select a mip from a ray cone instead of
// sampling the top mip unconditionally. One bindless triangle fetch serves
// both. See RayCone.slang for what the constant feeds.
public VertexData getVertexDataAndLodConstant(TriangleHit hit, out float triLodConstant)
{
    BindlessTriangleVtx vtx;
    VertexData v = getVertexDataAndTri(hit, vtx);

    triLodConstant = triangleTexLodConstantForInstance(vtx, hit.instanceID);
    return v;
}
