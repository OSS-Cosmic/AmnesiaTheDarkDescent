// Final composite pass.
//
// Evaluates hard gobo spotlight diffuse lighting after denoising. Reads the
// V-buffer, independently filtered ReSTIR direct irradiance, and REBLUR's two
// indirect outputs. Only the REBLUR outputs are YCoCg-packed and require
// REBLUR_BackEnd_UnpackRadianceAndNormHitDist; direct lighting is linear RGB.
// The diffuse terms are albedo-demodulated, while indirect GGX is already
// outgoing radiance. Hard gobo lights keep their deterministic evaluation here.
// The pass either:
//   * misses → no environment-map eval today, so emits black
//   * hits   → recovers world-space vertex data + diffuse albedo,
//              composites decals onto that albedo, multiplies the summed
//              demodulated diffuse terms by it (they store incoming
//              radiance with the cosθ factor baked in, so the composite
//              weight is just albedo / π · π = albedo), adds the specular
//              channel on top of that product, adds primary-hit emission
//              and applies fog
//
// Runs as a compute pass: one thread per pixel writes the composited result
// into the post-effect pogo buffer bound as a storage image (gOutput). The
// renderer transitions the pogo attach to GENERAL around the dispatch and back
// to COLOR_ATTACHMENT_OPTIMAL afterwards, so the downstream pogo toggle and the
// translucent/particle raster passes are unaffected.

#include "../HostDefinitions.h"
#include "../Constants.h"
#include "../RtDiffuseColor.h"
// Vendored NRD back end, included the same way NrdPack.cs.slang pulls in the
// front end, so the YCoCg encoding stays authoritative on both sides.
#include "../../../HPL2/extern/NRD/Shaders/NRD.hlsli"

import SceneTypes;
import Scene;
import RayCone;
import bindless;
import HitInfo;
import PerFrame.resource;
import Fog;
import Shading;                     // DiffuseShading / prepareDiffuseShading (re-exported from DiffuseShading.slang)
import Material;                    // loadMaterialHeader (shadow-flag overlay's translucent test)
import Grid;                         // gridCellPos / gridFlattenCellIndex / gridCellValid (decal grid lookup)
import Utils.Color.ColorHelpers;     // linearToSRGB / sRGBToLinear (legacy display-space decal blend)
import BlendModes;                   // blendDisplaySpaceExact (raw eMaterialBlendMode numbering)

struct MainCompositePushConstants
{
    uint overlayMode;
    uint hasTlas;
    float diffuseColorExposure; // active tone-map exposure; 0 disables correction
};

[vk::push_constant]
ConstantBuffer<MainCompositePushConstants> gPushConstants HPL_PUSH_CONSTANT_REGISTER;

// REBLUR's denoised indirect diffuse: albedo-demodulated, YCoCg-packed
// with normalized hit distance in .w.
[vk::binding(0, 2)]
Texture2D<float4> gIndirectLighting;

// RELAX-filtered ReSTIR DI, linear demodulated irradiance.
[vk::binding(3, 2)]
Texture2D<float4> gDirectLighting;

// Output: the post-effect pogo HDR target, bound as a storage image. The
// renderer puts it in GENERAL for the dispatch and back to COLOR afterwards.
[vk::binding(2, 2)]
RWTexture2D<float4> gOutput;

// Per-world decal data, baked by cWorld::Compile and bound on set 2 (the
// per-pass set) alongside this pass's resources above. Declared here rather than
// in bindless.slang because MainCompositePass is the only consumer — putting them
// in the shared module collided with every other pass's set-2 scratch. Bindings
// start at kBindingWorldDecals (4) to clear the composite's slots 0..3 above
// (slot 1 is a gap; slot 3 contains independently filtered direct lighting).
[vk::binding(kBindingWorldDecals, kWorldDecalSet)]
StructuredBuffer<GpuDecal> gDecals;
// Flat per-object decal-index pool. UniformObject.decalList packs (offset<<8)|count;
// compositeDecals reads gObjectDecalIndices[offset + k] → a stable gDecals[] index.
[vk::binding(kBindingWorldObjectDecalIndices, kWorldDecalSet)]
StructuredBuffer<uint> gObjectDecalIndices;

// Per-viewport decal accumulators: Type="Decal" meshes (Mul/MulX2/Add, no alpha)
// rasterized this frame. decalMul holds the multiplicative factor (white where
// no decal), decalAdd the additive term (black where no decal). Applied to the
// base albedo before lighting: albedo = albedo*decalMul + decalAdd, so the mesh
// decals are lit + fogged by the composite (the V-buffer renderer has no albedo
// G-buffer to write them into). Set-2 slots 6,7 after the world decal buffers.
[vk::binding(6, 2)]
Texture2D<float4> gDecalMul;
[vk::binding(7, 2)]
Texture2D<float4> gDecalAdd;

// REBLUR's OUT_SPEC_RADIANCE_HITDIST: the denoised specular lobe, also
// YCoCg-packed. NOT demodulated — there is no single
// albedo to divide out (F0 and roughness vary per texel), so this channel is
// already outgoing radiance and is added AFTER the albedo multiply, not folded
// into it. Set-2 slot 8, the first free one after the decal accumulators.
[vk::binding(8, 2)]
Texture2D<float4> gIndirectSpecular;

// --- Decal projection (folded in from the old albedo_resolve pass) ----------
// Clustered OOB decals are composited directly onto the base albedo here, before
// lighting, so they get lit like the surface — no separate albedo buffer/pass.

// The per-texel decal blend is blendDisplaySpaceExact (BlendModes.slang):
// software blend with dst in hand, replicating the legacy hardware blend
// factors bit-for-bit in display space. Takes the RAW eMaterialBlendMode
// numbering GpuDecal.blendMode stores (None=0 … PremulAlpha=5).

// Reject surfaces that are near-perpendicular to the decal's projection axis
// (the bleed artifact: a decal box catching side walls / edges). Two-sided and
// derived from the box's own axes, so it's independent of the projection-axis
// sign/convention. |cos| below this (~angle > ~75deg from the axis) is culled.
static const float kDecalFacingCos = 0.25f;

// Project one decal (by index): box-clip, facing-cull, atlas-UV, sample, blend.
// The per-object decal list (compositeDecals) is the editor's static association,
// so a listed decal is one this object was clipped to.
float3 projectDecal(uint di, float3 albedo, float3 posW, float3 normalW,
                    float surfaceFootprintW)
{
    GpuDecal d = gDecals[di];

    float3 lp = mul(d.invModelMat, float4(posW, 1.0f)).xyz;   // world -> box-local
    if (any(abs(lp) > 0.5f))
        return albedo;

    // Facing cull in box-local space: transform the surface normal by the same
    // invModelMat used for the position clip (sign-consistent with the box), and
    // require a real component along the projection axis (local Y). Reject only
    // near-perpendicular surfaces — keeps both facings, so it can't hide a
    // correctly-placed decal from an axis-sign mismatch.
    float3 nLocal = mul((float3x3)d.invModelMat, normalW);
    float  nLen   = length(nLocal);
    if (nLen < 1.0e-6f || abs(nLocal.y) <= kDecalFacingCos * nLen)
        return albedo;

    // Project along local Y; remap into the selected atlas cell, matching the
    // editor's baked UVs (DecalCreator.cpp:346-359). subDivIndex is the 0-based
    // cell used directly; clamp to [0,1] as the editor does so stale/degenerate
    // cell values (e.g. CurrentSubDiv=3 with a 1x1 grid) sample the edge instead
    // of off-texture.
    //   u = clamp((cellCol + (localX+0.5)) / subDivX, 0, 1)
    //   v = 1 - clamp((cellRow + (localZ+0.5)) / subDivY, 0, 1)   (V flipped)
    uint  sx = max(d.subDivX, 1u);
    uint  sy = max(d.subDivY, 1u);
    float fU = float(d.subDivIndex % sx);
    float fV = float(d.subDivIndex / sx);
    // This is category (b): decal-atlas UV from V-buffer-derived world position
    // in decal-local space, not screen-space UV for a jittered raster texture;
    // the projected cone LOD below uses the same affine decal coordinates.
    float2 uv = float2(saturate((fU + (lp.x + 0.5f)) / float(sx)),
                       1.0f - saturate((fV + (lp.z + 0.5f)) / float(sy)));

    // Decal diffuse texture is baked directly into the decal (a pinned, stable
    // gTextures2D[] slot); sample its projected footprint through the cone.
    uint dtex = d.diffuseTexIndex;
    if (dtex == kInvalidTextureIndex)
        return albedo;

    float lodBase = decalTexLodBase(d.invModelMat, d.subDivX, d.subDivY,
                                    surfaceFootprintW);
    float4 s = sampleBindless2DCone(dtex, uv, lodBase);
    // Legacy display-space blend, computed EXACTLY (software blend with dst in
    // hand — unlike the raster passes no powered-weight approximation is
    // needed): re-encode the sRGB-decoded texel and the linear albedo back to
    // the authored display space, blend like the legacy gbuffer did, decode
    // the result. d.color is the raw authored tint. This is "the albedo the
    // legacy gbuffer would have held", in linear — fixes e.g. MULX2's 0.5-gray
    // neutral and the residual pink-blood tint of the old linear-blend
    // compromise.
    float4 srcD = float4(linearToSRGB(s.rgb), s.a) * d.color;
    if (srcD.a < kDecalAlphaReject)                           // legacy ALPHA_REJECT
        return albedo;

    return sRGBToLinear(blendDisplaySpaceExact(d.blendMode, linearToSRGB(albedo), srcD));
}

// Composite the decals precomputed for this object (the static association the
// editor clipped it to). `decalList` packs (offset << 8) | count into the shared
// gObjectDecalIndices pool. `normalW` is the hit surface normal (facing cull).
float3 compositeDecals(float3 albedo, float3 posW, float3 normalW,
                       float surfaceFootprintW, uint decalList)
{
    uint count  = decalList & 0xFFu;
    uint offset = decalList >> 8;
    for (uint k = 0u; k < count; ++k)
        albedo = projectDecal(gObjectDecalIndices[offset + k], albedo, posW,
                              normalW, surfaceFootprintW);
    return albedo;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    const uint overlayMode = gPushConstants.overlayMode;
    
    uint2 pixelPos = tid.xy;
    uint  outW, outH;
    gOutput.GetDimensions(outW, outH);
    // Camera-jitter audit (a): composite runs over the full output extent,
    // matching the packed V-buffer and every denoised input. `pixelPos` is the
    // primary raster sample in that common grid; no camera ray is reconstructed
    // and no half-resolution or extra jitter conversion is appropriate.
    if (pixelPos.x >= outW || pixelPos.y >= outH)
        return;   // edge threads of the rounded-up dispatch

    // The hit and v.posW below come from the full-resolution V-buffer (only the
    // barycentrics are POM-perturbed), not from reconstructing a camera ray from
    // pixelPos. They already represent the primary raster sample and need no
    // second jitter term.
    // NOTE: HitInfo.isValid() passes as long as the fields aren't all zero,
    // so the instance-0 / prim-0 / barys-(0,0,0) corner case reads as
    // invalid. Same edge case that already exists for any consumer of the
    // raster V-buffer; not addressed here.
    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixelPos]);

    if (!hitInfo.isValid())
    {
        // Miss pixel — no env-map binding today, emit black.
        // When env map gets wired into the bindless set, this is
        // where the lookup goes:
        //   `float3 envColor = gEnvMap.SampleLevel(..., ray, 0).rgb;`
        gOutput[pixelPos] = float4(0.f, 0.f, 0.f, 1.f);
        return;
    }

    TriangleHit triangleHit = hitInfo.getTriangleHit();
    UniformObject obj = gSceneObjects[triangleHit.instanceID];

    if (overlayMode == kOverlayModeShadowFlag)
    {
        // Report what actually blocks shadow rays, not just the authored bit.
        // Mirrors the TLAS mask rule in World.cpp: an instance joins the shadow
        // mask when it is not translucent AND (allLightsCastShadows || authored).
        // Yellow calls out the gap — opaque geometry the artist did NOT flag,
        // occluding anyway because the all-lights policy is on; flipping the
        // authored bit there changes nothing until the policy is off.
        const bool authored    = (obj.renderFlags & kObjectFlagShadowCaster) != 0u;
        const bool translucent = loadMaterialHeader(obj.materialID).type == MATERIAL_TYPE_TRANSLUCENT;
        const bool effective   = !translucent && (gPerFrame.allLightsCastShadows != 0u || authored);

        float3 overlayColor = float3(1.f, 0.f, 0.f);          // not an occluder
        if (effective)
            overlayColor = authored ? float3(0.f, 1.f, 0.f)   // authored caster
                                    : float3(1.f, 1.f, 0.f);  // caster via policy only

        gOutput[pixelPos] = float4(overlayColor, 1.f);
        return;
    }

    uint materialID = obj.materialID;

    // No refraction re-trace happens anywhere: the primary hit read above is the
    // plain rasterized front surface, so at water/glass pixels this shades the
    // translucent surface's own front face. The water surface (tint + reflection)
    // and the glass surface are then drawn on top in their raster passes.
    // Refracted-background support is currently MISSING: the legacy bend-behind
    // was removed and the RT V-buffer meant to replace it was retired without
    // ever running. Known gap, not working behaviour.

    float triLod;
    VertexData v = getVertexDataAndLodConstant(triangleHit, triLod);
    // The primary cone derives its spread from the render extent, so a
    // reduced temporal-upscaling extent widens the footprint automatically.
    float3 toHit  = v.posW - gPerFrame.posW;
    float  hitT   = length(toHit);
    RayCone cone  = rayConePropagate(rayConePrimary(), hitT);
    // Use the geometric interpolated normal: cone-to-surface projection must
    // not be changed by a normal map applied later during shading.
    float texLodBase = rayConeTexLodBase(cone, triLod, v.normalW,
                                         toHit / max(hitT, 1e-6f));
    float surfaceFootprintW = rayConeSurfaceFootprint(cone, v.normalW,
                                                      toHit / max(hitT, 1e-6f));
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount,
                                              texLodBase, surfaceFootprintW,
                                              /*decodeRoughness=*/false,
                                              /*applyNormalMap=*/true);

    // Composite clustered OOB decals straight onto the base albedo, before
    // lighting, so they get lit like the surface (replaces the old albedo_resolve
    // buffer + decal raster pass).
    sd.albedo = compositeDecals(sd.albedo, v.posW, normalize(v.normalW),
                                surfaceFootprintW, obj.decalList);

    // Rasterized Type="Decal" mesh decals (Mul/MulX2/Add): apply the multiply
    // factor then the additive term (same place + reason as the clustered decals
    // above). Done in DISPLAY (gamma) space — matching the legacy gamma diffuse
    // G-buffer — so MulX2's 0.5-gray neutral stays neutral (2×0.5=1) instead of
    // darkening; saturate matches the legacy UNORM [0,1] clamp. White/black
    // identities leave non-decal pixels unchanged. Lit + fogged below.
    // Category (a): these decal buffers are full-resolution raster outputs, so
    // direct `pixelPos` indexing keeps their primary sample aligned without a
    // UV conversion or an extra jitter term.
    float3 albedoG = linearToSRGB(sd.albedo);
    albedoG = saturate(albedoG * gDecalMul[pixelPos].rgb + gDecalAdd[pixelPos].rgb);
    sd.albedo = sRGBToLinear(albedoG);

    bool wantDirect = kDefaultRenderDirectLighting != 0u;
    float3 direct = wantDirect
        ? RELAX_BackEnd_UnpackRadiance(gDirectLighting[pixelPos]).rgb : float3(0.f);
    bool wantIndirect = kDefaultRenderIndirectLighting != 0u;

    // Both REBLUR outputs are YCoCg-packed; unpack before any arithmetic. The
    // .w each carries is the normalized hit distance and is not needed here.
    float3 diffuse = wantIndirect
        ? REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gIndirectLighting[pixelPos]).xyz
        : float3(0.f);
    float3 indirectSpecular = wantIndirect
        ? REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gIndirectSpecular[pixelPos]).xyz
        : float3(0.f);

    // Direct and indirect diffuse are added after their independent filters.
    // Apply albedo exactly once, after compositeDecals, to both terms.
    //
    // `indirectSpecular` is NOT demodulated: the path tracer already evaluated
    // its GGX lobe (F0, roughness, NdotV all vary per texel, so there is no one
    // albedo to pull out), making it outgoing radiance already. It is added
    // OUTSIDE the albedo factor. Consequence: projected decals modulate the
    // diffuse channel, but NOT the specular term.
    // Hard gobo lights are excluded from primary ReSTIR DI. Evaluate each
    // deterministically here so NRD cannot blur their projected pattern.
    float3 goboDiffuse = float3(0.f);
    if (wantDirect && gPushConstants.hasTlas != 0u)
        goboDiffuse = evaluateDirectGoboLighting(sd);
    // Correct only the visible diffuse material response. The lighting buffers
    // and bounce reflectance retain their physical linear values; specular,
    // emission and fog are composed independently below.
    float3 finalGather = rtDiffuseColor(sd.albedo, direct + diffuse + goboDiffuse,
                                       gPushConstants.diffuseColorExposure)
                       + indirectSpecular;

    // Emissive (illumination-map) surfaces glow regardless of the indirect
    // toggle — added on top so glowing textures aren't dark. Primary-hit
    // emission stays HERE rather than in the path tracer on purpose: the
    // path tracer's output is denoised downstream by REBLUR, which would blur
    // these sharp emissive texels across the surface.
    finalGather += sd.emission;

    // Per-pixel fog: world fog + each fog-area bounding box from gFogAreas.
    // Replaces the legacy per-object visibility table (which only existed
    // for translucents) — applies uniformly to the opaque composite here.
    // Fog receives V-buffer-derived world position, so it does not reconstruct
    // a camera ray from pixelPos and requires no additional projection jitter.
    finalGather = applyFog(finalGather, v.posW, gPerFrame.posW);

    gOutput[pixelPos] = float4(finalGather, 1.f);
}
