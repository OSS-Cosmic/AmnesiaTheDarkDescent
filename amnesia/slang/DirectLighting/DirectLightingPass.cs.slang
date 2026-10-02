// Direct-lighting pass — stage 1 of 3 (ReSTIR DI temporal reuse).
//
// Per pixel: reconstruct the surface from the V-buffer (rasterized primary
// visibility, POM-corrected barycentrics), build a light-selection reservoir by
// streaming RIS over the cell's lights (unshadowed-luminance target, NO shadow
// ray), then temporally reuse the previous
// frame's reservoir — reprojected through the velocity texture and
// rejected on a depth/normal disocclusion test, with the previous confidence M
// clamped so stale lighting can't dominate. Writes the merged reservoir
// (consumed by DirectSpatialReusePass) and the current surface key.
//
// The single shadow ray + resolve to radiance happens in the spatial pass; this
// pass traces nothing. Demodulated irradiance throughout — the composite applies
// albedo once, to direct + indirect together.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import RayCone;
import bindless;                       // gPackedHitInfo, lights, grid
import HitInfo;
import PerFrame.resource;
import Shading;                        // prepareDiffuseShading / buildDirectReservoir / directTargetPdf
import Random;
import Reservoir;

// Pass-private (set 2). Slot 0 is a gap; the remaining bindings keep their
// numbers so the host-side layout stays stable.
[vk::binding(1, 2)] Texture2D<float2>   gVelocity;             // screen-space motion (RG16F)
[vk::binding(2, 2)] Texture2D<float4>   gReservoirHistory;     // previous frame's final reservoir
[vk::binding(3, 2)] RWTexture2D<float4> gReservoirOut;         // output: temporally-merged reservoir
[vk::binding(4, 2)] Texture2D<float4>   gDirectKeyHistory;     // previous surface key (viewZ, normal.xyz)
[vk::binding(5, 2)] RWTexture2D<float4> gDirectKeyOut;         // output: current surface key
[vk::binding(6, 2)] RWTexture2D<float>  gLightConfidenceOut;   // output: NRD history confidence (LightChange.h)

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint outW, outH;
    gReservoirOut.GetDimensions(outW, outH);
    // Camera-jitter audit (a): this dispatch covers the full render extent,
    // matching gPackedHitInfo, the current key/output, and gVelocity. The
    // thread index is therefore the primary raster sample; no camera ray is
    // reconstructed and no half-resolution remap is needed.
    uint2 pixelPos = tid.xy;
    if (pixelPos.x >= outW || pixelPos.y >= outH)
        return;

    // The surface below is reconstructed from the full-resolution V-buffer hit
    // (only its barycentrics are POM-perturbed), not from a pixel-to-ray
    // conversion, so it already follows the primary raster sample and needs no
    // additional jitter term.
    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixelPos]);
    if (!hitInfo.isValid())
    {
        gReservoirOut[pixelPos] = float4(0.f, 0.f, 0.f, 0.f); // empty reservoir
        gDirectKeyOut[pixelPos] = float4(0.f, 0.f, 0.f, 0.f); // zero normal → rejected
        gLightConfidenceOut[pixelPos] = 1.f;                  // sky: NRD skips it anyway
        return;
    }

    TriangleHit th  = hitInfo.getTriangleHit();
    uint materialID = gSceneObjects[th.instanceID].materialID;
    float triLod;
    VertexData v    = getVertexDataAndLodConstant(th, triLod);
    // Use the geometric interpolated normal: cone-to-surface projection must
    // not be changed by a normal map applied later during shading.
    float3 toHit  = v.posW - gPerFrame.posW;
    float  hitT   = length(toHit);
    RayCone cone  = rayConePropagate(rayConePrimary(), hitT);
    float texLodBase = rayConeTexLodBase(cone, triLod, v.normalW,
                                         toHit / max(hitT, 1e-6f));
    float surfaceFootprintW = rayConeSurfaceFootprint(cone, v.normalW,
                                                      toHit / max(hitT, 1e-6f));
    UniformObject obj = gSceneObjects[th.instanceID];
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount,
                                              texLodBase, surfaceFootprintW,
                                              /*decodeRoughness*/ false, /*applyNormalMap*/ true);

    // Category (b): the reprojection key uses V-buffer world geometry and the
    // unjittered view matrix; projection jitter must not be added to viewZ.
    float  curZ = -mul(gPerFrame.viewMat, float4(sd.posW, 1.0f)).z;
    // Reprojection/disocclusion key uses the GEOMETRIC normal (not the bumped
    // sd.normalW) so high-frequency normal-map detail doesn't cause false
    // history rejections; lighting above still uses the bumped sd.normalW.
    float3 curN = normalize(v.normalW);

    // Reproject through the motion vector; reject (disocclusion) on depth/normal
    // mismatch. The key is point-sampled — it must not bilinear-blend across edges.
    float2 dims   = float2(outW, outH);
    // This pass runs on the full render extent, so the shared jitterUV is
    // already in its units — but the two frames were rasterized with DIFFERENT
    // sample offsets, so mapping a current texel onto a history texel needs
    // both. VBufferRaster writes velocity from the UNJITTERED clip positions,
    // so it carries no jitter of its own. At zero jitter both terms vanish and
    // this is exactly the original curUV - velocity mapping. (A pass on a
    // coarser grid would additionally need jitterUV * fullExtent / ownExtent.)
    float2 curUV  = (float2(pixelPos) + 0.5f) / dims;
    float2 velocity = gVelocity.Load(int3(pixelPos, 0)).xy;
    // The point at this pixel has unjittered UV curUV + current jitterUV; its
    // previous unjittered UV is that minus velocity, and the previous jittered
    // raster history texel is previous unjittered UV - previous jitterUV.
    float2 histUV = curUV - velocity + float2(gPerFrame.jitterX, gPerFrame.jitterY)
                  - float2(gPerFrame.prevJitterX, gPerFrame.prevJitterY);

    bool valid = !(any(histUV < 0.0f) || any(histUV > 1.0f));
    int2 ht = int2(0, 0);
    if (valid)
    {
        ht = clamp(int2(histUV * dims), int2(0), int2(dims) - 1);
        float4 k = gDirectKeyHistory.Load(int3(ht, 0));
        float  histZ = k.x;
        float3 histN = k.yzw;
        valid = abs(curZ - histZ) <= kReprojZRelTol * max(curZ, 1.0e-3f)
             && dot(curN, normalize(histN)) >= kReprojNormalCos;
    }

    // How much of this surface's lighting changed since last frame. Feeds the
    // reservoir history cap below and, as NRD history confidence, both
    // denoisers: a flickering light changes radiance without disoccluding
    // anything, so NRD would otherwise keep accumulating the old lighting.
    // Directional lights live outside the grid, so their change is global;
    // take the larger of the two rather than weighting them against each other.
    float lightChange = max(getCellLights(sd.posW).change, directionalLightChange());
    gLightConfidenceOut[pixelPos] = lightChangeHistoryConfidence(
        lightChange, kLightChangeConfidenceFullReset, kLightChangeConfidenceMin);

    RNG rng;
    rng.init(pixelPos, gPerFrame.totalFrames);

    // Build this frame's reservoir (RIS over the cell lights), then fold it into
    // the combination as the leading candidate.
    Reservoir cur = buildDirectReservoir(sd, rng);

    Reservoir R; R.init();
    mergeDirectReservoir(R, cur, cur.pHatY, rng);

    if (valid)
    {
        // Reproject the previous reservoir (point-sampled — packed light index
        // can't interpolate). Clamp its confidence so it can't outweigh the
        // fresh sample by more than kReservoirMClamp×, tightened toward 1× where
        // the cell's lighting just changed (a flickering light switching on
        // otherwise has to outvote up to 30 frames of history).
        Reservoir prev = unpackReservoir(gReservoirHistory.Load(int3(ht, 0)));
        float mClamp = lightChangeHistoryClamp(lightChange, kReservoirMClamp,
                                               kLightChangeFullReset);
        prev.M = min(prev.M, mClamp * max(cur.M, 1.0f));
        float pHatPrev = directTargetPdf(sd, prev.y);
        mergeDirectReservoir(R, prev, pHatPrev, rng);
    }

    R.finalizeCombine();

    gReservoirOut[pixelPos] = packReservoir(R);
    gDirectKeyOut[pixelPos] = float4(curZ, curN);
}
