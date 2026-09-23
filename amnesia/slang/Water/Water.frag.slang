/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Raster water surface — fragment stage. The underwater background is already
// in the pogo (the composite-shaded scene behind the water, with NO refraction
// bend applied to it). This pass adds the surface on top in TWO draws
// (hardware blend can't tint-dst and add reflection in one):
//   pass 0 (MUL blend): output surfaceTint·refractionExposure → dst *= that
//                       (tints + lifts the refracted bottom, matching the old
//                        surfaceTint · (refr·kWaterRefractionExposure)).
//   pass 1 (ADD blend): wave normal + Fresnel·fade × a denoised half-resolution
//                       reflection sampled from WaterReflectionPass before this draw
//                       → dst += reflColor·fresnel.
// Combined: surfaceTint·refr + reflColor·fresnel — the old water combine.
//
// The two draws are issued per mesh and INTERLEAVED (obj0 MUL, obj0 ADD, obj1
// MUL, obj1 ADD, ...). That interleaving is required, not incidental. With the
// surfaces drawn back-to-front it yields a correct nested over-composite; for
// two surfaces the target ends up as
//   bg·M0·M1 + A0·M1 + A1,   M_i = tint_i·exposure·visibility_i
//                            A_i = refl_i·fresnel_i·visibility_i + fogColor_i
// so the far surface's own reflection and fog colour are themselves tinted and
// fogged by the near surface they are seen through. The pass is therefore
// ORDER-DEPENDENT: the renderer sorts water meshes back-to-front before issuing
// them (cHybridRenderer::Draw). Hoisting all MUL passes ahead of all ADD passes
// would break this — the far surface's reflection and fog would come out
// unattenuated by the near surface.
//
// Fog is not double-applied across overlapping surfaces: each surface
// contributes its own fogColor exactly once, at its own layer. In the full-fog
// limit every visibility_i → 0, so every M_i → 0 and each A_i → fogColor_i, and
// the nesting telescopes to the nearest surface's fogColor — exactly fogColor,
// for any number of surfaces.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Material;             // loadWaterMaterial
import bindless;
import PerFrame.resource;
import Fog;                  // computeFog — fade the water surface into world/area fog
import Water.WaterSurface;
import Water.WaterReflectionSample;

[vk::binding(0, 2)] Texture2D<float4> gWaterReflectionSpecular;
[vk::binding(1, 2)] Texture2D<float4> gWaterReflectionGuidePosViewZ;
[vk::binding(2, 2)] Texture2D<float4> gWaterReflectionGuideNormalWeight;

struct WaterPC
{
    uint pass;   // 0 = tint (MUL), 1 = reflection (ADD), 2 = particle occlusion depth
    uint reflectionAvailable;
    uint _p1;
    uint _p2;
};
[vk::push_constant] WaterPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct WaterVSOut
{
    [vk::location(0)] float3 posW                    : TEXCOORD0;
    [vk::location(1)] float3 normalW                 : TEXCOORD1;
    [vk::location(2)] float3 tangentW                : TEXCOORD2;
    [vk::location(3)] float2 uv                      : TEXCOORD3;
    [vk::location(4)] nointerpolation uint objectId  : TEXCOORD4;
    float4 posH                    : SV_POSITION;
};

[shader("fragment")]
float4 psMain(WaterVSOut psIn) : SV_TARGET
{
    // Independent of reflection validity, Fresnel, fog and wave shading.
    // MIN blending records the nearest rasterized water surface per pixel.
    if (gPushConstants.pass == 2u)
        return float4(-mul(gPerFrame.viewMat, float4(psIn.posW, 1.0f)).z, 0, 0, 0);

    float2 dBaseUvDx = ddx(psIn.uv);
    float2 dBaseUvDy = ddy(psIn.uv);
    float3 dpdx = ddx(psIn.posW);
    float3 dpdy = ddy(psIn.posW);
    UniformObject obj   = gSceneObjects[psIn.objectId];
    WaterMaterial water = loadWaterMaterial(obj.materialID);

    // World + area fog at the water surface. We fog the water exactly like the
    // opaque composite does (applyFog: dst = visibility·color + fogColor), just
    // split across the two hardware blend passes (MUL then ADD):
    //   pass 0 MUL: dst *= tint·exp·visibility  -> refr·tint·exp·visibility
    //   pass 1 ADD: dst += reflColor·fresnel·visibility + fog.color
    //   sum = visibility·(refr·tint·exp + reflColor·fresnel) + fogColor.
    // At full fog visibility→0 so the surface collapses to fogColor and the water
    // dissolves into the fog like everything else, regardless of how shallow /
    // weakly-fogged the refracted background is.
    FogResult fog = computeFog(psIn.posW, gPerFrame.posW);

    if (gPushConstants.pass == 0u)
    {
        // Tint (+ refraction-exposure lift) of the refracted background (MUL),
        // faded to zero as fog thickens.
        float3 tint = float3(1.f);
        uint   diffuseTex = water.tex[0];
        if (diffuseTex != kInvalidTextureIndex)
            tint = sampleBindless2D(diffuseTex, psIn.uv).rgb;
        return float4(tint * kWaterRefractionExposure * fog.visibility, 1.0f);
    }

    // pass 1 — Fresnel-weighted lit reflection (ADD). The derivative inputs
    // were captured before fog and pass control flow above.
    // Category (a): this view vector is formed from the world position
    // interpolated at the jittered full-resolution raster sample, not from a
    // pixel-index ray reconstruction; the raster sample already carries the
    // primary-sample alignment.
    float3 eye  = normalize(psIn.posW - gPerFrame.posW); // camera -> surface
    WaterSurfaceFrame frame = waterSurfaceFrame(dpdx, dpdy, eye);
    float3 waveN = waterWaveNormalWorld(water, psIn.uv,
                                        dBaseUvDx, dBaseUvDy,
                                        gPerFrame.afT, frame, eye);
    float fresnel = waterFresnelTerm(water, eye, waveN);

    // Distance fade: fade the reflection out with view depth. Only when a real
    // fade range is authored — without this guard a 0 reflectionFadeEnd makes
    // fadeLen ~1e-6 and saturate()→1, which zeroes the reflection everywhere.
    float viewZ = -mul(gPerFrame.viewMat, float4(psIn.posW, 1.0f)).z;
    fresnel *= waterDistanceFade(water, viewZ);

    // Reflection normal: calmer than refraction (blend wave normal toward flat).
    float3 reflN = waterReflectionNormal(frame, waveN);

    float3 sampledReflection = float3(0.0f);
    if (gPushConstants.reflectionAvailable != 0u)
    {
        WaterReflectionSample s = sampleWaterReflectionHalfRes(
            gWaterReflectionSpecular,
            gWaterReflectionGuidePosViewZ,
            gWaterReflectionGuideNormalWeight,
            uint2(psIn.posH.xy),
            psIn.posW,
            frame.N,
            reflN);
        if (s.valid)
            sampledReflection = s.radiance;
    }

    // Fade the reflection by fog visibility, then add the fog colour exactly
    // once (the MUL pass already zeroed the surface at full fog) so the water
    // composites to the authored fog color in the distance. Cancel exposure
    // on the fog contribution because computeFog returns authored linear color.
    // Alpha 0: this blend is additive in alpha too (srcAlpha=ONE, dstAlpha=ONE),
    // so 0 is the identity that preserves the a = 1 the composite wrote.
    return float4(sampledReflection * kWaterReflectionExposure * fresnel * fog.visibility
                  + fog.color / kSceneExposure, 0.0f);
}
