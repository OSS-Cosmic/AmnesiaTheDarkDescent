#pragma once

// Raster-safe typed blob loads and the shared alpha/dissolve rejection gate
// over the engine's flat material table (gMaterials, set 0). Material data
// structs stay in SceneTypes.slang, shared with the C++ host. Import this module
// for raster material evaluation; ray-query helpers remain in Material.slang.

#include "HostDefinitions.h"
#include "Constants.h"

import SceneTypes;       // MaterialDataBlob / MaterialHeader / concrete structs, VertexData
import bindless;         // gMaterials, gMaterialSampler, gDissolveMap, sampleBindless2DLevel

// Typed material reads. gMaterials[materialID] is a fixed-size MaterialDataBlob;
// reinterpret<T> turns it into the concrete struct (the host memcpys the struct
// into the blob, scalar layout keeps offsets identical). reinterpret handles the
// inner tex[8] array correctly, unlike a BDA struct-pointer cast (slangc 2026+
// zeros those — see BindlessTriangle.slang). `type` and `materialConfig` sit at
// offset 0/1 for every variant, so loadMaterialHeader resolves the discriminator
// and shared flags without committing to a concrete type.
public MaterialHeader loadMaterialHeader(uint materialID)
{
    return reinterpret<MaterialHeader, MaterialDataBlob>(gMaterials[materialID]);
}
public DiffuseMaterial loadDiffuseMaterial(uint materialID)
{
    return reinterpret<DiffuseMaterial, MaterialDataBlob>(gMaterials[materialID]);
}
public TranslucentMaterial loadTranslucentMaterial(uint materialID)
{
    return reinterpret<TranslucentMaterial, MaterialDataBlob>(gMaterials[materialID]);
}
public WaterMaterial loadWaterMaterial(uint materialID)
{
    return reinterpret<WaterMaterial, MaterialDataBlob>(gMaterials[materialID]);
}

// Alpha-test for the diffuse opaque path: read tex[2] (alpha slot —
// see SceneTypes.slang DiffuseMaterial comment), sample at kAlphaTestLod
// with gMaterialSampler, reject when a < 0.5 (matches the GLSL
// ALPHA_REJECT default). Dedicated grayscale alpha maps (e.g.
// chandelier_simple_alpha.tga, loaded as R8_UNORM) carry the mask in
// .r — kMaterialFlagIsAlphaSingleChannel picks the channel.
// Alpha test + CoverageAmount dissolve — ONE gate for every ray/raster
// context (V-buffer raster FS, path-trace scatter anyhit, shadow /
// reflection RayQuery loops), so a fading object's primary visibility,
// shadows, and direct lighting all dissolve consistently instead of a
// half-dissolved mesh casting a full-strength ghost shadow.
//
// Dissolve math mirrors the legacy solid_z.frag.fsl (noise reshape per
// material variant, threshold at the z-pass ALPHA_REJECT 0.5) with one
// deviation: the noise is sampled in UV space (v.texC, wrap) instead of
// legacy screen space — RayQuery callers have no pixel coordinate, and a
// UV-anchored pattern sticks to the object instead of crawling with the
// camera. `dissolveAmount` is UniformObject.dissolveAmount
// (GetCoverageAmount); pass 1.0 semantics come from the object, not a
// default, so every caller fetches it. NOTE: rays only reach this for
// non-FORCE_OPAQUE instances — cHybridRenderer drops that flag while
// coverage < 1 or the material carries dissolve variants.
//
// The level is fixed at absolute 0 for every context by construction — there is
// no caller knob, on purpose. This gate decides which geometry EXISTS, not how
// it looks, and the three contexts above must agree on that answer, so a mip
// selected from one context's footprint cannot be allowed in. Three further
// reasons it stays put:
//  - Runtime mip chains are not dependable here. BitmapLoaderDevilMisc.cpp
//    forces lNumOfMipMaps = 1 for every non-DDS image, and Texture.cpp takes
//    mipLevels = options.use_mipmaps ? bitmap.GetNumOfMipMaps() : 1, so a
//    TGA/PNG mask has one level while a mipped DDS mask has many — a mipped
//    gate would act on some assets and be a no-op on others.
//  - Nothing generates alpha-coverage-preserving mips, so a fixed 0.5 test on
//    box-filtered alpha erodes thin cutouts (foliage, grates, chains) with
//    distance, taking their shadows and dissolve timing with them.
//  - A footprint-derived level drags gPerFrame.materialMipBias and the temporal
//    render extent in, making a silhouette depend on render resolution.
// notes/ray_cone_texture_lod.md carries the full argument and what would have to
// change to revisit it.
static const float kAlphaTestLod = 0.0f;

public bool materialAlphaTest(VertexData v, uint materialID, float dissolveAmount)
{
    // One blob load; `type` is at offset 0 for every material struct, so
    // reinterpreting as DiffuseMaterial reads the discriminator regardless.
    DiffuseMaterial m = loadDiffuseMaterial(materialID);
    if (m.type != MATERIAL_TYPE_DIFFUSE) return false;  // only diffuse/decal alpha-test

    float a = 1.0f;     // plain solids dissolve from full alpha
    uint alphaIdx = m.tex[2];
    if (alphaIdx != kInvalidTextureIndex)
    {
        float4 s = sampleBindless2DLevel(alphaIdx, v.texC, kAlphaTestLod);
        a = (m.materialConfig & kMaterialFlagIsAlphaSingleChannel) != 0u ? s.r : s.a;
    }
    else if (dissolveAmount >= 1.0f &&
             (m.materialConfig & (kMaterialFlagEnableDissolveAlpha
                                | kMaterialFlagUseDissolveFilter)) == 0u)
    {
        return false;   // plain solid at full coverage — cheap keep
    }

    bool hasDissolveAlpha  = (m.materialConfig & kMaterialFlagEnableDissolveAlpha) != 0u;
    bool hasDissolveFilter = (m.materialConfig & kMaterialFlagUseDissolveFilter)  != 0u;
    if (dissolveAmount < 1.0f || hasDissolveAlpha || hasDissolveFilter)
    {
        float d = gDissolveMap.SampleLevel(gMaterialSampler, v.texC, 0.f).x;
        if (hasDissolveAlpha)
        {
            // Combine the global noise with the material's DissolveAlpha
            // map (tex[6]): noise squeezed to [0.75,1], map subtracts up
            // to 0.25 — matches the legacy HasDissolveAlpha branch.
            d = d * 0.25f + 0.75f;
            uint dat = m.tex[6];
            float da = 1.0f;
            if (dat != kInvalidTextureIndex)
                da = sampleBindless2DLevel(dat, v.texC, kAlphaTestLod).r;
            d -= (0.25f - da * 0.25f);
        }
        else
        {
            d = d * 0.5f + 0.5f;
        }
        a = d - (1.0f - dissolveAmount * a) * 0.5f;
    }
    return a < 0.5f;
}
