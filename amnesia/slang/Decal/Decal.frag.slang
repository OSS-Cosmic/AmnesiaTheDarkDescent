/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/decal.frag.fsl, reworked for
// the V-buffer renderer. Type="Decal" meshes (dirt/moist/pool/trails, and the
// editor's projected decal geometry) are Mul/MulX2/Add: RGB transparency is the
// blend-mode identity, and alpha only feeds the legacy 0.01 alpha test
// (kDecalAlphaReject) that cuts the decal's shape. The decal pre-pass rasterizes them into two accumulators
// (decalMul, cleared white; decalAdd, cleared black) and MainCompositePass
// applies albedo = albedo*decalMul + decalAdd before lighting, so the decals are
// lit + fogged like the surface they sit on.
//
// This shader just emits the LINEAR decal colour; the hardware blend mode of the
// bound pipeline (BLEND_MUL / BLEND_MULX2 into decalMul, BLEND_ADD into decalAdd)
// does the accumulation. Multiplying linear albedo by the linear decal colour is
// exactly equivalent to the legacy gamma-space multiply via the power law
// (a·b)^γ = a^γ·b^γ, so Mul decals match the original; Add is a close linear
// approximation. White (Mul) / black (Add) texels are the blend identity, but
// some textures (dirt_coal) carry uniformly dark RGB with the shape only in
// alpha, so the alpha test is required; a discarded texel leaves the
// accumulator at its cleared identity.

#include "../Constants.h"

import bindless;
import SceneTypes;
import Scene;
import Material;                 // loadDiffuseMaterial / sampleBindless2D
import Utils.Color.ColorHelpers; // linearToSRGB

struct DecalVSOut
{
    [vk::location(0)] float2 uv                      : TEXCOORD0;
    [vk::location(1)] float4 color                   : TEXCOORD1;
    [vk::location(2)] nointerpolation uint objectId  : TEXCOORD2;
    float4 posH                    : SV_POSITION;
};

[shader("fragment")]
float4 psMain(DecalVSOut psIn) : SV_TARGET
{
    UniformObject   obj = gSceneObjects[psIn.objectId];
    DiffuseMaterial mat = loadDiffuseMaterial(obj.materialID);

    // Emit the DISPLAY-space (gamma) decal colour: the accumulators + the
    // composite blend run in gamma to match the legacy diffuse G-buffer, so a
    // MulX2 web's authored 0.5-gray neutral reaches the 2·src·dst blender as 0.5
    // (2×0.5=1, no change) instead of darkening as a linear 0.214 would. The
    // diffuse comes through an sRGB view (linear) -> re-encode to gamma. Missing
    // texture falls back to gamma white = Mul/MulX2 identity.
    float4 texel = float4(1.0f);
    uint   diffuseTex = mat.tex[0];
    if (diffuseTex != kInvalidTextureIndex)
        texel = sampleBindless2D(diffuseTex, psIn.uv);
    // Legacy alpha test on texture * vertex colour, as RenderDecals did.
    if (texel.a * psIn.color.a < kDecalAlphaReject)
        discard;
    float3 c = linearToSRGB(texel.rgb);
    c *= psIn.color.rgb; // authored (gamma) vertex tint

    return float4(c, 1.0f);
}
